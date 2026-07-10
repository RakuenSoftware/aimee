#!/bin/bash
# aimee-llm container supervisor: resolve the TIER, download its models on first
# boot (the image ships model-less), then launch the per-role llama.cpp servers
# (embed, rerank-encoder, synth) and the gateway. Each role is its own
# llama-server process so a crash is isolated to that role; if any exits the
# supervisor tears the container down (the orchestrator restarts it). A full
# s6-overlay (per-role restart) is the follow-up; this is the MVP supervisor.
#
# TIER SELECTION (runtime, not baked): AIMEE_LLM_TIER picks WHICH models to fetch
# and how to serve the synth. NGL (GPU offload) is a SEPARATE per-deploy knob.
#   (unset)/cpu  0.6B emb + ettin-68m  + gemma-4-E4B      (dense, CPU)
#   small        4B emb   + ettin-400m + gemma-4-12B      (dense, GPU, FA+K8V4)
#   mid          4B emb   + ettin-400m + gemma-4-26B-A4B  (MoE, GPU, FA+K8V4; SLOTS=2)
#   large        SAME 26B-A4B as mid but SLOTS=4 (32GB card → deploy 4×256K)
# The tier table below is the SINGLE source of truth (was Dockerfile ARGs + the
# CI build matrix). Embed+synth are plain GGUF pulls from HF; the ettin reranker
# is a pre-converted encoder GGUF + Dense head (head.npz) fetched from a GitHub
# release (published by .github/workflows/publish-rerank-artifacts.yml) — the ST
# score head doesn't survive GGUF conversion, so the gateway applies it.
set -u
LLAMA=/opt/llama/llama-server
NGL="${AIMEE_LLM_NGL:-0}"
MODELS_DIR="${AIMEE_LLM_MODELS_DIR:-/models}"
# Base URL for the pre-converted ettin rerank artifacts (override for a mirror).
RERANK_ASSET_BASE="${AIMEE_LLM_RERANK_ASSET_BASE:-https://github.com/RakuenSoftware/aimee/releases/download/rerank-artifacts-v1}"
# Synth context window. The synth GGUF (gemma) trains to 256K, but a hardcoded
# --ctx-size 8192 wasted that: large code symbols / doc chunks overran 8K and the
# server returned HTTP 400, failing extraction entirely. Default to a 16GB-VRAM-
# safe 32K (4x the old cap, covers real source files); raise it on bigger cards
# via AIMEE_LLM_SYNTH_CTX (e.g. 131072 for 128K). KV cache scales with context, so
# size it to the card. Embed/rerank inputs are small — they keep the 8K default.
SYNTH_CTX="${AIMEE_LLM_SYNTH_CTX:-32768}"

# ---- tier table (single source of truth) ------------------------------------
# Resolve AIMEE_LLM_TIER into the per-role model coordinates + the synth runtime
# profile. Empty tier ≡ cpu. Unknown tier fails fast.
TIER="${AIMEE_LLM_TIER:-cpu}"
tier_config() {
  case "$TIER" in
    cpu)
      EMBED_REPO="Qwen/Qwen3-Embedding-0.6B-GGUF"; EMBED_FILE="Qwen3-Embedding-0.6B-f16.gguf"
      RERANK_SIZE="68m"
      SYNTH_REPO="ggml-org/gemma-4-E4B-it-GGUF"; SYNTH_FILE="gemma-4-E4B-it-Q4_K_M.gguf"
      SYNTH_REVISION="main"; SYNTH_SHA256=""
      TIER_FA="off"; TIER_MOE="0"; TIER_N_CPU_MOE="0"; TIER_SLOTS="1"
      ;;
    small)
      EMBED_REPO="Qwen/Qwen3-Embedding-4B-GGUF"; EMBED_FILE="Qwen3-Embedding-4B-Q8_0.gguf"
      RERANK_SIZE="400m"
      SYNTH_REPO="unsloth/gemma-4-12B-it-qat-GGUF"; SYNTH_FILE="gemma-4-12B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="9586f3257bc62bd179767241c7ec0f66bc2a314a"
      SYNTH_SHA256="cc9ff072e0a8203429ed854e6662c17a6c2bc1e5dca5b475dd4736caaacbc165"
      TIER_FA="on"; TIER_MOE="0"; TIER_N_CPU_MOE="0"; TIER_SLOTS="1"
      ;;
    mid)
      EMBED_REPO="Qwen/Qwen3-Embedding-4B-GGUF"; EMBED_FILE="Qwen3-Embedding-4B-Q8_0.gguf"
      RERANK_SIZE="400m"
      SYNTH_REPO="unsloth/gemma-4-26B-A4B-it-qat-GGUF"; SYNTH_FILE="gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="02749a7b272109255a4c559a80894d3d9777574c"
      SYNTH_SHA256="dcf179a91153e3a7ece792e48ef872180d9d6ef9b7677f0a0bd3e83cfe624d5e"
      TIER_FA="on"; TIER_MOE="1"; TIER_N_CPU_MOE="0"; TIER_SLOTS="2"
      ;;
    large)
      # Byte-identical to mid EXCEPT SLOTS=4 (24GB→32GB card headroom).
      EMBED_REPO="Qwen/Qwen3-Embedding-4B-GGUF"; EMBED_FILE="Qwen3-Embedding-4B-Q8_0.gguf"
      RERANK_SIZE="400m"
      SYNTH_REPO="unsloth/gemma-4-26B-A4B-it-qat-GGUF"; SYNTH_FILE="gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="02749a7b272109255a4c559a80894d3d9777574c"
      SYNTH_SHA256="dcf179a91153e3a7ece792e48ef872180d9d6ef9b7677f0a0bd3e83cfe624d5e"
      TIER_FA="on"; TIER_MOE="1"; TIER_N_CPU_MOE="0"; TIER_SLOTS="4"
      ;;
    *)
      echo "aimee-llm: invalid AIMEE_LLM_TIER='$TIER' (valid: cpu, small, mid, large; empty ≡ cpu)" >&2
      exit 1
      ;;
  esac
}
tier_config

# Per-tier model cache (persist /models to a volume so restarts don't re-fetch).
D="$MODELS_DIR/$TIER"

# Deploy-time overrides win over the tier default (operators set CTX/SLOTS per card).
FA="${AIMEE_LLM_SYNTH_FA:-$TIER_FA}"
KV_K="${AIMEE_LLM_SYNTH_KV_K:-q8_0}"
KV_V="${AIMEE_LLM_SYNTH_KV_V:-q4_0}"
MOE="${AIMEE_LLM_SYNTH_MOE:-$TIER_MOE}"
MOE_LAYERS="${AIMEE_LLM_SYNTH_MOE_LAYERS:-40}"
N_CPU_MOE="${AIMEE_LLM_SYNTH_N_CPU_MOE:-$TIER_N_CPU_MOE}"
SLOTS="${AIMEE_LLM_SYNTH_SLOTS:-$TIER_SLOTS}"

# Gateway-facing model identity (all current tiers share the qwen3 embedder id +
# last-token pooling — the gateway reads these for /health + the drift guard).
export AIMEE_LLM_EMBED_MODEL="${AIMEE_LLM_EMBED_MODEL:-qwen3-embedding}"
export AIMEE_LLM_EMBED_POOLING="${AIMEE_LLM_EMBED_POOLING:-last}"
export AIMEE_LLM_RERANK_HEAD="$D/rerank-head"
POOL="$AIMEE_LLM_EMBED_POOLING"

pids=()

start() { # name port extra-args...
  local name="$1" port="$2"; shift 2
  echo "aimee-llm: starting $name on :$port (ngl=$NGL)" >&2
  "$LLAMA" --host 127.0.0.1 --port "$port" -ngl "$NGL" "$@" >&2 &
  pids+=("$!")
}

# fetch URL DEST — download with retries; return non-zero on failure (the caller
# leaves .ready absent so the next start retries a partial pull).
fetch() {
  local url="$1" dest="$2"
  wget -q -c --tries=5 --timeout=30 --waitretry=10 -O "$dest" "$url" \
    || { echo "aimee-llm: download FAILED: $url" >&2; return 1; }
}

# download_models — populate $D for the resolved tier ONCE (guarded by $D/.ready).
# Embed+synth pull straight from HF; the rerank encoder+head come from the GH
# release, and everything sha256-verified where a checksum is known.
download_models() {
  if [ -f "$D/.ready" ]; then
    echo "aimee-llm: tier '$TIER' models present in $D (skip download)" >&2
    return 0
  fi
  echo "aimee-llm: fetching tier '$TIER' models into $D" >&2
  mkdir -p "$D/rerank-head" || return 1

  # Embedder GGUF (repo default branch).
  fetch "https://huggingface.co/${EMBED_REPO}/resolve/main/${EMBED_FILE}" "$D/embed.gguf" || return 1

  # Synth GGUF, pinned to the tier's HF revision; sha256-verified when known so a
  # compromised/MITM'd upload can't ship.
  fetch "https://huggingface.co/${SYNTH_REPO}/resolve/${SYNTH_REVISION}/${SYNTH_FILE}" "$D/synth.gguf" || return 1
  if [ -n "$SYNTH_SHA256" ]; then
    echo "${SYNTH_SHA256}  $D/synth.gguf" | sha256sum -c - >&2 || {
      echo "aimee-llm: synth.gguf sha256 MISMATCH — refusing to serve" >&2; return 1; }
  fi

  # Pre-converted ettin rerank artifacts (encoder GGUF + Dense head npz) from the
  # GH release, verified against its SHA256SUMS.
  local rr="rerank-ettin-${RERANK_SIZE}"
  fetch "${RERANK_ASSET_BASE}/${rr}.gguf"     "$D/rerank-encoder.gguf"   || return 1
  fetch "${RERANK_ASSET_BASE}/${rr}.head.npz" "$D/rerank-head/head.npz"  || return 1
  if fetch "${RERANK_ASSET_BASE}/SHA256SUMS" "$D/SHA256SUMS"; then
    ( cd "$D" \
      && grep -E "  ${rr}\.gguf\$" SHA256SUMS | sed "s#${rr}\.gguf#rerank-encoder.gguf#" | sha256sum -c - \
      && grep -E "  ${rr}\.head\.npz\$" SHA256SUMS | sed "s#${rr}\.head\.npz#rerank-head/head.npz#" | sha256sum -c - ) >&2 || {
      echo "aimee-llm: rerank artifact sha256 MISMATCH — refusing to serve" >&2; return 1; }
  else
    echo "aimee-llm: WARNING — no SHA256SUMS for rerank artifacts (unverified)" >&2
  fi

  touch "$D/.ready"
  echo "aimee-llm: tier '$TIER' models ready in $D" >&2
}

# STUB mode (CI/dev): no GGUFs, no downloads, no llama-servers — the gateway serves
# deterministic embed/rerank/synth. Lets e2e exercise the kb->gateway contract
# cheaply (and the image runs without any model fetch).
case "${AIMEE_LLM_STUB:-}" in
  ""|0|false)
    download_models || { echo "aimee-llm: model provisioning failed; shutting down" >&2; exit 1; }
    # Embedder: one vector per input, no prompt-cache fragmentation (P2 flags).
    start embed 8081 -m "$D/embed.gguf" --embeddings --pooling "$POOL" \
      --ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots
    # Reranker ENCODER: CLS pooling + flash-attn; the gateway applies the Dense head.
    start rerank 8082 -m "$D/rerank-encoder.gguf" --embeddings --pooling cls -fa on
    # Synth: OpenAI-compatible /v1/chat/completions (grammar/JSON via --jinja). The
    # synth MODEL + its runtime profile come from the TIER table above; explicit
    # AIMEE_LLM_SYNTH_* envs still override per deploy. AIMEE_LLM_SYNTH_LOCAL=0 lets
    # an operator disable the local synth and forward via AIMEE_LLM_SYNTH_URL instead.
    if [ "${AIMEE_LLM_SYNTH_LOCAL:-1}" != "0" ] && [ -f "$D/synth.gguf" ]; then
      synth_args=(-m "$D/synth.gguf" --ctx-size "$SYNTH_CTX" --jinja --parallel "$SLOTS")
      # Flash-attention + quantized KV (K8V4 by default). NOTE: quantized V-cache
      # REQUIRES fa (llama.cpp refuses otherwise), so a healthy start proves fa on.
      if [ "$FA" = "on" ]; then
        synth_args+=(-fa on --cache-type-k "$KV_K" --cache-type-v "$KV_V")
      fi
      # MoE static expert-offload to system RAM (mid/large tiers). Clamp to [0, layers].
      if [ "$MOE" = "1" ]; then
        ncm="$N_CPU_MOE"; mlayers="$MOE_LAYERS"
        case "$ncm" in ''|*[!0-9]*) ncm=0;; esac
        [ "$ncm" -gt "$mlayers" ] && ncm=$mlayers
        synth_args+=(--n-cpu-moe "$ncm")
        echo "aimee-llm: synth MoE expert-offload --n-cpu-moe $ncm of $mlayers" >&2
      fi
      start synth 8083 "${synth_args[@]}"
    fi
    ;;
  *)
    echo "aimee-llm: STUB mode — skipping model fetch + llama-servers; gateway serves deterministic responses" >&2
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
