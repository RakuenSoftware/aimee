#!/usr/bin/env bash
# test-embedder-qwen.sh — stand up a small, REAL Qwen3-Embedding-0.6B (1024-d)
# embedder for e2e testing, exposing aimee's embedder contract (POST /embed with
# raw text -> bare JSON float array). It runs llama.cpp `llama-server` on the GGUF
# and fronts it with scripts/_embed_shim.py (stdlib only — no torch, no pip).
#
# Why a real model and not the deterministic EMBEDDER_STUB: the stub proves the
# kb -> embedder -> pgvector wiring at the right dim but produces meaningless
# vectors, so it can't catch a semantic-retrieval regression. A small Qwen3
# embedder gives genuine 1024-d vectors cheaply (matches the CPU-default corpus).
#
# On success prints two lines to stdout (consumed by callers), then blocks:
#   EMBEDDER_URL=http://127.0.0.1:<shim_port>
#   EMBEDDER_DIM=1024
# It keeps llama-server + the shim running until it receives SIGINT/SIGTERM.
#
# Env:
#   AIMEE_E2E_EMBEDDER_CACHE  cache dir for the binary + GGUF (default ~/.cache/aimee-e2e-embedder)
#   LLAMA_PORT                llama-server port (default 8899)
#   SHIM_PORT                 /embed shim port (default 8080)
#   READY_TIMEOUT             seconds to wait for the model to load (default 180)
set -euo pipefail

CACHE="${AIMEE_E2E_EMBEDDER_CACHE:-$HOME/.cache/aimee-e2e-embedder}"
LLAMA_PORT="${LLAMA_PORT:-8899}"
SHIM_PORT="${SHIM_PORT:-8080}"
READY_TIMEOUT="${READY_TIMEOUT:-180}"
GGUF_REPO="Qwen/Qwen3-Embedding-0.6B-GGUF"
GGUF_FILE="Qwen3-Embedding-0.6B-Q8_0.gguf"
DIM=1024
HERE="$(cd "$(dirname "$0")" && pwd)"

log() { printf '[qwen-embedder] %s\n' "$*" >&2; }

mkdir -p "$CACHE"

# --- 1) llama-server binary (prebuilt ubuntu-x64 release; static, no pip) ------
LLAMA_BIN="$(find "$CACHE" -name llama-server -type f 2>/dev/null | head -1 || true)"
if [[ -z "$LLAMA_BIN" ]]; then
  log "fetching llama.cpp ubuntu-x64 (CPU) release"
  # The plain CPU asset is llama-<tag>-bin-ubuntu-x64.tar.gz; the vulkan/rocm/sycl/
  # openvino variants carry an extra token (ubuntu-vulkan-x64, ...) so anchoring on
  # '-bin-ubuntu-x64.tar.gz' selects the CPU build uniquely.
  # GitHub's `latest` release can be a metadata-only tag (v0.3.0 currently)
  # with no binaries. Read a bounded releases page and select the newest exact
  # CPU asset, excluding Vulkan/ROCm/SYCL/OpenVINO variants by filename.
  releases_json="$CACHE/llama-releases.json"
  curl -fsSL 'https://api.github.com/repos/ggml-org/llama.cpp/releases?per_page=10' \
    -o "$releases_json"
  url="$(python3 - "$releases_json" <<'PY'
import json
import re
import sys

for release in json.load(open(sys.argv[1], encoding="utf-8")):
    for asset in release.get("assets", []):
        if re.fullmatch(r"llama-[^-]+-bin-ubuntu-x64\.tar\.gz", asset.get("name", "")):
            print(asset["browser_download_url"])
            raise SystemExit(0)
raise SystemExit(1)
PY
)"
  [[ -n "$url" ]] || { log "could not resolve a llama.cpp CPU release URL"; exit 1; }
  log "url=$url"
  curl -fsSL -o "$CACHE/llama.tar.gz" "$url"
  rm -rf "$CACHE/llama" && mkdir -p "$CACHE/llama"
  tar -xzf "$CACHE/llama.tar.gz" -C "$CACHE/llama"
  LLAMA_BIN="$(find "$CACHE/llama" -name llama-server -type f | head -1)"
  [[ -n "$LLAMA_BIN" ]] || { log "llama-server not found in release tarball"; exit 1; }
fi
LLAMA_LIBDIR="$(dirname "$LLAMA_BIN")"
log "llama-server: $LLAMA_BIN"

# --- 2) GGUF (small, cached) ---------------------------------------------------
if [[ ! -f "$CACHE/$GGUF_FILE" ]]; then
  log "downloading $GGUF_FILE (~610MB, one-time)"
  curl -fsSL -o "$CACHE/$GGUF_FILE.part" \
       "https://huggingface.co/$GGUF_REPO/resolve/main/$GGUF_FILE"
  mv "$CACHE/$GGUF_FILE.part" "$CACHE/$GGUF_FILE"
fi

# --- 3) launch llama-server (embeddings) + shim, tear both down on exit --------
llama_pid=""; shim_pid=""
cleanup() { [[ -n "$shim_pid" ]] && kill "$shim_pid" 2>/dev/null || true
            [[ -n "$llama_pid" ]] && kill "$llama_pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

log "starting llama-server on :$LLAMA_PORT"
LD_LIBRARY_PATH="$LLAMA_LIBDIR:${LD_LIBRARY_PATH:-}" "$LLAMA_BIN" \
  -m "$CACHE/$GGUF_FILE" --embeddings --pooling last \
  --host 127.0.0.1 --port "$LLAMA_PORT" --ctx-size 8192 -ub 8192 \
  >"$CACHE/llama-server.log" 2>&1 &
llama_pid=$!

deadline=$((SECONDS + READY_TIMEOUT))
until curl -fsS --max-time 3 "http://127.0.0.1:$LLAMA_PORT/health" >/dev/null 2>&1; do
  kill -0 "$llama_pid" 2>/dev/null || { log "llama-server exited early:"; tail -5 "$CACHE/llama-server.log" >&2; exit 1; }
  (( SECONDS < deadline )) || { log "llama-server not ready within ${READY_TIMEOUT}s"; exit 1; }
  sleep 2
done
log "llama-server ready"

log "starting /embed shim on :$SHIM_PORT"
python3 "$HERE/_embed_shim.py" "$LLAMA_PORT" "$SHIM_PORT" "$DIM" "Qwen3-Embedding-0.6B" &
shim_pid=$!
until curl -fsS --max-time 3 "http://127.0.0.1:$SHIM_PORT/health" >/dev/null 2>&1; do
  kill -0 "$shim_pid" 2>/dev/null || { log "shim exited early"; exit 1; }
  (( SECONDS < deadline )) || { log "shim not ready"; exit 1; }
  sleep 1
done

echo "EMBEDDER_URL=http://127.0.0.1:$SHIM_PORT"
echo "EMBEDDER_DIM=$DIM"
log "ready: real Qwen3-Embedding-0.6B (${DIM}-d) at http://127.0.0.1:$SHIM_PORT/embed"
wait "$llama_pid"
