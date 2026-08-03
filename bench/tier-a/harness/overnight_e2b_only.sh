#!/bin/bash
# E2B arms only, on the 5080. The XTX is mid-run on E4B Q8 and is not touched.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?}; OUT=${OUT:?}
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/overnight.log"; }
for q in Q4 Q6 Q8; do
  label="E2B.UD-${q}_K_XL.10k"
  if [ -s "$OUT/$label.pred.jsonl" ] && [ "$(wc -l < "$OUT/$label.pred.jsonl")" -ge "$(wc -l < "$GOLD")" ]; then
    say "[5080] SKIP $label"; continue
  fi
  say "[5080] START $label (4 shards)"
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" \
    REPO="unsloth/gemma-4-E2B-it-GGUF:UD-${q}_K_XL" \
    DRAFT="unsloth/gemma-4-E2B-it-GGUF" \
    CARD=5080 NPROC=4 BASE_PORT=8200 \
    bash harness/shard_run.sh >>"$OUT/overnight_5080.log" 2>&1
  got=0; [ -s "$OUT/$label.pred.jsonl" ] && got=$(wc -l < "$OUT/$label.pred.jsonl")
  if [ "$got" -ge "$(wc -l < "$GOLD")" ]; then
    python3 harness/score.py --gold "$GOLD" --pred "$OUT/$label.pred.jsonl" \
      --json-out "$OUT/$label.score.json" >/dev/null 2>&1
    f1=$(python3 -c "import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo refused)
    say "[5080] OK $label rows=$got strictF1=$f1"
  else
    say "[5080] FAIL $label rows=$got"
  fi
done
say "[5080] === E2B DONE ==="
