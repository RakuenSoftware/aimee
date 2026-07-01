#!/bin/bash
# aimee-delegate: a single-model, OpenAI-compatible synth/delegate server (Vulkan
# llama.cpp), split OUT of the unified aimee-llm (embed+rerank+gateway) per the
# 2026-07-01 design roundtable. One baked GGUF, one `llama-server`, STATIC expert
# offload (operator-set per card — no runtime VRAM auto-sizer, by design decision).
#
# Two published tiers differ only in the baked model + these ENV defaults:
#   aimee-delegate-small (16GB): Gemma 4 12B  (dense; 96K, 1 slot)
#   aimee-delegate-mid   (24-32GB): Qwen 3.6 35B-A3B (MoE; 256K = 2x128K, --n-cpu-moe)
#
# The embedder+reranker live on the SEPARATE `kb` image (aimee-llm), which points
# its AIMEE_LLM_SYNTH_URL at this container. That keeps the KB byte-portable and this
# tier swappable independently.
set -u

LLAMA=/opt/llama/llama-server
MODEL=/models/synth.gguf
LOG=/tmp/llama-server.log
DEGRADED=/tmp/aimee-delegate-degraded          # sentinel read by the healthcheck

# ---- tier runtime profile (baked as ENV per image; overridable at deploy) --------
PORT="${AIMEE_DELEGATE_PORT:-8083}"
CTX="${AIMEE_DELEGATE_CTX:-131072}"
SLOTS="${AIMEE_DELEGATE_SLOTS:-1}"
NGL="${AIMEE_DELEGATE_NGL:-999}"               # 999, not -1: some RADV/ROCm ignore -1
KV_K="${AIMEE_DELEGATE_KV_K:-q8_0}"
KV_V="${AIMEE_DELEGATE_KV_V:-q4_0}"            # K8V4 default (K is more quant-sensitive than V)
IS_MOE="${AIMEE_DELEGATE_MOE:-0}"             # "1" on the Qwen MoE tier
MOE_LAYERS="${AIMEE_DELEGATE_MOE_LAYERS:-40}" # Qwen 3.6 35B-A3B: MoE in all 40 blocks
N_CPU_MOE="${AIMEE_DELEGATE_N_CPU_MOE:-0}"    # STATIC expert-offload count (operator-set per card)
MODEL_ID="${AIMEE_DELEGATE_MODEL_ID:-aimee-synth}"
MIN_VRAM_MB="${AIMEE_DELEGATE_MIN_VRAM_MB:-12000}"
JINJA="${AIMEE_DELEGATE_JINJA:-1}"

log(){ echo "aimee-delegate: $*" >&2; }
rm -f "$DEGRADED"

# ---- supply-chain note: the GGUF sha256 is verified at BUILD time (Dockerfile
#      fails the build on mismatch). We only surface the recorded, build-verified
#      hash here for /health + operator audit (panel finding [9]); no 22GB re-hash
#      on every cold start.
BAKED_SHA="$( [ -r /models/synth.sha256 ] && cut -d' ' -f1 </models/synth.sha256 || echo unknown )"
log "model=$MODEL_ID sha256=$BAKED_SHA ctx=$CTX slots=$SLOTS ngl=$NGL kv=${KV_K}/${KV_V} moe=$IS_MOE"

# ---- GPU guard: min-VRAM floor + discrete-GPU pick (panel [8]). Vulkan-only, so
#      detect via kernel sysfs (no rocm-smi/vulkaninfo binary). Discrete AMD cards
#      expose mem_info_vram_total; most iGPUs do not -> they're skipped, and an
#      iGPU-only/APU host fails the floor instead of trying to load a big model into
#      a 512MB device.
best_mb=0
for dev in /sys/class/drm/card*/device; do
  [ -r "$dev/mem_info_vram_total" ] || continue
  raw=$(cat "$dev/mem_info_vram_total" 2>/dev/null)
  case "$raw" in ''|*[!0-9]*) continue;; esac      # skip empty/non-numeric (no arith crash)
  mb=$(( raw / 1048576 ))
  [ "$mb" -gt "$best_mb" ] && best_mb=$mb
done
if [ "$best_mb" -eq 0 ]; then
  log "WARN: no discrete GPU VRAM readable via sysfs; proceeding (CPU/other backend or restricted /sys)"
elif [ "$best_mb" -lt "$MIN_VRAM_MB" ]; then
  log "FATAL: largest discrete GPU = ${best_mb}MB < floor ${MIN_VRAM_MB}MB; refusing to load $MODEL_ID"
  exit 4
else
  log "GPU VRAM detected: ${best_mb}MB (floor ${MIN_VRAM_MB}MB)"
fi

# ---- assemble llama-server args --------------------------------------------------
args=( --host 0.0.0.0 --port "$PORT" -m "$MODEL" --alias "$MODEL_ID"
       -ngl "$NGL" --ctx-size "$CTX" --parallel "$SLOTS"
       -fa on --cache-type-k "$KV_K" --cache-type-v "$KV_V" )
[ "$JINJA" = "1" ] && args+=( --jinja )

# Auth: if a token is set, actually gate /v1 with it. Passing the token to the
# container but never wiring --api-key would leave /v1 open on the Docker net while
# operators assume it's protected (panel code-review [3]). llama-server exempts
# /health from the key, so the readiness/health curls below still work unauthenticated.
[ -n "${AIMEE_LLM_AUTH_TOKEN:-}" ] && args+=( --api-key "$AIMEE_LLM_AUTH_TOKEN" )

# STATIC MoE expert-offload, defensively clamped to [0, MOE_LAYERS] (panel [4]).
if [ "$IS_MOE" = "1" ]; then
  case "$N_CPU_MOE" in ''|*[!0-9]*) log "FATAL: AIMEE_DELEGATE_N_CPU_MOE='$N_CPU_MOE' not a non-negative integer"; exit 5;; esac
  [ "$N_CPU_MOE" -gt "$MOE_LAYERS" ] && N_CPU_MOE=$MOE_LAYERS
  args+=( --n-cpu-moe "$N_CPU_MOE" )
  log "MoE expert-offload: --n-cpu-moe $N_CPU_MOE of $MOE_LAYERS layers on CPU"
fi

# ---- launch ----------------------------------------------------------------------
log "starting llama-server: ${args[*]}"
"$LLAMA" "${args[@]}" >"$LOG" 2>&1 &
srv=$!

# Graceful shutdown: forward TERM/INT to llama-server and exit 0 so an orchestrator-
# initiated `docker stop` is NOT recorded as a crash (panel code-review [5]). The EXIT
# trap is only a best-effort cleanup for other exit paths.
term() { log "received TERM/INT; stopping llama-server"; kill -TERM "$srv" 2>/dev/null; wait "$srv" 2>/dev/null; exit 0; }
trap term TERM INT
trap 'kill "$srv" 2>/dev/null' EXIT

# ---- readiness wait, then post-launch verification (panels [5] + [7]) ------------
# We do NOT run a separate probe load (a 22GB double-read would worsen cold-start /
# restart-storm risk, panel [13]). Instead we parse the real server's own startup log
# once /health is up, and assert that (a) flash-attention actually engaged and (b) the
# requested KV cache types were honored -- because on Vulkan, FA / quantized-V can
# silently fall through to a slow dequant path, and llama.cpp can silently CPU-fall-
# back MoE experts on OOM. A mismatch does NOT auto-relaunch (no 22GB re-read storm);
# it logs CRITICAL and trips the healthcheck so the operator flips KV/offload envs.
ready=0
for _ in $(seq 1 600); do                      # up to ~10min cold load for the 22GB tier
  if ! kill -0 "$srv" 2>/dev/null; then
    log "FATAL: llama-server exited during load; tail:"; tail -n 30 "$LOG" >&2; exit 1
  fi
  if curl -fsS --max-time 4 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then ready=1; break; fi
  sleep 1
done
[ "$ready" = 1 ] || { log "FATAL: llama-server not ready within deadline"; tail -n 30 "$LOG" >&2; exit 1; }
log "llama-server ready on :$PORT"

verify_startup() {
  local bad=0                                     # 0 = healthy (shell success); 1 = degraded
  # (a) flash-attention engaged?  llama.cpp logs "flash_attn = 1" (or "enabled").
  if grep -qiE 'flash[_ ]?att.*(= *1|enabled|: *1|on)' "$LOG"; then
    log "verified: flash-attention ENGAGED"
  elif grep -qiE 'flash[_ ]?att.*(= *0|disabled|not supported)' "$LOG"; then
    log "CRITICAL: flash-attention did NOT engage on this backend, but KV V-cache=$KV_V was requested"
    log "CRITICAL:   -> quantized V-cache without FA hits a slow dequant path. Set AIMEE_DELEGATE_KV_V=q8_0 (K8V8) and redeploy."
    bad=1
  else
    log "WARN: could not determine flash-attention state from server log (continuing)"
  fi
  # (b) MoE residency. NOTE: llama.cpp does NOT reliably emit an 'out of memory' string
  #     for SILENT expert CPU-fallback under VRAM pressure (panel code-review [1]) --
  #     it just prints per-device buffer sizes. So we (1) still fail on an EXPLICIT
  #     allocation error if one is logged, and (2) surface the device buffer split for
  #     the operator to eyeball against intent, rather than claim full auto-detection.
  if [ "$IS_MOE" = "1" ]; then
    if grep -qiE 'out of memory|failed to allocate|ErrorOutOfDeviceMemory|cannot allocate|alloc.*fail' "$LOG"; then
      log "CRITICAL: allocation failure in server log -> raise AIMEE_DELEGATE_N_CPU_MOE and redeploy."
      bad=1
    fi
    log "device buffer split (confirm experts landed as intended for N=$N_CPU_MOE):"
    grep -iE 'model buffer size|KV .*buffer size|compute buffer size|CPU_Mapped|Vulkan[0-9]|offloD?ed .*layers' "$LOG" | tail -n 8 >&2 || true
  fi
  return $bad
}
if ! verify_startup; then
  : > "$DEGRADED"                              # trip the healthcheck (see aimee-delegate-health.sh)
  log "startup verification FAILED -> marked DEGRADED (container will report unhealthy)"
fi

wait "$srv"
code=$?
log "llama-server exited ($code)"
exit "${code:-1}"
