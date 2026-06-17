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
webchat_bootstrap_user() {
    _wc_user="${AIMEE_WEBCHAT_USER:-}"
    _wc_pass="${AIMEE_WEBCHAT_PASSWORD:-}"
    if [ -z "$_wc_user" ] || [ -z "$_wc_pass" ]; then
        webchat_log "AIMEE_WEBCHAT_USER/AIMEE_WEBCHAT_PASSWORD unset; skipping login bootstrap (no account can sign in until one is provisioned)"
        return 0
    fi
    getent group "$WEBCHAT_GROUP" >/dev/null 2>&1 || groupadd --system "$WEBCHAT_GROUP"
    if id "$_wc_user" >/dev/null 2>&1; then
        # Existing user (e.g. the service account): just ensure group membership.
        usermod --append --groups "$WEBCHAT_GROUP" "$_wc_user" 2>/dev/null || true
    else
        webchat_log "creating login user '$_wc_user'"
        useradd --create-home --shell /usr/sbin/nologin --groups "$WEBCHAT_GROUP" "$_wc_user"
    fi
    printf '%s:%s\n' "$_wc_user" "$_wc_pass" | chpasswd
    webchat_log "login user '$_wc_user' ready (group $WEBCHAT_GROUP)"
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
    webchat_log "starting aimee-webchat on :$WEBCHAT_PORT (https), socket=$WEBCHAT_HOME/aimee-server.sock"
    HOME=/root aimee-webchat \
        --port "$WEBCHAT_PORT" \
        --socket "$WEBCHAT_HOME/aimee-server.sock" \
        --db "$WEBCHAT_HOME/webchat.db" \
        --spa "$WEBCHAT_SPA" &
    webchat_pid=$!
}

# Best-effort teardown, called from the entrypoint's shutdown trap.
webchat_stop() {
    [ -n "$webchat_pid" ] && kill "$webchat_pid" 2>/dev/null || true
}
