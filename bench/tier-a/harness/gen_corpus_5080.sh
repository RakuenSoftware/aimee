#!/bin/bash
# Host a generator on the idle 5080 and build the independent corpus.
#
# gemma-4-26B-A4B at UD-Q4_K_XL: a mixture of experts, 26B total for quality and
# about 4B active for speed, which is what makes a 26B-class generator fit a
# 16 GiB card at all. All experts stay resident, so this is ~15 GiB of VRAM
# rather than the ~2.5 GiB an active-parameter count would suggest -- the same
# arithmetic that stopped LFM2.5-8B-A1B running at three processes.
#
# NOTE ON INDEPENDENCE, since the plan asks for a generator outside the field
# under test. This is gemma-4, and gemma-4-E2B and E4B are the top rows of the
# head-to-head, so generator and subject share a family. I raised that and it was
# overruled, and my proposed alternative was no better: Qwen3-1.7B is also in the
# field, so a Qwen generator carried the same property. The honest position is
# that the corpus varies inventory, phrasing and generator SIZE and ARCHITECTURE
# from v5, and does not vary family. The registered acceptance test is a quant
# comparison WITHIN one model (E2B Q4 against Q6), where any family effect lands
# on both arms equally, so the test it was built for is unaffected. A cross-model
# ranking on this corpus would not be.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=data/corpora/v6
mkdir -p "$OUT" .scratch
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/gen.log"; }

HOST=root@192.168.1.253
EP=192.168.0.5
PORT=8991
BIN=/opt/llama.cpp/build-cuda/bin/llama-server
HFH=/opt/hf
LOGDIR=/opt/tierA
REPO=unsloth/gemma-4-26B-A4B-it-GGUF:UD-Q4_K_XL

say "=== starting generator $REPO on the 5080, port $PORT"
ssh -n -o ConnectTimeout=25 $HOST \
  "pct exec 140 -- bash -lc 'HF_HOME=$HFH nohup setsid $BIN -hf $REPO --host 0.0.0.0 --port $PORT -c 4096 -np 1 --no-webui --no-mmproj -ngl 99 >$LOGDIR/gen-$PORT.log 2>&1 </dev/null &'" >/dev/null 2>&1

for i in $(seq 1 140); do
  ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$EP:$PORT/health" >/dev/null 2>&1 && break
  sleep 15
done
ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$EP:$PORT/health" >/dev/null 2>&1 \
  || { say "FAIL generator never became healthy"; exit 1; }

LOADED=$(ssh -n $HOST "curl -s http://$EP:$PORT/props" \
  | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])")
say "    healthy, loaded $LOADED"
case "$LOADED" in
  *gemma-4-26B-A4B*) ;;
  *) say "IDENTITY GUARD FAILED: loaded $LOADED"; exit 1 ;;
esac

pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
  -L "$PORT:$EP:$PORT" "$HOST" >/dev/null 2>&1 </dev/null &
sleep 6
curl -sf --max-time 5 "http://127.0.0.1:$PORT/health" >/dev/null || { say "FAIL tunnel did not open"; exit 1; }

say "=== generating 1001 notes, gold first then surface text"
python3 harness/gen_corpus_v2.py --base-url "http://127.0.0.1:$PORT" \
  --out "$OUT/gold_small.jsonl" --n 1001 2>&1 | tee -a "$OUT/gen.log"

say "=== independence check: entity overlap against corpus v5"
python3 harness/corpus_overlap.py 2>&1 | tee -a "$OUT/gen.log"
say "=== CORPUS V6 GENERATION COMPLETE ==="
