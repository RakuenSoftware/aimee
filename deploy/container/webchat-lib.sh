# webchat-lib.sh — POSIX sh helpers to bootstrap + launch aimee-webchat inside a
# container. Sourced (not executed) by the image entrypoints, which run as root.
#
# aimee-webchat is the Go browser service. It is co-located with aimee-server in
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
WEBCHAT_SPA="${AIMEE_WEBCHAT_SPA:-/usr/local/share/aimee-webchat/index.html}"
webchat_pid=""

webchat_log() { printf '[webchat] %s\n' "$*"; }

# webchat scopes its user-management surface to a named OS group (it constructs
# auth.NewUserManager("aimee")). Adding the bootstrap account to that group makes
# it visible/manageable in the dashboard; login itself is PAM-only and does not
# require the group.
WEBCHAT_GROUP="${AIMEE_WEBCHAT_GROUP:-aimee}"

# Create (or update) the bootstrap login user from env. PAM authenticates browser
# logins against a real OS user, so without this no one can sign in. Unset
# credentials are tolerated: webchat still serves (login page up), it just has no
# account until an operator provisions one.
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
            return 0
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
            return 0
        fi
    fi
    # Set the password from AIMEE_WEBCHAT_PASSWORD. chpasswd splits on the FIRST
    # ':', so a password may contain ':'. Report a failure instead of leaving the
    # account with no usable password.
    if printf '%s:%s\n' "$_pu_user" "$_pu_pass" | chpasswd; then
        webchat_log "login user '$_pu_user' ready (group $WEBCHAT_GROUP)"
    else
        webchat_log "ERROR: could not set password for '$_pu_user' (chpasswd failed) — browser login for it will fail"
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
        webchat_provision_user "${_eu_entry%%:*}" "${_eu_entry#*:}"
    done
}

# Create (or update) the bootstrap login accounts from env. PAM authenticates
# browser logins against real OS users, so without at least one no one can sign
# in. The primary account (AIMEE_WEBCHAT_USER/PASSWORD) is optional — unset is
# tolerated (login page still serves) — and AIMEE_WEBCHAT_USERS adds any number
# of extra persistent accounts.
webchat_bootstrap_user() {
    _wc_user="${AIMEE_WEBCHAT_USER:-}"
    _wc_pass="${AIMEE_WEBCHAT_PASSWORD:-}"
    if [ -z "$_wc_user" ] || [ -z "$_wc_pass" ]; then
        webchat_log "AIMEE_WEBCHAT_USER/AIMEE_WEBCHAT_PASSWORD unset; skipping primary login bootstrap (AIMEE_WEBCHAT_USERS may still provision accounts)"
    else
        webchat_provision_user "$_wc_user" "$_wc_pass"
    fi
    webchat_bootstrap_extra_users
}

# Falsy test for the disable toggle: 0/false/no/off (any case) turns the browser
# UI off. Anything else — including unset/empty — leaves it ON. The webchat is a
# first-class surface: it ships enabled and must be explicitly disabled.
webchat_is_enabled() {
    case "$(printf '%s' "${AIMEE_WEBCHAT_ENABLED:-1}" | tr 'A-Z' 'a-z')" in
        0 | false | no | off) return 1 ;;
        *) return 0 ;;
    esac
}

# Provision the webchat<->server shared trust secret. Both aimee-server (which
# validates a webchat `X-Aimee-Webuser` assertion) and aimee-webchat (which sends
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

# Launch aimee-webchat in the background (as root, for PAM). Self-signed TLS on
# :8443 is auto-generated under AIMEE_HOME and persists on the data volume.
webchat_start() {
    # The browser UI ships ENABLED: it is a first-class surface, switched off only
    # when an operator sets AIMEE_WEBCHAT_ENABLED=0. The server's /v1 API is the
    # machine contract; webchat is the human one.
    if ! webchat_is_enabled; then
        webchat_log "AIMEE_WEBCHAT_ENABLED=0; browser UI disabled by operator"
        return 0
    fi
    # webchat is optional: images built with WITH_WEBCHAT=0 ship no aimee-webchat
    # binary. Skip the browser UI rather than fail — the server is the contract.
    if ! command -v aimee-webchat >/dev/null 2>&1; then
        webchat_log "aimee-webchat not present (image built without WITH_WEBCHAT); skipping browser UI"
        return 0
    fi
    webchat_bootstrap_user
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
    webchat_log "starting aimee-webchat on :$WEBCHAT_PORT (https), socket=$WEBCHAT_HOME/aimee-server.sock"
    # shellcheck disable=SC2086
    HOME=/root aimee-webchat \
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
