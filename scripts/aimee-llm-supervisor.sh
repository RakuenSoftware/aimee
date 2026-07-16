#!/bin/bash
# aimee-llm container supervisor: resolve each role's tier + backend mode, download
# only what it serves locally on first boot (the image ships model-less), then
# launch the per-role llama.cpp servers (embed, rerank-encoder, synth) and the
# gateway. Each role is its own llama-server process so a crash is isolated to that
# role; if any exits the supervisor tears the container down (the orchestrator
# restarts it). A full s6-overlay (per-role restart) is the follow-up.
#
# ROLE BACKEND (runtime, not baked): each role is independently
#   AIMEE_LLM_<ROLE>_MODE = local | external | off   (default local)
# and, when local, sized by
#   AIMEE_LLM_<ROLE>_TIER = cpu | small | mid | large (falls back to AIMEE_LLM_TIER,
#                                                      then cpu)
# so ONE instance can serve, e.g., a cpu embedder beside a gpu-large synth. NGL is
# forced 0 for a cpu-tier role and uses the deploy's AIMEE_LLM_NGL for a gpu one.
# Per-role tier -> model coordinates (embed/rerank collapse small/mid/large -> gpu;
# synth honors the full ladder):
#   cpu    0.6B emb + ettin-68m  + gemma-4-E4B      (dense, CPU)
#   small  4B emb   + ettin-400m + gemma-4-12B      (dense, GPU, FA+K8V4)
#   mid    4B emb   + ettin-400m + gemma-4-26B-A4B  (MoE, GPU, FA+K8V4; SLOTS=2)
#   large  SAME 26B-A4B as mid but SLOTS=4 (32GB card → deploy 4×256K)
# The resolvers below are the SINGLE source of truth (was Dockerfile ARGs + the
# CI build matrix). Embed+synth are plain GGUF pulls from HF; the ettin reranker
# is a pre-converted encoder GGUF + Dense head (head.npz) fetched from a GitHub
# release (published by .github/workflows/publish-rerank-artifacts.yml) — the ST
# score head doesn't survive GGUF conversion, so the gateway applies it.
set -u
# The llama.cpp server binary. Overridable so the role-decouple test can shim it
# (and so a custom install can point elsewhere); defaults to the image path.
LLAMA="${AIMEE_LLM_LLAMA_BIN:-/opt/llama/llama-server}"
NGL="${AIMEE_LLM_NGL:-0}"
MODELS_DIR="${AIMEE_LLM_MODELS_DIR:-/models}"
# Base URL for the pre-converted ettin rerank artifacts (override for a mirror).
RERANK_ASSET_BASE="${AIMEE_LLM_RERANK_ASSET_BASE:-https://github.com/RakuenSoftware/aimee/releases/download/rerank-artifacts-v1}"
# Synth context window. The synth GGUF (gemma) trains to 256K, but a hardcoded
# --ctx-size 8192 wasted that: large code symbols / doc chunks overran 8K and the
# server returned HTTP 400, failing extraction entirely. The synth context defaults
# PER TIER (synth_coords sets TIER_CTX; SYNTH_CTX is resolved from it once the tier
# is known): cpu/small 32K, mid 256K (2 slots ×128K), large 1024K (4 slots ×256K).
# KV cache scales with context and lives in VRAM under -ngl, so each tier's total is
# sized to its target card. AIMEE_LLM_SYNTH_CTX overrides per deploy (e.g. a 24GB
# card runs mid at 280000 = 4×70K). Embed/rerank inputs are small — they keep 8K.

# ---- per-role tier -> model coordinates -------------------------------------
# Each role's tier is chosen independently via AIMEE_LLM_<ROLE>_TIER, falling back
# to the instance-wide AIMEE_LLM_TIER, then cpu. ONE aimee-llm instance serves all
# of its local roles, each at its OWN tier — e.g. a cpu embedder beside a gpu-large
# synth on the same box. Embed/rerank have only cpu-vs-gpu variants (small/mid/large
# all resolve to the gpu model); synth honors the full size ladder. GPU offload is
# forced off for a cpu-tier role and uses $NGL for a gpu-tier one. Each role caches
# its GGUFs under $MODELS_DIR/<its-tier>, so roles sharing a tier share a dir (and a
# legacy all-in-one $MODELS_DIR/<tier> volume keeps working). Unknown tier fails fast.
EMBED_TIER="${AIMEE_LLM_EMBED_TIER:-${AIMEE_LLM_TIER:-cpu}}"
RERANK_TIER="${AIMEE_LLM_RERANK_TIER:-${AIMEE_LLM_TIER:-cpu}}"
SYNTH_TIER="${AIMEE_LLM_SYNTH_TIER:-${AIMEE_LLM_TIER:-cpu}}"

embed_coords() {
  case "$EMBED_TIER" in
    cpu)             EMBED_REPO="Qwen/Qwen3-Embedding-0.6B-GGUF"; EMBED_FILE="Qwen3-Embedding-0.6B-f16.gguf"; EMBED_NGL=0 ;;
    small|mid|large) EMBED_REPO="Qwen/Qwen3-Embedding-4B-GGUF";   EMBED_FILE="Qwen3-Embedding-4B-Q8_0.gguf";  EMBED_NGL="$NGL" ;;
    *) echo "aimee-llm: invalid AIMEE_LLM_EMBED_TIER='$EMBED_TIER' (cpu, small, mid, large)" >&2; exit 1 ;;
  esac
  EMBED_D="$MODELS_DIR/$EMBED_TIER"
}
rerank_coords() {
  case "$RERANK_TIER" in
    cpu)             RERANK_SIZE="68m";  RERANK_NGL=0 ;;
    small|mid|large) RERANK_SIZE="400m"; RERANK_NGL="$NGL" ;;
    *) echo "aimee-llm: invalid AIMEE_LLM_RERANK_TIER='$RERANK_TIER' (cpu, small, mid, large)" >&2; exit 1 ;;
  esac
  RERANK_D="$MODELS_DIR/$RERANK_TIER"
}
synth_coords() {
  case "$SYNTH_TIER" in
    cpu)
      SYNTH_REPO="ggml-org/gemma-4-E4B-it-GGUF"; SYNTH_FILE="gemma-4-E4B-it-Q4_K_M.gguf"
      SYNTH_REVISION="main"; SYNTH_SHA256=""
      TIER_FA="off"; TIER_MOE="0"; TIER_N_CPU_MOE="0"; TIER_SLOTS="1"; TIER_CTX="32768"; SYNTH_NGL=0
      ;;
    small)
      SYNTH_REPO="unsloth/gemma-4-12B-it-qat-GGUF"; SYNTH_FILE="gemma-4-12B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="9586f3257bc62bd179767241c7ec0f66bc2a314a"
      SYNTH_SHA256="cc9ff072e0a8203429ed854e6662c17a6c2bc1e5dca5b475dd4736caaacbc165"
      TIER_FA="on"; TIER_MOE="0"; TIER_N_CPU_MOE="0"; TIER_SLOTS="1"; TIER_CTX="32768"; SYNTH_NGL="$NGL"
      ;;
    mid)
      SYNTH_REPO="unsloth/gemma-4-26B-A4B-it-qat-GGUF"; SYNTH_FILE="gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="02749a7b272109255a4c559a80894d3d9777574c"
      SYNTH_SHA256="dcf179a91153e3a7ece792e48ef872180d9d6ef9b7677f0a0bd3e83cfe624d5e"
      TIER_FA="on"; TIER_MOE="1"; TIER_N_CPU_MOE="0"; TIER_SLOTS="2"; TIER_CTX="256000"; SYNTH_NGL="$NGL"
      ;;
    large)
      # Byte-identical to mid EXCEPT SLOTS=4 + a 1024K ctx (24GB→32GB card headroom).
      SYNTH_REPO="unsloth/gemma-4-26B-A4B-it-qat-GGUF"; SYNTH_FILE="gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf"
      SYNTH_REVISION="02749a7b272109255a4c559a80894d3d9777574c"
      SYNTH_SHA256="dcf179a91153e3a7ece792e48ef872180d9d6ef9b7677f0a0bd3e83cfe624d5e"
      TIER_FA="on"; TIER_MOE="1"; TIER_N_CPU_MOE="0"; TIER_SLOTS="4"; TIER_CTX="1024000"; SYNTH_NGL="$NGL"
      ;;
    *) echo "aimee-llm: invalid AIMEE_LLM_SYNTH_TIER='$SYNTH_TIER' (cpu, small, mid, large)" >&2; exit 1 ;;
  esac
  SYNTH_D="$MODELS_DIR/$SYNTH_TIER"
}
embed_coords
rerank_coords
synth_coords

# Synth context: an explicit AIMEE_LLM_SYNTH_CTX wins; otherwise the tier default
# (TIER_CTX) set in synth_coords. Resolved here so the tier is already known.
SYNTH_CTX="${AIMEE_LLM_SYNTH_CTX:-$TIER_CTX}"

# ---- per-role backend mode (local | external | off) -------------------------
# Each LLM role is independently local (a baked llama-server), external (the
# gateway forwards to an operator-provided OpenAI-compatible upstream), or off
# (the role is gated). Default is `local` for every role => byte-for-byte the
# prior all-local behavior. A role that is NOT local is neither downloaded nor
# launched: the plugin fetches and serves ONLY the roles it hosts. When every
# role is external/off the supervisor pulls nothing and starts no llama-server —
# the gateway is a pure forwarder (the deploy layer normally skips deploying the
# plugin at all in that case).
EMBED_MODE="${AIMEE_LLM_EMBED_MODE:-local}"
RERANK_MODE="${AIMEE_LLM_RERANK_MODE:-local}"
SYNTH_MODE="${AIMEE_LLM_SYNTH_MODE:-local}"
# Back-compat: the older AIMEE_LLM_SYNTH_LOCAL=0 knob means "external synth".
if [ "${AIMEE_LLM_SYNTH_LOCAL:-1}" = "0" ] && [ -z "${AIMEE_LLM_SYNTH_MODE:-}" ]; then
  SYNTH_MODE="external"
fi
validate_mode() { # env-name value
  case "$2" in
    local|external|off) ;;
    *) echo "aimee-llm: invalid $1='$2' (valid: local, external, off)" >&2; exit 1;;
  esac
}
validate_mode AIMEE_LLM_EMBED_MODE "$EMBED_MODE"
validate_mode AIMEE_LLM_RERANK_MODE "$RERANK_MODE"
validate_mode AIMEE_LLM_SYNTH_MODE "$SYNTH_MODE"

# Deploy-time overrides win over the synth tier default (operators set CTX/SLOTS per card).
FA="${AIMEE_LLM_SYNTH_FA:-$TIER_FA}"
KV_K="${AIMEE_LLM_SYNTH_KV_K:-q8_0}"
KV_V="${AIMEE_LLM_SYNTH_KV_V:-q4_0}"
MOE="${AIMEE_LLM_SYNTH_MOE:-$TIER_MOE}"
MOE_LAYERS="${AIMEE_LLM_SYNTH_MOE_LAYERS:-40}"
N_CPU_MOE="${AIMEE_LLM_SYNTH_N_CPU_MOE:-$TIER_N_CPU_MOE}"
SLOTS="${AIMEE_LLM_SYNTH_SLOTS:-$TIER_SLOTS}"
# Host-RAM prompt-cache cap (MiB). llama-server's default is 8192 (8 GiB) of
# ANONYMOUS host RAM for its server-side prompt/checkpoint cache — independent of
# the KV cache, which lives in VRAM under -ngl. On a modest appliance (e.g. a 16 GB
# box also hosting postgres + kb + server) that 8 GiB default triggers a host OOM
# that kills a postgres backend. Cap it low by default; operators with RAM to spare
# can raise AIMEE_LLM_SYNTH_CACHE_RAM. The embedder already runs --cache-ram 0.
SYNTH_CACHE_RAM="${AIMEE_LLM_SYNTH_CACHE_RAM:-2048}"

# Gateway-facing model identity (all current tiers share the qwen3 embedder id +
# last-token pooling — the gateway reads these for /health + the drift guard).
export AIMEE_LLM_EMBED_MODEL="${AIMEE_LLM_EMBED_MODEL:-qwen3-embedding}"
export AIMEE_LLM_EMBED_POOLING="${AIMEE_LLM_EMBED_POOLING:-last}"
# AIMEE_LLM_RERANK_HEAD is set per-mode in resolve_upstreams (local rerank only).
POOL="$AIMEE_LLM_EMBED_POOLING"

pids=()

start() { # name port extra-args...   (caller passes -ngl per role: cpu=0, gpu=$NGL)
  local name="$1" port="$2"; shift 2
  echo "aimee-llm: starting $name on :$port" >&2
  "$LLAMA" --host 127.0.0.1 --port "$port" "$@" >&2 &
  pids+=("$!")
}

# fetch URL DEST — download with retries; return non-zero on failure (the caller
# leaves .ready absent so the next start retries a partial pull).
fetch() {
  local url="$1" dest="$2"
  # wget when available (the standalone aimee-llm image), else curl (the combined
  # image ships curl only — its runtime stage has no wget, and a silent hard
  # dependency here meant every model fetch failed there).
  if command -v wget >/dev/null 2>&1; then
    wget -q -c --tries=5 --timeout=30 --waitretry=10 -O "$dest" "$url" \
      || { echo "aimee-llm: download FAILED: $url" >&2; return 1; }
  else
    curl -fsSL --connect-timeout 30 --retry 5 --retry-delay 10 -C - -o "$dest" "$url" \
      || { echo "aimee-llm: download FAILED: $url" >&2; return 1; }
  fi
}

# download_models — fetch ONLY the roles served locally, each into its own tier's
# cache dir ($MODELS_DIR/<role-tier>) and guarded by its own .<role>.ready marker,
# so switching one role's tier/mode never re-pulls the others. Embed+synth pull
# straight from HF; the rerank encoder+head come from the GH release, everything
# sha256-verified where a checksum is known.
download_models() {
  # Migrate the legacy single all-roles marker: a volume provisioned before the
  # per-role split has <dir>/.ready (all three fetched together at that tier).
  # Honor it per role so an upgrade does not needlessly re-download multi-GB GGUFs.
  [ -f "$EMBED_D/.ready" ]  && touch "$EMBED_D/.embed.ready"
  [ -f "$SYNTH_D/.ready" ]  && touch "$SYNTH_D/.synth.ready"
  [ -f "$RERANK_D/.ready" ] && touch "$RERANK_D/.rerank.ready"

  # Embedder GGUF (repo default branch) — local embed only.
  if [ "$EMBED_MODE" = "local" ] && [ ! -f "$EMBED_D/.embed.ready" ]; then
    echo "aimee-llm: fetching embed tier '$EMBED_TIER' into $EMBED_D" >&2
    mkdir -p "$EMBED_D" || return 1
    fetch "https://huggingface.co/${EMBED_REPO}/resolve/main/${EMBED_FILE}" "$EMBED_D/embed.gguf" || return 1
    touch "$EMBED_D/.embed.ready"
  fi

  # Synth GGUF, pinned to the tier's HF revision; sha256-verified when known so a
  # compromised/MITM'd upload can't ship — local synth only.
  if [ "$SYNTH_MODE" = "local" ] && [ ! -f "$SYNTH_D/.synth.ready" ]; then
    echo "aimee-llm: fetching synth tier '$SYNTH_TIER' into $SYNTH_D" >&2
    mkdir -p "$SYNTH_D" || return 1
    fetch "https://huggingface.co/${SYNTH_REPO}/resolve/${SYNTH_REVISION}/${SYNTH_FILE}" "$SYNTH_D/synth.gguf" || return 1
    if [ -n "$SYNTH_SHA256" ]; then
      echo "${SYNTH_SHA256}  $SYNTH_D/synth.gguf" | sha256sum -c - >&2 || {
        echo "aimee-llm: synth.gguf sha256 MISMATCH — refusing to serve" >&2; return 1; }
    fi
    touch "$SYNTH_D/.synth.ready"
  fi

  # Pre-converted ettin rerank artifacts (encoder GGUF + Dense head npz) from the
  # GH release, verified against its SHA256SUMS — local rerank only.
  if [ "$RERANK_MODE" = "local" ] && [ ! -f "$RERANK_D/.rerank.ready" ]; then
    echo "aimee-llm: fetching rerank tier '$RERANK_TIER' into $RERANK_D" >&2
    mkdir -p "$RERANK_D/rerank-head" || return 1
    local rr="rerank-ettin-${RERANK_SIZE}"
    fetch "${RERANK_ASSET_BASE}/${rr}.gguf"     "$RERANK_D/rerank-encoder.gguf"   || return 1
    fetch "${RERANK_ASSET_BASE}/${rr}.head.npz" "$RERANK_D/rerank-head/head.npz"  || return 1
    if fetch "${RERANK_ASSET_BASE}/SHA256SUMS" "$RERANK_D/SHA256SUMS"; then
      ( cd "$RERANK_D" \
        && grep -E "  ${rr}\.gguf\$" SHA256SUMS | sed "s#${rr}\.gguf#rerank-encoder.gguf#" | sha256sum -c - \
        && grep -E "  ${rr}\.head\.npz\$" SHA256SUMS | sed "s#${rr}\.head\.npz#rerank-head/head.npz#" | sha256sum -c - ) >&2 || {
        echo "aimee-llm: rerank artifact sha256 MISMATCH — refusing to serve" >&2; return 1; }
    else
      echo "aimee-llm: WARNING — no SHA256SUMS for rerank artifacts (unverified)" >&2
    fi
    touch "$RERANK_D/.rerank.ready"
  fi

  echo "aimee-llm: model provisioning done (embed=$EMBED_MODE/$EMBED_TIER rerank=$RERANK_MODE/$RERANK_TIER synth=$SYNTH_MODE/$SYNTH_TIER)" >&2
}

# resolve_upstreams — point the gateway's per-role upstream at the right place for
# each mode: local => the baked llama-server; external => the operator's upstream
# (must be set to a non-local base); off => empty so the gateway gates the role.
# The rerank Dense head is applied locally by the gateway, so only a LOCAL rerank
# encoder carries a head dir; external/off rerank clears it (gated).
resolve_upstreams() {
  resolve_one EMBED  "$EMBED_MODE"  "http://127.0.0.1:8081"
  resolve_one RERANK "$RERANK_MODE" "http://127.0.0.1:8082"
  resolve_one SYNTH  "$SYNTH_MODE"  "http://127.0.0.1:8083"
  if [ "$RERANK_MODE" = "local" ]; then
    export AIMEE_LLM_RERANK_HEAD="$RERANK_D/rerank-head"
  else
    export AIMEE_LLM_RERANK_HEAD=""
  fi
}
resolve_one() { # ROLE MODE localurl
  local role="$1" mode="$2" localurl="$3" var="AIMEE_LLM_${1}_URL" cur
  eval "cur=\${$var:-}"
  case "$mode" in
    local) export "$var=$localurl" ;;
    external)
      if [ -z "$cur" ] || [ "$cur" = "$localurl" ]; then
        echo "aimee-llm: ${role} mode=external requires $var set to the external upstream base" >&2
        exit 1
      fi ;;
    off) export "$var=" ;;
  esac
}

# DOWNLOAD-ONLY mode (build-time pre-bake): fetch the resolved tiers' models into
# $MODELS_DIR and exit without launching anything. Dockerfile.aimee-llm uses this
# (AIMEE_LLM_BAKE_TIER) to bake the cpu tier into the aimee-llm-cpu image, keeping
# the tier table here as the single source of truth instead of duplicating model
# URLs in the Dockerfile.
if [ "${AIMEE_LLM_DOWNLOAD_ONLY:-0}" = "1" ]; then
  download_models || { echo "aimee-llm: pre-bake download failed" >&2; exit 1; }
  exit 0
fi

# STUB mode (CI/dev): no GGUFs, no downloads, no llama-servers — the gateway serves
# deterministic embed/rerank/synth. Lets e2e exercise the kb->gateway contract
# cheaply (and the image runs without any model fetch).
case "${AIMEE_LLM_STUB:-}" in
  ""|0|false)
    download_models || { echo "aimee-llm: model provisioning failed; shutting down" >&2; exit 1; }
    # Embedder: one vector per input, no prompt-cache fragmentation (P2 flags).
    if [ "$EMBED_MODE" = "local" ]; then
      start embed 8081 -ngl "$EMBED_NGL" -m "$EMBED_D/embed.gguf" --embeddings --pooling "$POOL" \
        --ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots
    fi
    # Reranker ENCODER: CLS pooling + flash-attn; the gateway applies the Dense head.
    if [ "$RERANK_MODE" = "local" ]; then
      start rerank 8082 -ngl "$RERANK_NGL" -m "$RERANK_D/rerank-encoder.gguf" --embeddings --pooling cls -fa on
    fi
    # Synth: OpenAI-compatible /v1/chat/completions (grammar/JSON via --jinja). The
    # synth MODEL + its runtime profile come from the synth tier above; explicit
    # AIMEE_LLM_SYNTH_* envs still override per deploy.
    if [ "$SYNTH_MODE" = "local" ] && [ -f "$SYNTH_D/synth.gguf" ]; then
      synth_args=(-ngl "$SYNTH_NGL" -m "$SYNTH_D/synth.gguf" --ctx-size "$SYNTH_CTX" --jinja \
        --parallel "$SLOTS" --cache-ram "$SYNTH_CACHE_RAM")
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

# Point the gateway at each role's resolved upstream (local server / external base
# / gated) now that the local servers, if any, are launching.
resolve_upstreams

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
