# runtime-web-lib.sh — Vault-only webchat migration + launch helpers.
# POSIX sh; sourced by the root server entrypoint.

WEBCHAT_PORT="${AIMEE_WEBCHAT_PORT:-8443}"
WEBCHAT_HOME="${AIMEE_HOME:-/var/lib/aimee}"
WEBCHAT_SPA="${AIMEE_WEBCHAT_SPA:-/usr/local/share/aimee-runtime-web/index.html}"
WEBCHAT_BOOTSTRAP_REPLACED="${WEBCHAT_HOME}/webchat/bootstrap-replaced"
WEBCHAT_BOOTSTRAP_USER="${WEBCHAT_HOME}/webchat/bootstrap-user"
WEBCHAT_BOOTSTRAP_CREDENTIALS="${WEBCHAT_HOME}/webchat/bootstrap-credentials"
# "<user>:<shadow verifier>" per line, written by runtime-web after every account
# mutation so the logins survive the container being replaced. Must match
# identityRecordPath() in runtime-web/identity_persist.go.
WEBCHAT_IDENTITIES="${WEBCHAT_HOME}/webchat/identities"
WEBCHAT_LEGACY_TLS_KEY="${WEBCHAT_HOME}/webchat.key"
# Dashboard logins are local PAM accounts, scoped to this group so runtime-web
# can only see and manage the logins it provisioned — never the container's own
# system users. Must match webchatLoginGroup in runtime-web.
WEBCHAT_LOGIN_GROUP="${AIMEE_WEBCHAT_LOGIN_GROUP:-aimee-webchat}"
webchat_pid=""
WEBCHAT_PREPARED=0

# Diagnostics go to stderr, never stdout. The entrypoint execs an arbitrary
# command override (`docker run ... sh -c ...`), and anything this library prints
# to stdout is prepended to that command's own output — corrupting it for any
# caller capturing it. Container runtimes collect both streams as logs, so this
# costs nothing operationally.
webchat_log() { printf '[webchat] %s\n' "$*" >&2; }

webchat_is_enabled() {
    case "$(printf '%s' "${AIMEE_RUNTIME_WEB_ENABLED:-1}" | tr 'A-Z' 'a-z')" in
        0 | false | no | off) return 1 ;;
        *) return 0 ;;
    esac
}

webchat_remove_legacy_file() {
    _rf_path="$1"
    [ -f "$_rf_path" ] || return 0
    if command -v shred >/dev/null 2>&1; then
        shred -u -n 1 -z -- "$_rf_path" 2>/dev/null || rm -f -- "$_rf_path"
    else
        rm -f -- "$_rf_path"
    fi
}

webchat_read_generated_credentials() {
    wc_generated_user=""
    wc_generated_pass=""
    [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ] || return 1
    [ "$(grep -c '^username=' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" 2>/dev/null || true)" -eq 1 ] || return 1
    [ "$(grep -c '^password=' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" 2>/dev/null || true)" -eq 1 ] || return 1
    [ "$(awk 'NF { n++ } END { print n + 0 }' "$WEBCHAT_BOOTSTRAP_CREDENTIALS")" -eq 2 ] || return 1
    wc_generated_user="$(sed -n 's/^username=//p' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" | head -n 1)"
    wc_generated_pass="$(sed -n 's/^password=//p' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" | head -n 1)"
    case "$wc_generated_user" in
        aimee-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) : ;;
        *) return 1 ;;
    esac
    case "$wc_generated_pass" in *[!0-9a-f]* | '') return 1 ;; esac
    [ "${#wc_generated_pass}" -eq 64 ]
}

# The helper accepts only a closed record-name allowlist. Secret bytes travel on
# stdin to a short-lived process running as the Vault owner.
webchat_seal_record() {
    _sr_name="$1"
    runuser -u aimee -- aimee-server --webchat-vault-seal "$_sr_name"
}

# Retire legacy credential FILES. Logins themselves are not migrated anywhere:
# a PAM verifier lives in shadow, and shadow is the source of truth.
#
# This used to seal the bootstrap login and the PAM verifier registry into the
# Vault and then erase the shadow entries behind them. The Vault holds aimee's
# own secrets — the session HMAC, the TLS key, provider credentials — and a host
# password is not one of those. Sealing it built a second identity system, and
# erasing shadow afterwards deleted the credential the browser actually
# authenticates with, which is how an upgrade could lock an operator out of
# their own appliance.
#
# What is still removed is the PLAINTEXT bootstrap file, and only after the
# account has been provisioned from it, so the password never outlives its use.
# The TLS key IS aimee's own secret, so it still goes to the Vault.
webchat_migrate_legacy_credentials() {
    if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ] && ! webchat_read_generated_credentials; then
        webchat_log "ERROR: legacy bootstrap credential file is invalid; refusing to delete unknown authentication state"
        return 1
    fi
    # Provision BEFORE deleting the plaintext source: the generated password is
    # the only copy, so losing it before the account exists is unrecoverable.
    webchat_provision_bootstrap_account || return 1
    if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
        webchat_remove_legacy_file "$WEBCHAT_BOOTSTRAP_CREDENTIALS" || return 1
        webchat_log "removed the legacy plaintext bootstrap login"
    fi
    if [ -f "$WEBCHAT_LEGACY_TLS_KEY" ]; then
        if ! webchat_seal_record tls_key < "$WEBCHAT_LEGACY_TLS_KEY"; then
            webchat_log "ERROR: could not seal the legacy TLS private key into Vault"
            return 1
        fi
        webchat_remove_legacy_file "$WEBCHAT_LEGACY_TLS_KEY" || return 1
        webchat_log "sealed and removed the legacy TLS private key"
    fi
    wc_generated_user="" wc_generated_pass=""
}

# Provision the first-boot dashboard login as a REAL system account.
#
# This is the wizard's way in: on first boot there is no identity yet, so PAM has
# nobody to authenticate. The appliance used to solve that by sealing a
# credential into the Vault, which worked but created a second identity system
# that never handed back to PAM — an appliance stayed on bootstrap-shaped
# credentials permanently. Creating a real account keeps the bootstrap idea and
# drops the parallel store: PAM has something to authenticate from the first
# login onward, and replacing it later is an ordinary account change.
#
# Idempotent: an existing account keeps its password, so a restart never resets a
# credential the operator has already changed.
# The managed login group, created on demand. Both the restore path and the
# provision path need it before their first usermod, and each having its own copy
# is how one of them ended up without it.
webchat_ensure_login_group() {
    getent group "$WEBCHAT_LOGIN_GROUP" >/dev/null 2>&1 && return 0
    if ! groupadd --system "$WEBCHAT_LOGIN_GROUP"; then
        webchat_log "ERROR: could not create the managed login group"
        return 1
    fi
    return 0
}

webchat_provision_login() {
    _bs_user="$1"
    _bs_pass="$2"
    [ -n "$_bs_user" ] && [ -n "$_bs_pass" ] || return 0

    webchat_ensure_login_group || return 1
    if id "$_bs_user" >/dev/null 2>&1; then
        usermod -aG "$WEBCHAT_LOGIN_GROUP" "$_bs_user" 2>/dev/null || true
        # An account can exist WITHOUT being able to log in: the image ships
        # `aimee` as the service account with a locked verifier ('!' or '*'), and
        # the documented compose names that same user as the dashboard login. Set
        # the password only in that case — a real verifier means the operator (or
        # an earlier boot) already owns it, and resetting it every boot would undo
        # their own change.
        _bs_hash="$(getent shadow "$_bs_user" 2>/dev/null | cut -d: -f2)"
        case "$_bs_hash" in
            '' | '!'* | '*'*)
                if ! printf '%s:%s' "$_bs_user" "$_bs_pass" | chpasswd; then
                    webchat_log "ERROR: could not set the first-boot dashboard password"
                    _bs_user="" _bs_pass="" _bs_hash=""
                    return 1
                fi
                webchat_log "enabled the first-boot dashboard login '$_bs_user' (local PAM)"
                ;;
        esac
        _bs_user="" _bs_pass="" _bs_hash=""
        return 0
    fi
    if ! useradd --system --no-create-home --shell /usr/sbin/nologin \
                 --gid "$WEBCHAT_LOGIN_GROUP" "$_bs_user" 2>/dev/null; then
        webchat_log "ERROR: could not create the first-boot dashboard login"
        _bs_user="" _bs_pass=""
        return 1
    fi
    # ALSO a supplementary member, not just primary. `getent group` lists only
    # supplementary members, so a primary-only account is invisible to
    # UserManager.List() — the dashboard's user list came back empty while login
    # worked, because IsManagedUser reads the USER's groups and List reads the
    # GROUP's members. The two must agree.
    usermod -aG "$WEBCHAT_LOGIN_GROUP" "$_bs_user" 2>/dev/null || true
    if ! printf '%s:%s' "$_bs_user" "$_bs_pass" | chpasswd; then
        webchat_log "ERROR: could not set the first-boot dashboard password"
        _bs_user="" _bs_pass=""
        return 1
    fi
    webchat_log "provisioned the first-boot dashboard login '$_bs_user' (local PAM)"
    _bs_user="" _bs_pass=""
}

# Provision EVERY first-boot login the appliance carries, not just the preferred
# one. An explicit AIMEE_WEBCHAT_USER pair and a generated bootstrap-credentials
# file can both exist, and the generated account may be the one the operator is
# currently signed in with — provisioning only the explicit pair and then
# deleting the generated file would destroy a working login. Both become PAM
# accounts; the wizard retires the generated one when it is replaced.
# Never fatal, for the reason spelled out in webchat_generate_bootstrap_login:
# the entrypoint calls this unguarded under `set -eu`, so a failure here would
# take the container down instead of leaving it up without a browser login.
# The operator-supplied first-boot pair, from wherever it survived to here.
#
# PAM owns user control; env vars only PRE-SEED it on first boot. Reading the
# environment alone found nothing on either supported path: the entrypoint
# scrubs the credential env names into Vault before provisioning runs, and the
# managed compose deliberately never places them in the long-lived container's
# environment at all. Both paths leave the values in the Vault records the
# bootstrap seals, so recover them from there when the environment is empty.
#
# That is TRANSPORT, not a second identity store: the PAM account created from
# these is the source of truth from the first login onward, which is the whole
# point of webchat_provision_login above.
webchat_read_seeded_credentials() {
    wc_seed_user="${AIMEE_WEBCHAT_USER:-}"
    wc_seed_pass="${AIMEE_WEBCHAT_PASSWORD:-}"
    [ -n "$wc_seed_user" ] && [ -n "$wc_seed_pass" ] && return 0
    _ws_export=$(runuser -u aimee -- aimee-server --webchat-vault-export 2>/dev/null) || return 1
    wc_seed_user=$(printf '%s\n' "$_ws_export" | awk -F'\t' '$1=="user"{print $2}' |
        base64 -d 2>/dev/null) || wc_seed_user=""
    wc_seed_pass=$(printf '%s\n' "$_ws_export" | awk -F'\t' '$1=="password"{print $2}' |
        base64 -d 2>/dev/null) || wc_seed_pass=""
    _ws_export=""
    [ -n "$wc_seed_user" ] && [ -n "$wc_seed_pass" ]
}

# Recreate the managed logins recorded by a previous container.
#
# PAM identities live in the container's writable layer, so replacing the image
# destroys them, while $AIMEE_HOME survives -- including the operator's projects,
# which are filed by webuser NAME. Minting a fresh generated login stops the
# lockout but not this: a new random name leaves the whole project tree attached
# to a user nobody signs in as. runtime-web records the managed accounts and
# their shadow verifiers after every mutation (identity_persist.go); restore them
# here, before anything decides a new login is needed.
#
# Only accounts that are actually MISSING are touched. An account that survived
# keeps its current password: the record can be older than a password change the
# operator made since, and restoring over it would silently roll that back.
webchat_restore_identities() {
    [ -f "$WEBCHAT_IDENTITIES" ] || return 0
    # The group must exist BEFORE the first usermod. It ships in no image, and this
    # function runs first in webchat_provision_bootstrap_account -- ahead of
    # webchat_provision_login, which was the only thing that created it. So on every
    # container replacement each restored account failed its `usermod -aG`, silently
    # (|| true), and came back OUTSIDE the managed group.
    #
    # That is not cosmetic. UserManager.List() reads the GROUP, so an unmanaged account
    # is invisible to it, and snapshotManagedIdentities() rebuilds this record from that
    # list and writes it WHOLESALE. The next account operation therefore dropped the
    # restored account from the record, and the container replacement after that lost
    # the account for good -- observed on the testing appliance, where `admin` existed
    # only in this record (it is in no image) and was one snapshot away from deletion.
    webchat_ensure_login_group || return 0
    _wc_restored=0
    while IFS=: read -r _wc_u _wc_h; do
        [ -n "$_wc_u" ] && [ -n "$_wc_h" ] || continue
        # Mirror usableShadowHash() in identity_persist.go. A locked or disabled
        # verifier is not a login: restoring it would recreate an account nobody
        # can authenticate as, and its group membership would then suppress
        # minting a real one -- the lockout, rebuilt from the record.
        case "$_wc_h" in
            '!'* | '*'*) continue ;;
        esac
        getent passwd "$_wc_u" >/dev/null 2>&1 && continue
        if ! useradd --create-home --shell /usr/sbin/nologin "$_wc_u" >/dev/null 2>&1; then
            webchat_log "WARNING: could not restore the login '$_wc_u'"
            continue
        fi
        # -e: the field is already a hash, not a plaintext password.
        if ! printf '%s:%s\n' "$_wc_u" "$_wc_h" | chpasswd -e >/dev/null 2>&1; then
            webchat_log "WARNING: could not restore the verifier for '$_wc_u'"
            userdel -r "$_wc_u" >/dev/null 2>&1 || true
            continue
        fi
        # NOT `|| true`. An account outside the managed group is invisible to
        # UserManager.List(), so the next snapshot rewrites this record without it and
        # the following container replacement loses it. Swallowing this is what turned a
        # recoverable group failure into silent account deletion, so say so loudly.
        if ! usermod -aG "$WEBCHAT_LOGIN_GROUP" "$_wc_u" >/dev/null 2>&1; then
            webchat_log "WARNING: restored '$_wc_u' but could not add it to $WEBCHAT_LOGIN_GROUP;" \
                        "it is NOT managed and the next snapshot will drop it"
        fi
        _wc_restored=$((_wc_restored + 1))
    done < "$WEBCHAT_IDENTITIES"
    [ "$_wc_restored" -gt 0 ] && webchat_log "restored $_wc_restored dashboard login(s) after a container replacement"
    _wc_u="" _wc_h="" _wc_restored=""
    return 0
}

webchat_provision_bootstrap_account() {
    # Before anything asks whether a login is needed: put back the ones this
    # appliance already had. Without this an upgrade hands the operator a new
    # generated account while their projects stay filed under the old name.
    webchat_restore_identities
    if webchat_read_seeded_credentials; then
        webchat_provision_login "$wc_seed_user" "$wc_seed_pass" || true
    fi
    wc_seed_user="" wc_seed_pass=""
    if webchat_read_generated_credentials; then
        webchat_provision_login "$wc_generated_user" "$wc_generated_pass" || true
    fi
    webchat_generate_bootstrap_login || true
    return 0
}

# 0 when at least one member of the managed login group is an account a human can
# ACTUALLY sign in with: present in passwd, and holding a usable password hash.
#
# Membership alone does not mean that. Retiring the bootstrap account through the
# wizard locks its shadow entry ("!$y...") and leaves the name in the group, so a
# group with members can describe an account nobody can authenticate as.
webchat_has_usable_login() {
    _wc_members=$(getent group "$WEBCHAT_LOGIN_GROUP" 2>/dev/null | cut -d: -f4 | tr ',' ' ')
    for _wc_u in $_wc_members; do
        [ -n "$_wc_u" ] || continue
        getent passwd "$_wc_u" >/dev/null 2>&1 || continue
        _wc_h=$(getent shadow "$_wc_u" 2>/dev/null | cut -d: -f2)
        # Empty, "!"-locked, or "*"-disabled are all unusable for PAM auth.
        case "$_wc_h" in
            '' | '!'* | '*'*) continue ;;
        esac
        _wc_members="" _wc_u="" _wc_h=""
        return 0
    done
    _wc_members="" _wc_u="" _wc_h=""
    return 1
}

# 1 when this appliance still has no way for a human to sign in.
#
# An explicit pair wins and needs nothing generated. Otherwise the question is
# answered by what EXISTS, not by what a marker remembers.
#
# The markers used to answer it, and that locked operators out of upgraded
# appliances. PAM identities live in the container's writable layer -- /etc/passwd
# and /etc/shadow are on no volume -- while the markers live in $AIMEE_HOME, which
# is. So `docker compose up --force-recreate` onto a new image destroyed every
# account and kept the markers that say one exists, leaving the appliance with no
# login and no way to mint another. Reproduced on a running appliance: after an
# ordinary image upgrade the volume still recorded webchat/bootstrap-replaced =
# the operator's name while the container held exactly one uid>=1000 account, the
# service account. Nothing in the Vault could restore it either; only session_hmac
# and tls_key survive there, no account or hash.
#
# Regenerating still must not fire while a working login exists -- that would
# change the password out from under the operator on every restart -- which is
# what webchat_has_usable_login establishes.
webchat_bootstrap_login_needed() {
    # Same source as provisioning: an operator-supplied pair that reached us via
    # the Vault transport must suppress generation just as an environment one
    # does, or the appliance prints a random credential the operator never asked
    # for beside the account they did.
    if webchat_read_seeded_credentials; then
        wc_seed_user="" wc_seed_pass=""
        return 1
    fi
    wc_seed_user="" wc_seed_pass=""
    webchat_has_usable_login && return 1
    return 0
}

# Generate the first-boot dashboard login when the deployment supplied none, and
# PRINT IT to the container log.
#
# Without this an appliance deployed from a credential-free manifest has no way
# into its own wizard: the compose files deliberately carry no password (they are
# container metadata, readable by anyone who can inspect the service), and
# nothing else creates one. Measured on a clean install of this branch — server
# healthy, PAM service shipped, and no account a human could use.
#
# The log IS the delivery channel, which is the deliberate trade: a first-boot
# secret has to reach the operator somehow, and `docker logs` is the one place
# they can already read without an account. It is printed once, on the boot that
# creates it, and never persisted in plaintext — the marker written afterwards
# records only the NAME, so the wizard knows this login is a temporary one to
# replace, without keeping the secret at rest.
webchat_generate_bootstrap_login() {
    webchat_bootstrap_login_needed || return 0

    # aimee-<12 hex> / 64 hex: the shape readGeneratedBootstrapUsername and
    # pendingBootstrapUsername already recognise as a retirable generated login.
    _gen_user="aimee-$(od -An -N6 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
    _gen_pass="$(od -An -N32 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
    if [ "${#_gen_user}" -ne 18 ] || [ "${#_gen_pass}" -ne 64 ]; then
        webchat_log "ERROR: could not draw a first-boot dashboard credential"
        _gen_user="" _gen_pass=""
        return 0
    fi
    if ! webchat_provision_login "$_gen_user" "$_gen_pass"; then
        # NOT fatal. The entrypoint runs under `set -eu` and does not guard this
        # call, so returning non-zero here aborts the whole container — a host
        # without useradd/groupadd would stop booting rather than merely lacking
        # a dashboard login. Say so loudly and carry on degraded.
        webchat_log "ERROR: could not provision a first-boot dashboard login; the"
        webchat_log "browser UI will have no account until one is supplied via"
        webchat_log "AIMEE_WEBCHAT_USER/AIMEE_WEBCHAT_PASSWORD."
        _gen_user="" _gen_pass=""
        return 0
    fi
    mkdir -p "$(dirname "$WEBCHAT_BOOTSTRAP_USER")" 2>/dev/null || true
    # Reaching here means no usable login existed, so any surviving marker is
    # stale -- it names an account this container no longer has. Left in place,
    # bootstrap-replaced would tell the wizard setup is already finished while the
    # only working credential is the one being printed below.
    rm -f "$WEBCHAT_BOOTSTRAP_REPLACED" 2>/dev/null || true
    printf 'generated:%s\n' "$_gen_user" > "$WEBCHAT_BOOTSTRAP_USER" 2>/dev/null || {
        webchat_log "ERROR: could not record the generated first-boot login"
        _gen_user="" _gen_pass=""
        return 0
    }
    # 0644, not 0600: the C server runs as `aimee` and resolves the appliance
    # administrator from this marker. Unreadable, its gate falls back to "admin"
    # and refuses the very operator this file names. The content is a username.
    chmod 644 "$WEBCHAT_BOOTSTRAP_USER" 2>/dev/null || true

    webchat_log "======================================================================"
    webchat_log "FIRST-BOOT DASHBOARD LOGIN (shown once — copy it now)"
    webchat_log "    username: $_gen_user"
    webchat_log "    password: $_gen_pass"
    webchat_log "Sign in at https://<host>:$WEBCHAT_PORT and replace this account in"
    webchat_log "the wizard. Supply AIMEE_WEBCHAT_USER/AIMEE_WEBCHAT_PASSWORD at"
    webchat_log "deploy time to choose your own instead."
    webchat_log "======================================================================"
    _gen_user="" _gen_pass=""
}

webchat_prepare() {
    # Migration is unconditional. Disabling the browser surface must not leave
    # credentials from an older image sitting on the data volume or in shadow.
    webchat_migrate_legacy_credentials || return 1
    if ! webchat_is_enabled; then
        webchat_log "AIMEE_RUNTIME_WEB_ENABLED=0; browser UI disabled by operator"
        WEBCHAT_PREPARED=1
        return 0
    fi
    if ! command -v aimee-runtime-web >/dev/null 2>&1; then
        webchat_log "aimee-runtime-web not present (image built without WITH_RUNTIME_WEB); skipping browser UI"
        WEBCHAT_PREPARED=1
        return 0
    fi
    # The first-boot account is provisioned by webchat_migrate_legacy_credentials
    # above, before the plaintext source is removed. Nothing to gate here: logins
    # are PAM accounts, so there is no Vault record to require.
    # server.token was a persistent shared bearer. UDS peer credentials replaced
    # it; erase any legacy copy before starting services.
    webchat_remove_legacy_file "$WEBCHAT_HOME/server.token" || return 1
    WEBCHAT_PREPARED=1
}

webchat_start() {
    [ "$WEBCHAT_PREPARED" -eq 1 ] || webchat_prepare
    webchat_is_enabled || return 0
    command -v aimee-runtime-web >/dev/null 2>&1 || return 0
    if [ -n "${AIMEE_WEBCHAT_TLS_KEY:-}" ]; then
        webchat_log "ERROR: TLS private-key files are forbidden; import first-boot key material into Vault"
        return 1
    fi
    _wc_cert=""
    if [ -n "${AIMEE_WEBCHAT_TLS_CERT:-}" ]; then
        _wc_cert="--cert $AIMEE_WEBCHAT_TLS_CERT"
        webchat_log "using operator TLS certificate $AIMEE_WEBCHAT_TLS_CERT with its Vault-held key"
    fi
    webchat_log "starting Vault-authenticated aimee-runtime-web on :$WEBCHAT_PORT"
    # shellcheck disable=SC2086
    HOME=/root AIMEE_HOME="$WEBCHAT_HOME" aimee-runtime-web \
        --port "$WEBCHAT_PORT" \
        --socket "$WEBCHAT_HOME/aimee-server.sock" \
        --db "$WEBCHAT_HOME/webchat.db" \
        --spa "$WEBCHAT_SPA" \
        $_wc_cert &
    webchat_pid=$!
}

webchat_stop() {
    [ -n "$webchat_pid" ] && kill "$webchat_pid" 2>/dev/null || true
}
