#!/bin/sh
# Entrypoint for the combined aimee-server+kb image.
#
# Starts aimee-kb (DB2 + pgvector + embedder over /v1 on loopback :8741), waits
# for it to report healthy, then starts aimee-server pointed at it via
# AIMEE_KB_API_URL=http://127.0.0.1:8741, plus the co-located aimee-webchat
# browser UI on :8443. The container's lifecycle follows the SERVER: if the
# server exits, we tear down the kb + webchat and exit with the server's code;
# SIGTERM/SIGINT are forwarded to all so `docker stop` is clean.
#
# Runs as root so it can bootstrap the webchat PAM login user and run webchat
# (which needs /etc/shadow access for pam_unix); aimee-kb and aimee-server are
# dropped to the unprivileged "aimee" user via runuser.
#
# POSIX sh (the image has no bash). Endpoints/DB come from the environment
# (compose supplies AIMEE_DB2_URL / AIMEE_EMBEDDER_URL).
set -eu

AIMEE_HOME="${AIMEE_HOME:-/var/lib/aimee}"
KB_HTTP_PORT="${AIMEE_KB_HTTP_PORT:-8741}"
SERVER_SOCK="${AIMEE_SERVER_SOCK:-/var/lib/aimee/aimee-server.sock}"
KB_WAIT_SECONDS="${KB_WAIT_SECONDS:-120}"

# Worker threads (server + kb drain/ingest/watch/query) need a 64 MB stack; the
# 8 MB container default overflows and SIGSEGVs on real queries. Raise the soft
# limit here (inherited by the runuser children); hard limit is unlimited on
# typical hosts. Best-effort — a runtime --ulimit still works if disallowed.
ulimit -s 65536 2>/dev/null || true

# Seed the baked default config into AIMEE_HOME if absent. The server and kb
# share AIMEE_HOME and both read $AIMEE_HOME/aimee.yaml; a bind-mounted (empty)
# volume would otherwise leave them with no config (no /v1 bearer; embeddings
# falling back to the broken builtin). Never clobber an operator's config. Done
# as root before dropping to "aimee", then chown so aimee can read/rewrite it.
if [ ! -f "$AIMEE_HOME/aimee.yaml" ] && [ -f /opt/aimee/defaults/aimee.yaml ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/aimee.yaml "$AIMEE_HOME/aimee.yaml"
fi
# Ensure aimee (uid 1000) owns its home + workspaces so it can write on an empty
# bind mount. On smoothfs tiers ownership is forced to 1000 regardless; harmless.
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true

kb_pid=""
server_pid=""

. /usr/local/bin/webchat-lib.sh

log() { printf '[combined-entrypoint] %s\n' "$*"; }

shutdown() {
    # Best-effort teardown of all children; ignore errors during shutdown.
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null || true
    webchat_stop
}
trap 'shutdown' TERM INT

log "starting aimee-kb (http-port=$KB_HTTP_PORT) as user aimee"
runuser -u aimee -- aimee-kb --http-port="$KB_HTTP_PORT" &
kb_pid=$!

log "waiting up to ${KB_WAIT_SECONDS}s for aimee-kb /v1/health on :$KB_HTTP_PORT"
i=0
while :; do
    if curl -fsS --max-time 3 "http://127.0.0.1:${KB_HTTP_PORT}/v1/health" >/dev/null 2>&1; then
        log "aimee-kb is healthy"
        break
    fi
    # If the kb process died during startup, fail fast with its exit status.
    if ! kill -0 "$kb_pid" 2>/dev/null; then
        wait "$kb_pid" || true
        log "aimee-kb exited during startup; aborting"
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge "$KB_WAIT_SECONDS" ]; then
        log "aimee-kb did not become healthy within ${KB_WAIT_SECONDS}s; aborting"
        shutdown
        exit 1
    fi
    sleep 1
done

log "starting aimee-server (socket=$SERVER_SOCK kb=$AIMEE_KB_API_URL) as user aimee"
runuser -u aimee -- aimee-server --socket="$SERVER_SOCK" &
server_pid=$!

# Browser UI (root, PAM). Supplementary — a webchat crash must not take the
# container down; the server is the contract.
webchat_start

# Wait for whichever child exits first; the server is the container's contract,
# so its exit (or a forwarded signal) tears the container down.
wait "$server_pid"
status=$?
log "aimee-server exited (status $status); shutting down aimee-kb + webchat"
shutdown
exit "$status"
