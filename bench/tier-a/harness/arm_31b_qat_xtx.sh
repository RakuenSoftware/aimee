#!/bin/bash
# gemma-4-31B QAT on the XTX, the missing half of the 31B QAT-vs-non-QAT pair.
# 17.53 GiB fits the XTX's 24 GB where it does not fit the 5080's 16. The non-QAT
# side has 596 rows banked from a rented box; this is the local counterpart.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=results/ct140
mkdir -p "$OUT"
GOLD=data/corpora/v5/gold_small.jsonl
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/arms.log"; }
say "=== gemma-4-31B QAT on the XTX, nproc=1, MTP"
t0=$(date +%s)
GOLD="$GOLD" OUT="$OUT" LABEL=gemma-4-31B-it.qat-UD-Q4_K_XL.xtx \
  REPO=unsloth/gemma-4-31B-it-qat-GGUF:UD-Q4_K_XL \
  DRAFT=unsloth/gemma-4-31B-it-qat-GGUF:MTP/mtp-gemma-4-31B-it-Q8_0.gguf \
  VERIFY_FAM=gemma-4-31B-it-qat CARD=xtx NPROC=1 BASE_PORT=8810 CACHE_RAM_MIB=1024 \
  bash harness/shard_run.sh
rc=$?; t1=$(date +%s)
[ $rc -ne 0 ] && { say "FAIL 31B qat xtx rc=$rc"; exit 1; }
python3 harness/score.py --gold "$GOLD" --pred "$OUT/gemma-4-31B-it.qat-UD-Q4_K_XL.xtx.pred.jsonl" \
  --json-out "$OUT/gemma-4-31B-it.qat-UD-Q4_K_XL.xtx.score.json" 2>/dev/null \
  || python3 harness/score.py --gold "$GOLD" --pred "$OUT/gemma-4-31B-it.qat-UD-Q4_K_XL.xtx.pred.jsonl" \
     --allow-thinking-off --json-out "$OUT/gemma-4-31B-it.qat-UD-Q4_K_XL.xtx.score.json" >/dev/null 2>&1
say "OK 31B qat xtx $(python3 -c "
import json;s=json.load(open('$OUT/gemma-4-31B-it.qat-UD-Q4_K_XL.xtx.score.json'))['strict']
print('F1=%.4f'%s['f1'])" 2>/dev/null) wall=$(( (t1-t0)/60 ))m"
