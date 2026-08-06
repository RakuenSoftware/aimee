#!/bin/bash
# gemma-4-26B-A4B unsloth QAT+UD on the local 5080.
#
# This is the second half of the pair whose first half is already banked: the
# google q4_0 build of the same QAT weights scored F1 0.6575 at n=1001. The
# question the pair answers is whether unsloth's dynamic quant applied ON TOP of
# Google's QAT weights beats Google's own flat q4_0 of them, which is the third
# quant scheme in this project and the only one never measured.
#
# It runs locally because it fits and because the rented attempts did not:
#   26B-A4B-it-qat-UD-Q4_K_XL   13.27 GiB
#   MTP draft Q8_0               0.43 GiB
#   free VRAM on the 5080       15.92 GiB
# That is tight but sufficient at -c 8192 -np 1, and the card is otherwise idle
# now that the 12B non-QAT arm has banked.
#
# The rented path had spent 86 minutes on a 5090 whose port never opened and 26
# minutes on a 3090 still loading, at $0.50/hr between them, to run a model the
# local card can hold. Moving it here is a reallocation, not an abandonment: the
# pool holding the arm is stopped FIRST so there is never a second client writing
# this prediction file, which is how four arms truncated each other last night.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=results/ct140
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/arms.log"; }

HOST=root@192.168.1.253
EP=$(ssh -n -o ConnectTimeout=15 $HOST "pct exec 140 -- hostname -I" 2>/dev/null | awk '{print $1}')
PORT=8992
BIN=/opt/llama.cpp/build-cuda/bin/llama-server
GOLD=data/corpora/v5/gold_small.jsonl
LBL=gemma-4-26B-A4B.qat-unsloth-UDQ4.5080
PRED="$OUT/$LBL.pred.jsonl"
REPO=unsloth/gemma-4-26B-A4B-it-qat-GGUF:UD-Q4_K_XL
DRAFT=unsloth/gemma-4-26B-A4B-it-qat-GGUF:MTP/mtp-gemma-4-26B-A4B-it-Q8_0.gguf

say "=== $LBL on the 5080 (CT140 at $EP)"

say "stopping whatever still holds VRAM from the finished 12B arm"
ssh -n -o ConnectTimeout=25 $HOST \
  "pct exec 140 -- bash -lc 'for p in \$(pgrep -f \"port $PORT\"); do kill \$p; done; sleep 5; true'" >/dev/null 2>&1
ssh -n -o ConnectTimeout=20 $HOST "nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader" 2>/dev/null | sed 's/^/  VRAM after stop: /'

say "starting the server"
ssh -n -o ConnectTimeout=25 $HOST \
  "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $PORT -c 8192 -np 1 --cache-ram 1024 --no-webui --no-mmproj -ngl 99 >/opt/tierA/arm-$LBL.log 2>&1 </dev/null &'" >/dev/null 2>&1

# Evidence only. No clock. The wait ends when the server answers, or when the
# server process is gone and its log says why. A 13 GiB download takes as long as
# it takes, and every deadline tried on this project abandoned a working host.
while true; do
  ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$EP:$PORT/health" >/dev/null 2>&1 && break
  if ! ssh -n -o ConnectTimeout=10 $HOST "pct exec 140 -- bash -lc 'pgrep -f \"port $PORT\" >/dev/null'" 2>/dev/null; then
    say "FAIL: server gone. Last lines of its log:"
    ssh -n $HOST "pct exec 140 -- bash -lc 'tail -12 /opt/tierA/arm-$LBL.log'" 2>&1 | tail -12
    exit 1
  fi
  sleep 20
done

# Defect 30: a server answering with someone else's weights. The unsloth and
# google builds of these QAT weights are the whole comparison, so loading the
# wrong one would produce a plausible number for the wrong model.
LOADED=$(ssh -n $HOST "curl -s http://$EP:$PORT/props" | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])" 2>/dev/null)
case "$LOADED" in
  *qat-UD-Q4_K_XL*) say "loaded $LOADED" ;;
  *) say "IDENTITY GUARD FAILED: loaded '$LOADED', wanted *qat-UD-Q4_K_XL*"; exit 1 ;;
esac
ssh -n -o ConnectTimeout=20 $HOST "nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader" 2>/dev/null | sed 's/^/  VRAM with model resident: /'

pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -L "$PORT:$EP:$PORT" "$HOST" >/dev/null 2>&1 </dev/null &
sleep 6

t0=$(date +%s)
python3 harness/run_llamacpp.py --base-url "http://127.0.0.1:$PORT" --model "$LBL" \
  --gold "$GOLD" --out "$PRED" --thinking --max-tokens 8192 --concurrency 1 || { say "FAIL client"; exit 1; }
t1=$(date +%s)

if ! python3 harness/score.py --gold "$GOLD" --pred "$PRED" --json-out "$OUT/$LBL.score.json" 2>"$OUT/$LBL.err"; then
  if grep -q 'thinking:true' "$OUT/$LBL.err"; then
    python3 harness/score.py --gold "$GOLD" --pred "$PRED" --allow-thinking-off --json-out "$OUT/$LBL.score.json" >/dev/null 2>&1
  else
    say "BLOCKED: $(tr '\n' ' ' < "$OUT/$LBL.err" | cut -c1-200)"; exit 1
  fi
fi
rm -f "$OUT/$LBL.err"
say "OK $LBL $(python3 -c "
import json;d=json.load(open('$OUT/$LBL.score.json'));s=d['strict']
print('F1=%.4f P=%.4f R=%.4f'%(s['f1'],s['precision'],s['recall']))") wall=$(( (t1-t0)/60 ))m"

# Defect 39: every comparison gets its own interval. This one is same-corpus,
# same client, same prompt, different quant scheme, different host, so the
# +/-0.019 rented-vs-local bound applies to it and must be stated with it.
say "=== paired bootstrap, unsloth QAT+UD against google q4_0 at 26B ==="
python3 harness/bootstrap_ci.py --gold "$GOLD" \
  --pred "google_q4_0=results/vast/gemma-4-26B-A4B.qat-google-q4_0.live.pred.jsonl" \
  --pred "unsloth_UD=$PRED" --boot 20000 2>&1 | tee -a "$OUT/arms.log"
say "=== 26B QUANT-SCHEME PAIR COMPLETE ==="

# Stop the server. Leaving it resident cost 12 minutes: the next arm queued on this
# card asked for 6390 MiB, the 26B still held 14828 of 16303, and llama-server died
# with `cudaMalloc failed: out of memory` while the launcher reported "sizing".
# shard_run.sh kills by listening socket within its OWN port range, which is right
# for isolation and useless against a server another script started on 8992.
# A script that starts a server owns stopping it.
ssh -n -o ConnectTimeout=25 $HOST \
  "pct exec 140 -- bash -lc 'for p in \$(pgrep -f \"port $PORT\"); do kill \$p; done; sleep 4; true'" >/dev/null 2>&1
say "server stopped, card released"
