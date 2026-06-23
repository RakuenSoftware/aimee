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
pids=()

start() { # name port extra-args...
  local name="$1" port="$2"; shift 2
  echo "aimee-llm: starting $name on :$port (ngl=$NGL)" >&2
  "$LLAMA" --host 127.0.0.1 --port "$port" -ngl "$NGL" "$@" >&2 &
  pids+=("$!")
}

# Embedder: one vector per input, no prompt-cache fragmentation (P2 flags).
start embed 8081 -m /models/embed.gguf --embeddings --pooling "$POOL" \
  --ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots
# Reranker ENCODER: CLS pooling + flash-attn; the gateway applies the Dense head.
start rerank 8082 -m /models/rerank-encoder.gguf --embeddings --pooling cls -fa on
# Synth: OpenAI-compatible /v1/chat/completions (grammar/JSON via --jinja).
if [ -f /models/synth.gguf ]; then
  start synth 8083 -m /models/synth.gguf --ctx-size 8192 --jinja
fi

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
