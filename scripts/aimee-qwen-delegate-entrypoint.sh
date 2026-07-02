#!/bin/bash
# aimee-qwen-delegate: a single-model, OpenAI-compatible coding delegate server on the
# CPU llama.cpp build. One baked GGUF (Qwen 3.6 27B, dense Gated-DeltaNet hybrid), one
# `llama-server`, CPU-only (NGL=0). Defaults to 4 slots x 128K = 512K aggregate
# context with q8_0 KV + flash-attention so the KV cache stays affordable in RAM.
#
# Register the running container as an aimee delegate:
#   aimee agent local qwen3.6-27b http://<host>:8744/v1 \
#       --model qwen3.6-27b --provider openai --slots 4 --ctx 131072 \
#       --roles code,reason,execute --cost-tier 0
set -u

LLAMA=/opt/llama/llama-server
MODEL=/models/synth.gguf
LOG=/tmp/llama-server.log
DEGRADED=/tmp/aimee-delegate-degraded          # sentinel read by the healthcheck

# ---- runtime profile (baked as ENV per image; overridable at deploy) -------------
PORT="${AIMEE_DELEGATE_PORT:-8744}"
CTX="${AIMEE_DELEGATE_CTX:-524288}"            # aggregate across all slots
SLOTS="${AIMEE_DELEGATE_SLOTS:-4}"            # 4 slots -> CTX/SLOTS = 128K each
THREADS="${AIMEE_DELEGATE_THREADS:-16}"       # physical cores (EPYC 7302P = 16c/32t)
NGL="${AIMEE_DELEGATE_NGL:-0}"                # 0 = CPU-only (this image ships no GPU libs)
FA="${AIMEE_DELEGATE_FA:-on}"                 # flash-attention; required for quantized KV
KV_K="${AIMEE_DELEGATE_KV_K:-q8_0}"
KV_V="${AIMEE_DELEGATE_KV_V:-q8_0}"
MODEL_ID="${AIMEE_DELEGATE_MODEL_ID:-qwen3.6-27b}"
JINJA="${AIMEE_DELEGATE_JINJA:-1}"

log(){ echo "aimee-qwen-delegate: $*" >&2; }
rm -f "$DEGRADED"

# ---- supply-chain note: the GGUF sha256 is verified at BUILD time (Dockerfile fails
#      the build on mismatch). We only surface the recorded, build-verified hash here
#      for operator audit; no ~24GB re-hash on every cold start.
BAKED_SHA="$( [ -r /models/synth.sha256 ] && cut -d' ' -f1 </models/synth.sha256 || echo unknown )"
log "model=$MODEL_ID sha256=$BAKED_SHA ctx=$CTX slots=$SLOTS threads=$THREADS ngl=$NGL fa=$FA kv=${KV_K}/${KV_V}"

# ---- assemble llama-server args --------------------------------------------------
# CPU-only: -ngl 0. Quantized KV cache REQUIRES flash-attention, so the cache-type
# flags are only passed when FA is on; with FA off we fall back to the default f16 KV
# (guaranteed to run everywhere, at higher RAM cost).
args=( --host 0.0.0.0 --port "$PORT" -m "$MODEL" --alias "$MODEL_ID"
       -ngl "$NGL" --ctx-size "$CTX" --parallel "$SLOTS" --threads "$THREADS" )
if [ "$FA" = "on" ]; then
  args+=( -fa on --cache-type-k "$KV_K" --cache-type-v "$KV_V" )
else
  log "WARN: flash-attention OFF -> using default f16 KV cache (higher RAM at ${CTX} ctx)"
fi
[ "$JINJA" = "1" ] && args+=( --jinja )

# Auth: if a token is set, actually gate /v1 with it. Passing the token to the
# container but never wiring --api-key would leave /v1 open on the Docker net while
# operators assume it's protected. llama-server exempts /health from the key, so the
# readiness/health curls below still work unauthenticated.
[ -n "${AIMEE_LLM_AUTH_TOKEN:-}" ] && args+=( --api-key "$AIMEE_LLM_AUTH_TOKEN" )

# ---- launch ----------------------------------------------------------------------
log "starting llama-server: ${args[*]}"
"$LLAMA" "${args[@]}" >"$LOG" 2>&1 &
srv=$!

# Graceful shutdown: forward TERM/INT to llama-server and exit 0 so an orchestrator-
# initiated `docker stop` is NOT recorded as a crash. The EXIT trap is best-effort
# cleanup for other exit paths.
# Forward TERM/INT, wait up to ~25s for a graceful exit, then hard-kill so a stuck
# server can't hang `docker stop` until the orchestrator SIGKILLs the whole container.
term() {
  log "received TERM/INT; stopping llama-server"
  kill -TERM "$srv" 2>/dev/null
  for _ in $(seq 1 25); do kill -0 "$srv" 2>/dev/null || break; sleep 1; done
  kill -KILL "$srv" 2>/dev/null
  exit 0
}
trap term TERM INT
trap 'kill "$srv" 2>/dev/null' EXIT

# ---- readiness wait --------------------------------------------------------------
# Up to ~20min cold load: a ~24GB GGUF read into RAM plus KV allocation for 512K ctx.
# Kept comfortably LONGER than the Docker HEALTHCHECK start-period (600s) so a slightly
# slow first load reports unhealthy transiently rather than making the entrypoint exit
# and trip a restart loop (restartPolicy restarts on process EXIT). If FA/quantized-KV
# is unsupported on this CPU build, llama-server errors out at load here -> we surface
# the tail and exit non-zero (loud failure, not a silent slow path).
ready=0
for _ in $(seq 1 1200); do
  if ! kill -0 "$srv" 2>/dev/null; then
    log "FATAL: llama-server exited during load; tail:"; tail -n 30 "$LOG" >&2; exit 1
  fi
  if curl -fsS --max-time 4 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then ready=1; break; fi
  sleep 1
done
[ "$ready" = 1 ] || { log "FATAL: llama-server not ready within deadline"; tail -n 30 "$LOG" >&2; exit 1; }
log "llama-server ready on :$PORT"

# ---- post-launch verification (soft) ---------------------------------------------
# When FA is requested, confirm it actually engaged: quantized KV without FA is a slow
# dequant path. An EXPLICIT "flash disabled" while KV is quantized trips the degraded
# sentinel (healthcheck reports unhealthy so the operator flips KV=f16/FA=off);
# otherwise we only warn -- llama-server would have failed to load if the combo were
# truly unsupported, so a healthy server is the real signal.
verify_startup() {
  local bad=0
  [ "$FA" = "on" ] || return 0
  if grep -qiE 'flash[_ ]?att.*(= *1|enabled|: *1|on)' "$LOG"; then
    log "verified: flash-attention ENGAGED"
  elif grep -qiE 'flash[_ ]?att.*(= *0|disabled|not supported)' "$LOG"; then
    log "CRITICAL: flash-attention did NOT engage but KV=${KV_K}/${KV_V} is quantized"
    log "CRITICAL:   -> set AIMEE_DELEGATE_FA=off + AIMEE_DELEGATE_KV_K/V=f16 and redeploy."
    bad=1
  else
    log "WARN: could not determine flash-attention state from server log (continuing)"
  fi
  return $bad
}
if ! verify_startup; then
  : > "$DEGRADED"
  log "startup verification FAILED -> marked DEGRADED (container will report unhealthy)"
fi

wait "$srv"
code=$?
log "llama-server exited ($code)"
exit "${code:-1}"
