#!/bin/bash
# Drain throughput on CPU: server with parallel slots, concurrent requests,
# warm shared prefix. This is the number that decides CPU viability, not the
# single-request latency the first pass measured.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build/bin/llama-server}
OUT=results/cpu-deep
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8090}
THREADS=${THREADS:-20}

MODELS="
ggml-org/gemma-4-E2B-it-GGUF:Q4_K_M
ibm-granite/granite-4.0-1b-GGUF:Q4_K_M
ggml-org/gemma-4-E2B-it-GGUF:Q8_0
"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/:.' '___')
  JSON="$OUT/throughput_$SLUG.json"
  [ -s "$JSON" ] && { echo "SKIP $M"; continue; }
  echo "=== SERVE $M (CPU, -t $THREADS, 8 slots) ==="
  # --parallel 8 gives the server room to batch; a large context holds the slots.
  taskset -c "0-$((THREADS-1))" "$SERVER" -hf "$M" --port "$PORT" -ngl 0 \
      -t "$THREADS" --parallel 8 -c 16384 --no-webui > "$OUT/$SLUG.server.log" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 200); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" = 1 ]; then
    $PY harness/bench_cpu_throughput.py --base-url "http://127.0.0.1:$PORT" \
        --model "$M" --gold data/gold.jsonl --concurrency 1 4 8 \
        --out "$JSON" && echo "OK   $M" || echo "FAIL $M"
  else
    echo "FAIL $M -> server never healthy: $(tail -2 "$OUT/$SLUG.server.log"|tr '\n' ' '|cut -c1-140)"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
done
echo "SWEEP_CPU_THROUGHPUT_DONE"
