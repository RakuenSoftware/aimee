# webchat-lib.sh — POSIX sh helpers to bootstrap + launch aimee-runtime-web inside a
# container. Sourced (not executed) by the image entrypoints, which run as root.
#
# aimee-runtime-web is the Go browser service. It is co-located with aimee-server in
# the same image and shares AIMEE_HOME, so its Unix-socket transports line up
# automatically:
#   - chat stream + dashboard RPC  -> $AIMEE_HOME/aimee-http.sock  (server /v1 UDS)
#   - legacy NDJSON fallback        -> $AIMEE_HOME/aimee-server.sock
#   - OpenAI-proxy bearer            -> $AIMEE_HOME/server.token
# Passing --socket "$AIMEE_HOME/aimee-server.sock" makes webchat derive all three
# from that directory.
#
# PAM (pam_unix) reads /etc/shadow, so webchat must run as root; the entrypoint
# therefore starts as root, runs webchat here, and drops aimee-server/kb to the
# unprivileged "aimee" user separately.

WEBCHAT_PORT="${AIMEE_WEBCHAT_PORT:-8443}"
WEBCHAT_HOME="${AIMEE_HOME:-/var/lib/aimee}"
WEBCHAT_SPA="${AIMEE_WEBCHAT_SPA:-/usr/local/share/aimee-runtime-web/index.html}"
webchat_pid=""

webchat_log() { printf '[webchat] %s\n' "$*"; }

# webchat scopes its user-management surface to a named OS group (it constructs
# auth.NewUserManager("aimee")). Adding the bootstrap account to that group makes
# it visible/manageable in the dashboard; login itself is PAM-only and does not
# require the group.
WEBCHAT_GROUP="${AIMEE_WEBCHAT_GROUP:-aimee}"

# Durable mirror of the provisioned PAM logins, kept on the persistent AIMEE_HOME
# volume. The container's /etc/passwd + /etc/shadow live ONLY in the ephemeral
# container filesystem, so anything that starts the container from a fresh rootfs
# — an image update, a `docker rm`/recreate, or a runtime (e.g. the SmoothNAS
# appliance) that re-creates the container on every host reboot — wipes every PAM
# account, and browser login breaks until someone manually re-runs `useradd`.
# We therefore record each provisioned account here (username + its crypt(3) shadow
# hash, NEVER a plaintext password) and restore any missing account from it on every
# start. $AIMEE_HOME IS a persistent volume, so the login survives reboots even when
# the env that originally created it is no longer injected. Root-owned, 0600.
WEBCHAT_LOGIN_STORE="${WEBCHAT_HOME}/webchat/logins"
# Written by POST /api/setup/account after the onboarding wizard creates a real
# operator login. The image still contains the unprivileged `aimee` service UID,
# but this marker prevents a temporary generated login from returning after a
# container recreate.
WEBCHAT_BOOTSTRAP_REPLACED="${WEBCHAT_HOME}/webchat/bootstrap-replaced"
# Root-only plaintext for the temporary login generated when neither primary
# credential env var is set. Keeping it on AIMEE_HOME makes a recreate print the
# SAME usable credential instead of silently rotating it. POST /api/setup/account
# deletes it transactionally when the operator creates their permanent account.
WEBCHAT_BOOTSTRAP_CREDENTIALS="${WEBCHAT_HOME}/webchat/bootstrap-credentials"

# Record (or replace) the "user:hash" line for $1 in the durable store, reading the
# crypt hash straight from /etc/shadow so we persist the encrypted form, not the
# plaintext. Skips locked/empty hashes ('!'/'*'/'') so an unusable account is not
# mirrored. Best-effort: a persistence failure must never break provisioning.
webchat_persist_login() {
    _pl_user="$1"
    [ -n "$_pl_user" ] || return 0
    _pl_hash="$(getent shadow "$_pl_user" 2>/dev/null | cut -d: -f2)"
    case "$_pl_hash" in
        '' | '!'* | '*'*) return 0 ;;
    esac
    mkdir -p "$(dirname "$WEBCHAT_LOGIN_STORE")" 2>/dev/null || return 0
    _pl_umask_old="$(umask)"
    umask 077
    _pl_tmp="${WEBCHAT_LOGIN_STORE}.tmp.$$"
    if [ -f "$WEBCHAT_LOGIN_STORE" ]; then
        if ! awk -F: -v user="$_pl_user" '$1 != user { print }' \
            "$WEBCHAT_LOGIN_STORE" > "$_pl_tmp" 2>/dev/null; then
            rm -f "$_pl_tmp"
            umask "$_pl_umask_old"
            return 0
        fi
    else
        : > "$_pl_tmp"
    fi
    printf '%s:%s\n' "$_pl_user" "$_pl_hash" >> "$_pl_tmp"
    mv -f "$_pl_tmp" "$WEBCHAT_LOGIN_STORE" 2>/dev/null || rm -f "$_pl_tmp"
    umask "$_pl_umask_old"
    chmod 600 "$WEBCHAT_LOGIN_STORE" 2>/dev/null || true
}

# Remove a retired bootstrap login from both the durable mirror and the live PAM
# database. The image's `aimee` service UID is retained, but its password is
# locked. This is also the migration path from the old published aimee password.
webchat_retire_login() {
    _rt_user="$1"
    [ -n "$_rt_user" ] || return 0
    if [ -f "$WEBCHAT_LOGIN_STORE" ]; then
        _rt_tmp="${WEBCHAT_LOGIN_STORE}.tmp.$$"
        if ! awk -F: -v user="$_rt_user" '$1 != user { print }' \
            "$WEBCHAT_LOGIN_STORE" > "$_rt_tmp" 2>/dev/null; then
            rm -f "$_rt_tmp"
            webchat_log "ERROR: could not read the persistent login store while retiring '$_rt_user'"
            return 1
        fi
        if ! mv -f "$_rt_tmp" "$WEBCHAT_LOGIN_STORE"; then
            rm -f "$_rt_tmp"
            webchat_log "ERROR: could not retire login '$_rt_user' from the persistent login store"
            return 1
        fi
        chmod 600 "$WEBCHAT_LOGIN_STORE" 2>/dev/null || true
    fi
    if id "$_rt_user" >/dev/null 2>&1; then
        if ! usermod --lock "$_rt_user" 2>/dev/null; then
            webchat_log "ERROR: could not lock retired bootstrap login '$_rt_user'"
            return 1
        fi
    fi
}

webchat_random_hex() {
    _rh_bytes="$1"
    _rh_value=""
    if command -v openssl >/dev/null 2>&1; then
        _rh_value="$(openssl rand -hex "$_rh_bytes" 2>/dev/null)"
    fi
    if [ -z "$_rh_value" ] && [ -r /dev/urandom ]; then
        _rh_value="$(head -c "$_rh_bytes" /dev/urandom | od -An -tx1 | tr -d ' \n')"
    fi
    [ -n "$_rh_value" ] || return 1
    printf '%s\n' "$_rh_value"
}

# Load the generated credential into wc_generated_user/wc_generated_pass.
# The format is intentionally tiny and shell-readable; generated fields are
# lowercase hex (with an aimee- prefix on the username), so no escaping exists.
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
    case "$wc_generated_pass" in
        *[!0-9a-f]* | '') return 1 ;;
    esac
    [ "${#wc_generated_pass}" -eq 64 ] || return 1
}

webchat_generate_credentials() {
    wc_generated_user=""
    wc_generated_pass=""
    _gc_tries=0
    while [ "$_gc_tries" -lt 10 ]; do
        _gc_suffix="$(webchat_random_hex 6)" || return 1
        _gc_candidate="aimee-$_gc_suffix"
        if ! id "$_gc_candidate" >/dev/null 2>&1 &&
           ! { [ -f "$WEBCHAT_LOGIN_STORE" ] && grep -q "^${_gc_candidate}:" "$WEBCHAT_LOGIN_STORE"; }; then
            wc_generated_user="$_gc_candidate"
            break
        fi
        _gc_tries=$((_gc_tries + 1))
    done
    [ -n "$wc_generated_user" ] || return 1
    wc_generated_pass="$(webchat_random_hex 32)" || return 1

    _gc_dir="$(dirname "$WEBCHAT_BOOTSTRAP_CREDENTIALS")"
    mkdir -p "$_gc_dir" || return 1
    chmod 700 "$_gc_dir" 2>/dev/null || true
    _gc_tmp="${WEBCHAT_BOOTSTRAP_CREDENTIALS}.tmp.$$"
    if ! (umask 077 && printf 'username=%s\npassword=%s\n' \
        "$wc_generated_user" "$wc_generated_pass" > "$_gc_tmp" &&
        mv -f "$_gc_tmp" "$WEBCHAT_BOOTSTRAP_CREDENTIALS"); then
        rm -f "$_gc_tmp"
        return 1
    fi
    chmod 600 "$WEBCHAT_BOOTSTRAP_CREDENTIALS" 2>/dev/null || true
}

webchat_log_generated_credentials() {
    webchat_log "generated temporary browser login (replace it in onboarding):"
    webchat_log "  username: $wc_generated_user"
    webchat_log "  password: $wc_generated_pass"
}

# Recreate any persisted login that is missing from the container's PAM database,
# restoring its exact crypt hash (chpasswd -e). Runs on every start BEFORE the
# env-based bootstrap, so a fresh container rootfs regains every previously
# provisioned account. The env bootstrap can update its password until onboarding
# retires it. An account that already exists is left untouched (only its group
# membership is re-asserted) — the live /etc/shadow wins.
webchat_restore_logins() {
    [ -f "$WEBCHAT_LOGIN_STORE" ] || return 0
    getent group "$WEBCHAT_GROUP" >/dev/null 2>&1 || groupadd --system "$WEBCHAT_GROUP" || true
    while IFS=: read -r _rl_user _rl_hash; do
        [ -n "$_rl_user" ] && [ -n "$_rl_hash" ] || continue
        if id "$_rl_user" >/dev/null 2>&1; then
            usermod --append --groups "$WEBCHAT_GROUP" "$_rl_user" 2>/dev/null || true
            continue
        fi
        webchat_log "restoring persisted login '$_rl_user' (group $WEBCHAT_GROUP)"
        if ! useradd --create-home --shell /usr/sbin/nologin --groups "$WEBCHAT_GROUP" "$_rl_user"; then
            webchat_log "ERROR: useradd failed restoring '$_rl_user' — browser login for it will fail"
            continue
        fi
        # crypt hashes use '$', never ':', so the hash survives the IFS=: split intact.
        if ! printf '%s:%s\n' "$_rl_user" "$_rl_hash" | chpasswd -e 2>/dev/null; then
            webchat_log "ERROR: could not restore password for '$_rl_user' (chpasswd -e failed) — browser login for it will fail"
        fi
    done < "$WEBCHAT_LOGIN_STORE"
}

# Provision (create or update) a single PAM login user in the webchat group.
# Idempotent: re-runs every start, so accounts persist across container rebuilds
# without a manual `useradd` (which lives only in the ephemeral container fs).
webchat_provision_user() {
    _pu_user="$1"
    _pu_pass="$2"
    [ -n "$_pu_user" ] && [ -n "$_pu_pass" ] || return 0
    # PAM authenticates against a real OS user, and useradd's default NAME_REGEX
    # (and the webchat user manager) only accept lowercase letters, digits, '-'
    # and '_'. Reject anything else LOUDLY here rather than let useradd fail
    # silently and leave login broken with a confusing "invalid credentials".
    case "$_pu_user" in
        *[!a-z0-9_-]* | [!a-z_]*)
            webchat_log "ERROR: login user '$_pu_user' is not a valid username (use lowercase letters, digits, '-', '_'; must not start with a digit/hyphen). Skipping — browser login for it will fail."
            return 1
            ;;
    esac
    getent group "$WEBCHAT_GROUP" >/dev/null 2>&1 || groupadd --system "$WEBCHAT_GROUP" || true
    if id "$_pu_user" >/dev/null 2>&1; then
        # Existing user (e.g. the aimee service account): just ensure group membership.
        usermod --append --groups "$WEBCHAT_GROUP" "$_pu_user" 2>/dev/null || true
    else
        webchat_log "creating login user '$_pu_user'"
        # Guard useradd so a failure is reported, not swallowed by the entrypoint's
        # `set -e` (which would crash the container before webchat even starts).
        if ! useradd --create-home --shell /usr/sbin/nologin --groups "$WEBCHAT_GROUP" "$_pu_user"; then
            webchat_log "ERROR: useradd failed for '$_pu_user' — browser login for it will fail"
            return 1
        fi
    fi
    # Set the supplied explicit or generated password. chpasswd splits on the
    # FIRST ':', so a password may contain ':'. Report a failure instead of
    # leaving the account with no usable password.
    if printf '%s:%s\n' "$_pu_user" "$_pu_pass" | chpasswd; then
        webchat_log "login user '$_pu_user' ready (group $WEBCHAT_GROUP)"
        # Mirror the account onto the persistent volume so it survives a fresh
        # container rootfs even if this env is not re-injected next boot.
        webchat_persist_login "$_pu_user"
    else
        webchat_log "ERROR: could not set password for '$_pu_user' (chpasswd failed) — browser login for it will fail"
        return 1
    fi
}

# Additional persistent logins from AIMEE_WEBCHAT_USERS: a list of "user:password"
# entries separated by commas and/or newlines. Only the FIRST ':' splits, so a
# password may contain ':' — but not a comma or newline (those delimit entries).
# Provisioned on every start just like the primary account, so operators can add
# durable browser logins (e.g. "admin:s3cret") that survive a container rebuild.
webchat_bootstrap_extra_users() {
    [ -n "${AIMEE_WEBCHAT_USERS:-}" ] || return 0
    # Normalize commas to newlines, then provision one entry per line. The pipe
    # runs the loop in a subshell, which is fine: provisioning mutates the OS
    # (useradd/chpasswd), not shell state.
    printf '%s\n' "$AIMEE_WEBCHAT_USERS" | tr ',' '\n' | while IFS= read -r _eu_entry; do
        _eu_entry="$(printf '%s' "$_eu_entry" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
        [ -n "$_eu_entry" ] || continue
        case "$_eu_entry" in
            *:*) : ;;
            *)
                webchat_log "AIMEE_WEBCHAT_USERS: skipping malformed entry (expected user:password)"
                continue
                ;;
        esac
        webchat_provision_user "${_eu_entry%%:*}" "${_eu_entry#*:}" || true
    done
}

# Create the primary PAM login from an explicit environment pair, or generate a
# strong temporary pair when both are absent. A partial pair is a configuration
# error: silently inventing the missing half would leave the operator unsure which
# credential is authoritative. Generated plaintext is logged on every boot until
# onboarding replaces it, then is deleted and never logged again.
webchat_bootstrap_user() {
    _wc_user="${AIMEE_WEBCHAT_USER:-}"
    _wc_pass="${AIMEE_WEBCHAT_PASSWORD:-}"

    if { [ -n "$_wc_user" ] && [ -z "$_wc_pass" ]; } ||
       { [ -z "$_wc_user" ] && [ -n "$_wc_pass" ]; }; then
        webchat_log "ERROR: AIMEE_WEBCHAT_USER and AIMEE_WEBCHAT_PASSWORD must be set together, or both omitted for generated credentials"
        return 1
    fi

    if [ -f "$WEBCHAT_BOOTSTRAP_REPLACED" ]; then
        if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
            if ! webchat_read_generated_credentials; then
                webchat_log "ERROR: generated bootstrap credential file is invalid; refusing to leave an unknown login active"
                return 1
            fi
            webchat_retire_login "$wc_generated_user" || return 1
            rm -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" || return 1
        fi
        webchat_restore_logins
        if [ -n "$_wc_user" ]; then
            if [ "$_wc_user" = "aimee" ] && [ "$_wc_pass" = "aimee-local-dev" ]; then
                webchat_log "retired legacy bootstrap login remains disabled"
            else
                webchat_provision_user "$_wc_user" "$_wc_pass" || return 1
            fi
        else
            webchat_log "temporary bootstrap login remains retired"
        fi
        webchat_bootstrap_extra_users
        return 0
    fi

    if [ -n "$_wc_user" ]; then
        # An explicit pair supersedes a previously generated temporary login.
        if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
            if ! webchat_read_generated_credentials; then
                webchat_log "ERROR: generated bootstrap credential file is invalid; refusing to leave an unknown login active"
                return 1
            fi
            webchat_retire_login "$wc_generated_user" || return 1
            rm -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" || return 1
        fi
        webchat_restore_logins
        webchat_provision_user "$_wc_user" "$_wc_pass" || return 1
        webchat_bootstrap_extra_users
        return 0
    fi

    if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
        if ! webchat_read_generated_credentials; then
            webchat_log "ERROR: generated bootstrap credential file is invalid; refusing to rotate an unknown login"
            return 1
        fi
    else
        # Upgrade safety: retire the old published aimee login before creating
        # the first random credential on an existing persistent volume.
        webchat_retire_login "aimee" || return 1
        if ! webchat_generate_credentials; then
            webchat_log "ERROR: could not generate and persist temporary browser credentials"
            return 1
        fi
    fi

    webchat_restore_logins
    webchat_provision_user "$wc_generated_user" "$wc_generated_pass" || return 1
    webchat_log_generated_credentials
    webchat_bootstrap_extra_users
}

# Falsy test for the disable toggle: AIMEE_RUNTIME_WEB_ENABLED (back-compat alias
# AIMEE_RUNTIME_WEB_ENABLED). 0/false/no/off (any case) turns the browser
# UI off. Anything else — including unset/empty — leaves it ON. The webchat is a
# first-class surface: it ships enabled and must be explicitly disabled.
webchat_is_enabled() {
    case "$(printf '%s' "${AIMEE_RUNTIME_WEB_ENABLED:-${AIMEE_RUNTIME_WEB_ENABLED:-1}}" | tr 'A-Z' 'a-z')" in
        0 | false | no | off) return 1 ;;
        *) return 0 ;;
    esac
}

# Provision the webchat<->server shared trust secret. Both aimee-server (which
# validates a webchat `X-Aimee-Webuser` assertion) and aimee-runtime-web (which sends
# it, plus uses it as the legacy socket bearer) read $AIMEE_HOME/server.token.
# Without it EVERY per-user vault/git/editor call fails closed ("aimee-server
# unavailable" / "editor unavailable"), because webchat short-circuits before it
# even reaches the server. Generate a strong random secret once, 0600, owned by
# the aimee user so the privilege-dropped server can read it. Never clobber an
# operator-provided token. Idempotent — safe to call on every start.
webchat_ensure_server_token() {
    _tok_path="${WEBCHAT_HOME}/server.token"
    if [ -s "$_tok_path" ]; then
        return 0
    fi
    mkdir -p "$WEBCHAT_HOME"
    _tok=""
    if command -v openssl >/dev/null 2>&1; then
        _tok="$(openssl rand -hex 32 2>/dev/null)"
    fi
    if [ -z "$_tok" ] && [ -r /dev/urandom ]; then
        _tok="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    fi
    if [ -z "$_tok" ]; then
        webchat_log "WARNING: could not generate server.token (no openssl/urandom); per-user vault/git/editor will be unavailable"
        return 0
    fi
    _umask_old="$(umask)"
    umask 077
    printf '%s\n' "$_tok" > "$_tok_path"
    umask "$_umask_old"
    # aimee-server runs as the unprivileged 'aimee' user and reads this file.
    chown aimee:aimee "$_tok_path" 2>/dev/null || true
    chmod 600 "$_tok_path" 2>/dev/null || true
    webchat_log "generated server.token (webchat<->server trust for per-user vault/git/editor)"
}

# Launch aimee-runtime-web in the background (as root, for PAM). Self-signed TLS on
# :8443 is auto-generated under AIMEE_HOME and persists on the data volume.
webchat_start() {
    # The browser UI ships ENABLED: it is a first-class surface, switched off only
    # when an operator sets AIMEE_RUNTIME_WEB_ENABLED=0. The server's /v1 API is the
    # machine contract; webchat is the human one.
    if ! webchat_is_enabled; then
        webchat_log "AIMEE_RUNTIME_WEB_ENABLED=0; browser UI disabled by operator"
        return 0
    fi
    # webchat is optional: images built with WITH_RUNTIME_WEB=0 ship no aimee-runtime-web
    # binary. Skip the browser UI rather than fail — the server is the contract.
    if ! command -v aimee-runtime-web >/dev/null 2>&1; then
        webchat_log "aimee-runtime-web not present (image built without WITH_RUNTIME_WEB); skipping browser UI"
        return 0
    fi
    webchat_bootstrap_user || return 1
    webchat_ensure_server_token
    # Operator-supplied TLS cert (PEM). A browser-trusted cert is required for the
    # in-app VSCode editor's webviews (the service worker won't register over an
    # untrusted cert). Without these, webchat self-signs (now with the host's IP +
    # hostname in the SAN, so the generated cert can be imported + trusted).
    _wc_tls=""
    if [ -n "${AIMEE_WEBCHAT_TLS_CERT:-}" ] && [ -n "${AIMEE_WEBCHAT_TLS_KEY:-}" ]; then
        _wc_tls="--cert $AIMEE_WEBCHAT_TLS_CERT --key $AIMEE_WEBCHAT_TLS_KEY"
        webchat_log "using operator TLS cert $AIMEE_WEBCHAT_TLS_CERT"
    fi
    webchat_log "starting aimee-runtime-web on :$WEBCHAT_PORT (https), socket=$WEBCHAT_HOME/aimee-server.sock"
    # shellcheck disable=SC2086
    HOME=/root aimee-runtime-web \
        --port "$WEBCHAT_PORT" \
        --socket "$WEBCHAT_HOME/aimee-server.sock" \
        --db "$WEBCHAT_HOME/webchat.db" \
        --spa "$WEBCHAT_SPA" \
        $_wc_tls &
    webchat_pid=$!
}

# Best-effort teardown, called from the entrypoint's shutdown trap.
webchat_stop() {
    [ -n "$webchat_pid" ] && kill "$webchat_pid" 2>/dev/null || true
}
