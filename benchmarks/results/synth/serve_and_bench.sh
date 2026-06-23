#!/bin/bash
# sg4.sh MODELFILE MODE LABEL — serve one model, run grammar-enforced corpus harness, kill.
set -u
NAME="$1"; MODE="${2:-gpu}"; LABEL="${3:-$1}"
DIR=/mnt/media/synthbench
BIN=$DIR/llama-b9761/llama-server; LIB=$DIR/llama-b9761
M=$DIR/models/${NAME}.gguf
PORT=8920
[ "$MODE" = cpu ] && NGL=0 || NGL=99
pkill -9 -u "$(id -un)" -f "llama-server.*$PORT" 2>/dev/null; sleep 2
LD_LIBRARY_PATH=$LIB nohup "$BIN" -m "$M" -ngl $NGL -fa on --jinja --ctx-size 8192 -ub 2048 -np 1 \
  --host 127.0.0.1 --port $PORT > "$DIR/srv_${LABEL}.log" 2>&1 &
SRV=$!
ok=0
for i in $(seq 1 150); do sleep 2; curl -s http://127.0.0.1:$PORT/health 2>/dev/null | grep -q ok && { ok=1; break; }; done
if [ "$ok" = 0 ]; then echo "SERVER FAILED ($LABEL): $(tail -3 $DIR/srv_${LABEL}.log)"; kill $SRV 2>/dev/null; pkill -9 -u "$(id -un)" -f "llama-server.*$PORT" 2>/dev/null; exit 1; fi
python3 "$DIR/synth_bench.py" $PORT "$LABEL" "$LABEL"
kill $SRV 2>/dev/null; pkill -9 -u "$(id -un)" -f "llama-server.*$PORT" 2>/dev/null
echo "SG4DONE_${LABEL}"
