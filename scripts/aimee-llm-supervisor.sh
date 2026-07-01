#!/bin/bash
# aimee-llm container supervisor: launch the per-role llama.cpp servers (embed,
# rerank-encoder, synth) from the baked GGUFs, then the gateway. Each role is its
# own llama-server process so a crash is isolated to that role; if any exits the
# supervisor tears the container down (the orchestrator restarts it). A full
# s6-overlay (per-role restart) is the follow-up; this is the MVP supervisor.
set -u
LLAMA=/opt/llama/llama-server
NGL="${AIMEE_LLM_NGL:-0}"
POOL="${AIMEE_LLM_EMBED_POOLING:-last}"
# Synth context window. The synth GGUF (gemma-4-12b) trains to 256K, but a
# hardcoded --ctx-size 8192 wasted that: large code symbols / doc chunks overran
# 8K and the server returned HTTP 400, failing extraction entirely. Default to a
# 16GB-VRAM-safe 32K (4x the old cap, covers real source files); raise it on
# bigger cards via AIMEE_LLM_SYNTH_CTX (e.g. 131072 for 128K). KV cache scales
# with context, so size it to the card. Embed/rerank inputs are small — they keep
# the 8K default.
SYNTH_CTX="${AIMEE_LLM_SYNTH_CTX:-32768}"
pids=()

start() { # name port extra-args...
  local name="$1" port="$2"; shift 2
  echo "aimee-llm: starting $name on :$port (ngl=$NGL)" >&2
  "$LLAMA" --host 127.0.0.1 --port "$port" -ngl "$NGL" "$@" >&2 &
  pids+=("$!")
}

# STUB mode (CI/dev): no GGUFs, no llama-servers — the gateway serves
# deterministic embed/rerank/synth. Lets e2e exercise the kb->gateway contract
# cheaply (and the image can be built without baking models).
case "${AIMEE_LLM_STUB:-}" in
  ""|0|false)
    # Embedder: one vector per input, no prompt-cache fragmentation (P2 flags).
    start embed 8081 -m /models/embed.gguf --embeddings --pooling "$POOL" \
      --ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots
    # Reranker ENCODER: CLS pooling + flash-attn; the gateway applies the Dense head.
    start rerank 8082 -m /models/rerank-encoder.gguf --embeddings --pooling cls -fa on
    # Synth: OpenAI-compatible /v1/chat/completions (grammar/JSON via --jinja). The
    # synth MODEL + its runtime profile is baked per TIER (build-args -> ENV):
    #   aimee-kb-cpu       gemma-4-E4B     (dense, CPU)
    #   aimee-kb-gpu-small gemma-4-12B     (dense, GPU, FA+K8V4)
    #   aimee-kb-gpu-mid   Qwen3.6-35B-A3B (MoE, GPU, FA+K8V4 + static --n-cpu-moe
    #                      expert-offload so the 22GB synth co-fits embed+rerank on 24GB)
    # The gemma tiers leave FA/MoE unset -> identical to prior behaviour. AIMEE_LLM_
    # SYNTH_LOCAL=0 still lets an operator disable the local synth and forward via
    # AIMEE_LLM_SYNTH_URL instead.
    if [ "${AIMEE_LLM_SYNTH_LOCAL:-1}" != "0" ] && [ -f /models/synth.gguf ]; then
      synth_args=(-m /models/synth.gguf --ctx-size "$SYNTH_CTX" --jinja \
                  --parallel "${AIMEE_LLM_SYNTH_SLOTS:-1}")
      # Flash-attention + quantized KV (K8V4 by default). NOTE: quantized V-cache
      # REQUIRES fa (llama.cpp refuses otherwise), so a healthy start proves fa on.
      if [ "${AIMEE_LLM_SYNTH_FA:-off}" = "on" ]; then
        synth_args+=(-fa on \
          --cache-type-k "${AIMEE_LLM_SYNTH_KV_K:-q8_0}" \
          --cache-type-v "${AIMEE_LLM_SYNTH_KV_V:-q4_0}")
      fi
      # MoE static expert-offload to system RAM (mid tier). Clamp to [0, layers].
      if [ "${AIMEE_LLM_SYNTH_MOE:-0}" = "1" ]; then
        ncm="${AIMEE_LLM_SYNTH_N_CPU_MOE:-0}"; mlayers="${AIMEE_LLM_SYNTH_MOE_LAYERS:-40}"
        case "$ncm" in ''|*[!0-9]*) ncm=0;; esac
        [ "$ncm" -gt "$mlayers" ] && ncm=$mlayers
        synth_args+=(--n-cpu-moe "$ncm")
        echo "aimee-llm: synth MoE expert-offload --n-cpu-moe $ncm of $mlayers" >&2
      fi
      start synth 8083 "${synth_args[@]}"
    fi
    ;;
  *)
    echo "aimee-llm: STUB mode — skipping llama-servers; gateway serves deterministic responses" >&2
    ;;
esac

# Gateway (foreground-ish; backgrounded so we can reap any child exit).
echo "aimee-llm: starting gateway on :${AIMEE_LLM_PORT:-8080}" >&2
python3 /opt/aimee/aimee_llm_gateway.py >&2 &
pids+=("$!")

# If ANY supervised process exits, stop the rest and exit non-zero so the
# orchestrator restarts the container (restart isolation is the s6 follow-up).
wait -n "${pids[@]}"
code=$?
echo "aimee-llm: a supervised process exited ($code); shutting down" >&2
kill "${pids[@]}" 2>/dev/null
exit "${code:-1}"
