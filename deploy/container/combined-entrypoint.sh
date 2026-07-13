#!/bin/sh
# Entrypoint for the all-in-one aimee appliance (server + kb + llm in one image).
#
# Starts the bundled aimee-llm (embed/rerank/synth on loopback :8080, cpu tier) and
# aimee-kb (DB2 + pgvector over /v1 on loopback :8741), waits for the kb to report
# healthy, then starts aimee-server pointed at the kb via
# AIMEE_KB_API_URL=http://127.0.0.1:8741, plus the co-located aimee-webchat UI on
# :8443. The container's lifecycle follows the SERVER: if it exits, we tear down the
# llm + kb + webchat and exit with its code; SIGTERM/SIGINT are forwarded to all.
#
# Runs as root so it can bootstrap the webchat PAM login user (pam_unix needs
# /etc/shadow); the llm, kb, and server are dropped to the unprivileged "aimee" user.
#
# POSIX sh (no bash). Postgres is external (compose supplies AIMEE_DB2_URL).
set -eu

AIMEE_HOME="${AIMEE_HOME:-/var/lib/aimee}"
KB_HTTP_PORT="${AIMEE_KB_HTTP_PORT:-8741}"
SERVER_SOCK="${AIMEE_SERVER_SOCK:-/var/lib/aimee/aimee-server.sock}"
KB_WAIT_SECONDS="${KB_WAIT_SECONDS:-120}"

# Worker threads (server + kb) need a 64 MB stack; the 8 MB container default
# SIGSEGVs on real queries. Raise the soft limit (inherited by the children).
ulimit -s 65536 2>/dev/null || true

# Seed the baked default config/roster/workflows into AIMEE_HOME if absent, never
# clobbering an operator's. Done as root, then chowned to aimee.
if [ ! -f "$AIMEE_HOME/aimee.yaml" ] && [ -f /opt/aimee/defaults/aimee.yaml ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/aimee.yaml "$AIMEE_HOME/aimee.yaml"
fi
if [ ! -f "$AIMEE_HOME/agents.json" ] && [ -f /opt/aimee/defaults/agents.json ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/agents.json "$AIMEE_HOME/agents.json"
fi
if [ -d /opt/aimee/defaults/workflows ]; then
    mkdir -p "$AIMEE_HOME/workflows/.seeded"
    for wf in /opt/aimee/defaults/workflows/*.yaml; do
        [ -e "$wf" ] || continue
        base=$(basename "$wf")
        dst="$AIMEE_HOME/workflows/$base"
        rec="$AIMEE_HOME/workflows/.seeded/$base"
        if ! command -v sha256sum >/dev/null 2>&1; then
            [ -f "$dst" ] || cp "$wf" "$dst"
            continue
        fi
        shipped=$(sha256sum "$wf" | cut -d' ' -f1)
        if [ ! -f "$dst" ]; then
            cp "$wf" "$dst" && printf '%s\n' "$shipped" > "$rec"
        elif [ -f "$rec" ]; then
            disk=$(sha256sum "$dst" | cut -d' ' -f1)
            if [ "$disk" = "$(cat "$rec")" ] && [ "$disk" != "$shipped" ]; then
                cp "$wf" "$dst" && printf '%s\n' "$shipped" > "$rec"
            fi
        elif [ "$(sha256sum "$dst" | cut -d' ' -f1)" = "$shipped" ]; then
            printf '%s\n' "$shipped" > "$rec"
        fi
    done
fi
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" /models 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true

llm_pid=""
kb_pid=""
server_pid=""

. /usr/local/bin/webchat-lib.sh

log() { printf '[combined-entrypoint] %s\n' "$*"; }

shutdown() {
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null || true
    [ -n "$llm_pid" ] && kill "$llm_pid" 2>/dev/null || true
    webchat_stop
}
trap 'shutdown' TERM INT

# --- bundled aimee-llm (embed/rerank/synth on loopback, cpu tier) ------------
# The supervisor downloads the cpu-tier models into /models on first boot, so it
# can take minutes to report healthy. We do NOT block on it: the kb fail-opens on
# embedding until the endpoint is reachable, so the server comes up promptly.
LLM_PORT="${AIMEE_LLM_PORT:-8080}"
log "starting bundled aimee-llm (tier=${AIMEE_LLM_TIER:-cpu} port=$LLM_PORT) as user aimee"
# Exec the script itself (bash shebang): it uses bash arrays/wait -n, which
# dash chokes on ("Syntax error: \"(\" unexpected") — under `sh` the bundled
# llm silently never started and the kb dim probe waited forever.
runuser -u aimee -- /opt/aimee/supervisor.sh &
llm_pid=$!

# The kb + curator reach the bundled llm on loopback. AIMEE_LLM_URL is baked to
# http://127.0.0.1:8080; derive the embed/synth endpoints from it when unset.
if [ -n "${AIMEE_LLM_URL:-}" ]; then
    export AIMEE_EMBEDDER_URL="${AIMEE_EMBEDDER_URL:-$AIMEE_LLM_URL}"
    export LLM_ENDPOINT="${LLM_ENDPOINT:-${AIMEE_LLM_URL}/v1}"
fi
export AIMEE_EMBEDDER_URL="${AIMEE_EMBEDDER_URL:-http://127.0.0.1:${LLM_PORT}}"
export LLM_ENDPOINT="${LLM_ENDPOINT:-http://127.0.0.1:${LLM_PORT}/v1}"

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
    if ! kill -0 "$kb_pid" 2>/dev/null; then
        wait "$kb_pid" || true
        log "aimee-kb exited during startup; aborting"
        shutdown
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

# The server is the container's contract: its exit tears the container down.
wait "$server_pid"
status=$?
log "aimee-server exited (status $status); shutting down llm + kb + webchat"
shutdown
exit "$status"
