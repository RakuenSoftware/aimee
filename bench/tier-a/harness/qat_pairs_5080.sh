#!/bin/bash
# Finish the QAT-vs-non-QAT accuracy pairs on the 5080.
#
# The article can currently say only that QAT is worth +0.0389 F1 on E2B and
# nothing on E4B: two models, opposite answers, no third point. 12B and 31B turn
# that into four sizes and decide whether the benefit tracks model size.
#
# This runs on the LOCAL card rather than rented boxes. The 5080 sat idle most of
# the night while these arms waited on rate-limited rented placement, which was a
# straightforward waste: it is faster than anything rented (285 tok/s on the QAT
# 12B against 84-232 on rented 3090s), it is free, and it does not vanish
# mid-arm.
#
# Sequential, one model at a time, one card. Both arms of a pair therefore share
# a machine, which is exactly the property the rented arms could not offer and
# the reason the rented 1.7x speed gap turned out to be two thirds host.
#
# 31B at UD-Q4_K_XL is 17.53 GiB and does NOT fit the 5080's 16303 MiB, so only
# the 12B pair runs here. The 31B pair stays on rented 5090s.
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
EXPECT=$(wc -l < "$GOLD")

run_arm() {   # $1 label, $2 repo, $3 draft, $4 verify-substring
  local LBL=$1 REPO=$2 DRAFT=$3 WANT=$4
  local PRED="$OUT/$LBL.pred.jsonl"
  if [ -s "$PRED" ] && [ "$(wc -l < "$PRED")" -ge "$EXPECT" ]; then say "SKIP $LBL (banked)"; return 0; fi

  say "--- $LBL"
  ssh -n -o ConnectTimeout=25 $HOST \
    "pct exec 140 -- bash -lc 'for p in \$(pgrep -f \"port $PORT\"); do kill \$p; done; sleep 4; true'" >/dev/null 2>&1
  ssh -n -o ConnectTimeout=25 $HOST \
    "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $PORT -c 8192 -np 1 --cache-ram 1024 --no-webui --no-mmproj -ngl 99 >/opt/tierA/arm-$LBL.log 2>&1 </dev/null &'" >/dev/null 2>&1

  # wait on evidence only: healthy, or the server process is gone
  while true; do
    ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$EP:$PORT/health" >/dev/null 2>&1 && break
    if ! ssh -n -o ConnectTimeout=10 $HOST "pct exec 140 -- bash -lc 'pgrep -f \"port $PORT\" >/dev/null'" 2>/dev/null; then
      say "FAIL $LBL: server gone"; ssh -n $HOST "pct exec 140 -- bash -lc 'tail -6 /opt/tierA/arm-$LBL.log'" 2>&1 | tail -6
      return 1
    fi
    sleep 20
  done

  LOADED=$(ssh -n $HOST "curl -s http://$EP:$PORT/props" | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])" 2>/dev/null)
  case "$LOADED" in
    *"$WANT"*) say "    loaded $LOADED" ;;
    *) say "IDENTITY GUARD FAILED for $LBL: loaded $LOADED, wanted *$WANT*"; return 1 ;;
  esac

  pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -L "$PORT:$EP:$PORT" "$HOST" >/dev/null 2>&1 </dev/null &
  sleep 6

  t0=$(date +%s)
  python3 harness/run_llamacpp.py --base-url "http://127.0.0.1:$PORT" --model "$LBL" \
    --gold "$GOLD" --out "$PRED" --thinking --max-tokens 8192 --concurrency 1 || { say "FAIL $LBL client"; return 1; }
  t1=$(date +%s)

  if ! python3 harness/score.py --gold "$GOLD" --pred "$PRED" --json-out "$OUT/$LBL.score.json" 2>"$OUT/$LBL.err"; then
    if grep -q 'thinking:true' "$OUT/$LBL.err"; then
      python3 harness/score.py --gold "$GOLD" --pred "$PRED" --allow-thinking-off --json-out "$OUT/$LBL.score.json" >/dev/null 2>&1
    else
      say "BLOCKED $LBL: $(tr '\n' ' ' < "$OUT/$LBL.err" | cut -c1-200)"; return 1
    fi
  fi
  rm -f "$OUT/$LBL.err"
  say "OK   $LBL $(python3 -c "
import json;d=json.load(open('$OUT/$LBL.score.json'));s=d['strict']
print('F1=%.4f P=%.4f R=%.4f'%(s['f1'],s['precision'],s['recall']))") wall=$(( (t1-t0)/60 ))m"
}

say "=== QAT accuracy pairs on the 5080, 12B only (31B at 17.53GiB does not fit 16303 MiB)"
run_arm gemma-4-12B-it.UD-Q4_K_XL.5080 \
  unsloth/gemma-4-12b-it-GGUF:UD-Q4_K_XL \
  unsloth/gemma-4-12b-it-GGUF:MTP/mtp-gemma-4-12b-it-Q8_0.gguf gemma-4-12b-it
run_arm gemma-4-12B-it.qat-UD-Q4_K_XL.5080 \
  unsloth/gemma-4-12B-it-qat-GGUF:UD-Q4_K_XL \
  unsloth/gemma-4-12B-it-qat-GGUF:MTP/mtp-gemma-4-12B-it-Q8_0.gguf gemma-4-12B-it-qat

say "=== paired bootstrap, QAT against non-QAT at 12B ==="
python3 harness/bootstrap_ci.py --gold "$GOLD" \
  --pred "nonQAT=$OUT/gemma-4-12B-it.UD-Q4_K_XL.5080.pred.jsonl" \
  --pred "QAT=$OUT/gemma-4-12B-it.qat-UD-Q4_K_XL.5080.pred.jsonl" --boot 20000 2>&1 | tee -a "$OUT/arms.log"
say "=== 12B QAT PAIR COMPLETE ==="
