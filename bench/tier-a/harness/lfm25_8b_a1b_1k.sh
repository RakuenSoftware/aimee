#!/bin/bash
# LFM2.5-8B-A1B at Q4_K_M, 1001 notes, on the 5080.
#
# THIS ARM IS NOT COMPARABLE TO THE RANKING TABLE, and that is stated here rather
# than discovered later.
#
# Every other arm in the field runs NPROC=3. This model cannot: Q4_K_M is 5.16 GB
# and three copies are 15.5 GB of a 16303 MiB card before any KV cache. It is a
# MoE -- 8B total, ~1B active -- so all experts stay resident and the gemma-3n
# lesson (5.02 GB file, 3414 MiB resident) does not rescue it. Q6 and Q8 are 20.9
# and 27.0 GB for three and are not attempted at all.
#
# So this runs at NPROC=0, auto-sized from measured VRAM, and whatever process
# count that yields is a different configuration from the rest of the field.
# finding 19 prices process count at 0.0105 F1 -- the same size as the noise
# threshold this campaign uses to decide whether anything is real. The number
# this produces therefore belongs in the writeup as an aside about the largest
# model that fits, NOT as a row in the ranking.
#
# Runs only because the 5080 is idle while the XTX finishes the E4B side of the
# MTP ladder. It is the lowest-priority item in the queue and the first thing to
# drop if the card is wanted.
#
# Tier is gold_small, 1001 notes, matching every other 5080 arm in this campaign.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/lfm25-8b}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/8b.log"; }

LABEL=LFM2.5-8B-A1B.Q4_K_M
REPO=LiquidAI/LFM2.5-8B-A1B-GGUF:Q4_K_M
pred="$OUT/$LABEL.pred.jsonl"

say "=== LFM2.5-8B-A1B Q4_K_M, $EXPECT notes, NPROC=auto (cannot fit 3 copies)"
say "    NOT comparable to the nproc=3 ranking table -- see header"

if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
  say "SKIP $LABEL (banked)"; exit 0
fi

say "--- $LABEL  $REPO  nproc=auto  no MTP"
t0=$(date +%s)
GOLD="$GOLD" OUT="$OUT" LABEL="$LABEL" REPO="$REPO" DRAFT="" \
  CARD=5080 NPROC=0 BASE_PORT=8970 CACHE_RAM_MIB=1024 \
  bash harness/shard_run.sh
rc=$?
t1=$(date +%s)
if [ $rc -ne 0 ]; then say "FAIL $LABEL (rc=$rc)"; exit 1; fi

if ! python3 harness/score.py --gold "$GOLD" --pred "$pred" \
      --json-out "$OUT/$LABEL.score.json" 2>"$OUT/$LABEL.score.err"; then
  if grep -q 'thinking:true' "$OUT/$LABEL.score.err"; then
    say "  thinking guard fired; re-scoring with --allow-thinking-off"
    python3 harness/score.py --gold "$GOLD" --pred "$pred" --allow-thinking-off \
      --json-out "$OUT/$LABEL.score.json" >/dev/null 2>&1
  else
    say "BLOCKED $LABEL -- $(tr '\n' ' ' < "$OUT/$LABEL.score.err" | cut -c1-300)"
    exit 1
  fi
fi
rm -f "$OUT/$LABEL.score.err"

f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$LABEL.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
say "OK   $LABEL strictF1=$f1 wall=$(( (t1-t0)/60 ))m  (nproc differs from the field)"
say "=== 8B-A1B COMPLETE ==="
