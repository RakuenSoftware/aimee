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
# Seed the default delegate roster (definitions only; keys are client-held) so
# delegates / the roundtable work out of the box. Never clobber an operator's.
if [ ! -f "$AIMEE_HOME/agents.json" ] && [ -f /opt/aimee/defaults/agents.json ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/agents.json "$AIMEE_HOME/agents.json"
fi
# Seed default dev-lifecycle workflows so autonomous development (default-on) can
# resolve "build" out of the box. Shipped defaults are hash-tracked under
# .seeded/<name>: a fresh install is seeded and its hash recorded; on later
# starts an UNMODIFIED managed default (on-disk hash still equals the recorded
# seed hash) is refreshed when the image ships a newer one. An operator-edited
# default (hash diverged) or one of unknown provenance (no seed record and not
# already equal to the shipped default) is never clobbered.
if [ -d /opt/aimee/defaults/workflows ]; then
    mkdir -p "$AIMEE_HOME/workflows/.seeded"
    for wf in /opt/aimee/defaults/workflows/*.yaml; do
        [ -e "$wf" ] || continue
        base=$(basename "$wf")
        dst="$AIMEE_HOME/workflows/$base"
        rec="$AIMEE_HOME/workflows/.seeded/$base"
        if ! command -v sha256sum >/dev/null 2>&1; then
            # No hasher available: fall back to conservative never-clobber seed.
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
            # No record yet, but already identical to the shipped default: adopt
            # it as managed so a future image can refresh it.
            printf '%s\n' "$shipped" > "$rec"
        fi
    done
fi
# Ensure aimee (uid 1000) owns its home + workspaces so it can write on an empty
# bind mount. On smoothfs tiers ownership is forced to 1000 regardless; harmless.
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true

kb_pid=""
server_pid=""
llm_pid=""

. /usr/local/bin/webchat-lib.sh

log() { printf '[combined-entrypoint] %s\n' "$*"; }

shutdown() {
    # Best-effort teardown of all children; ignore errors during shutdown.
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null || true
    [ -n "$llm_pid" ] && kill "$llm_pid" 2>/dev/null || true
    webchat_stop
}
trap 'shutdown' TERM INT

# --- bundled aimee-llm (CPU): auto-start + point the kb/server at it ---------
# When this image was built WITH_LLM=1 it carries the unified llama.cpp/Vulkan
# gateway (embed + rerank + Tier-A synth) and its baked GGUFs. We start it here
# and repoint the kb's embed path + the curator synth at the in-container gateway
# so the combined container is self-contained (no external embedder/llm).
#
# Operator endpoints WIN — an existing deploy that set AIMEE_LLM_URL,
# AIMEE_EMBEDDER_URL or LLM_ENDPOINT (e.g. an external embedder, or opting up to a
# GPU endpoint) is never silently overridden:
#   AIMEE_BUNDLED_LLM=auto (default) — start the bundled gateway ONLY if the
#       operator configured no endpoint; otherwise respect theirs.
#   AIMEE_BUNDLED_LLM=on            — always start + use the bundled gateway,
#       overriding any operator endpoint.
#   AIMEE_BUNDLED_LLM=off           — never start it; use operator/legacy endpoints.
GW_PORT="${AIMEE_LLM_PORT:-8742}"
operator_llm=0
if [ -n "${AIMEE_LLM_URL:-}" ] || [ -n "${AIMEE_EMBEDDER_URL:-}" ] || [ -n "${LLM_ENDPOINT:-}" ]; then
    operator_llm=1
fi
start_bundled_llm=0
if [ -x /opt/aimee/supervisor.sh ]; then
    case "${AIMEE_BUNDLED_LLM:-auto}" in
        on)   start_bundled_llm=1 ;;
        off)  start_bundled_llm=0 ;;
        auto) if [ "$operator_llm" = 0 ]; then start_bundled_llm=1; fi ;;
        *)    log "AIMEE_BUNDLED_LLM='${AIMEE_BUNDLED_LLM:-}' unrecognized; treating as auto"
              if [ "$operator_llm" = 0 ]; then start_bundled_llm=1; fi ;;
    esac
fi
if [ "$operator_llm" = 1 ] && [ "$start_bundled_llm" = 0 ]; then
    log "using operator-configured LLM endpoints (bundled aimee-llm not started)"
fi
if [ "$start_bundled_llm" = 1 ]; then
    log "starting bundled aimee-llm (CPU) gateway on :$GW_PORT as user aimee"
    # Set the runtime's lib + module paths INSIDE the child (after runuser's uid
    # switch) via sh -c, not as inline vars on runuser: LD_LIBRARY_PATH is
    # security-sensitive and can be dropped across a privilege change, and we must
    # NOT put /opt/llama/lib on the aimee-server/kb load path anyway. The gateway's
    # other settings (AIMEE_LLM_EMBED_URL/RERANK_URL/SYNTH_URL/RERANK_HEAD/...) are
    # image ENV that runuser preserves like the kb/server's existing env.
    runuser -u aimee -- sh -c \
        'LD_LIBRARY_PATH=/opt/llama/lib:/opt/llama PYTHONPATH=/opt/aimee AIMEE_LLM_PORT="$1" exec /opt/aimee/supervisor.sh' \
        aimee-llm "$GW_PORT" &
    llm_pid=$!
    # One-knob: drive the kb embed path (in-process http; the seeded config has no
    # embedding_command) + reranking via the gateway, and the curator synth
    # sidecar via its /v1. Default the CPU tier dim (operator-set dim wins). These
    # exports reach the kb/server children below (runuser preserves the env).
    export AIMEE_LLM_URL="http://127.0.0.1:${GW_PORT}"
    export AIMEE_EMBEDDER_URL="http://127.0.0.1:${GW_PORT}"
    export LLM_ENDPOINT="http://127.0.0.1:${GW_PORT}/v1"
    : "${AIMEE_EMBEDDING_DIM:=1024}"
    export AIMEE_EMBEDDING_DIM
else
    # Not starting the bundled gateway. Honor operator endpoints, deriving the
    # ones they left unset from the AIMEE_LLM_URL one-knob; fall back to the legacy
    # external defaults (embedder:8080 / llm:8080) so a lean WITH_LLM=0 image with
    # the external-llm compose profile keeps working unchanged.
    if [ -n "${AIMEE_LLM_URL:-}" ]; then
        export AIMEE_EMBEDDER_URL="${AIMEE_EMBEDDER_URL:-$AIMEE_LLM_URL}"
        export LLM_ENDPOINT="${LLM_ENDPOINT:-${AIMEE_LLM_URL}/v1}"
    fi
    export AIMEE_EMBEDDER_URL="${AIMEE_EMBEDDER_URL:-http://embedder:8080}"
    export LLM_ENDPOINT="${LLM_ENDPOINT:-http://llm:8080/v1}"
fi

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

# If we started the bundled gateway, wait for it to load its models so the first
# kb embed/search succeeds. Cold CPU load of multi-GB GGUFs is slow, so allow a
# long window. FAIL-OPEN: if it doesn't come up we still bring up the server (the
# container's contract) — embeddings/synth degrade until the gateway is healthy,
# rather than the whole container failing. If the supervisor died, log and move on.
if [ "$start_bundled_llm" = 1 ]; then
    LLM_WAIT_SECONDS="${AIMEE_LLM_WAIT_SECONDS:-600}"
    log "waiting up to ${LLM_WAIT_SECONDS}s for bundled aimee-llm /health on :$GW_PORT"
    i=0
    while :; do
        if curl -fsS --max-time 3 "http://127.0.0.1:${GW_PORT}/health" >/dev/null 2>&1; then
            log "bundled aimee-llm is healthy"
            break
        fi
        if ! kill -0 "$llm_pid" 2>/dev/null; then
            log "WARNING: bundled aimee-llm exited during startup; embeddings/synth will be unavailable until it is restarted (continuing fail-open)"
            llm_pid=""
            break
        fi
        i=$((i + 1))
        if [ "$i" -ge "$LLM_WAIT_SECONDS" ]; then
            log "WARNING: bundled aimee-llm not healthy within ${LLM_WAIT_SECONDS}s; continuing fail-open (embeddings/synth degraded)"
            break
        fi
        sleep 1
    done
fi

# Delegate-vault auto-provisioning. aimee-server seals operator-supplied delegate
# API keys into its server-principal vault at startup, so a fresh deploy's
# delegates/roundtables work with no manual `aimee vault set`. The source comes
# from the environment, which runuser preserves into the aimee-server child:
#   - AIMEE_DELEGATE_SECRETS_FILE=/run/secrets/aimee-delegates.json
#       a JSON object {"<agent>":"<api-key>", ...}. Mount it readable by the
#       container's "aimee" user (the server reads it after dropping privileges).
#   - AIMEE_DELEGATE_KEY_<AGENT>=<api-key>   (env-only convenience; agent name
#       lowercased, e.g. AIMEE_DELEGATE_KEY_MISTRAL).
# Non-destructive by default; set AIMEE_DELEGATE_SECRETS_OVERWRITE=1 to replace an
# existing vaulted key. Secrets are never written to AIMEE_HOME or logs.
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
