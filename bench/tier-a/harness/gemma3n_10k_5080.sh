#!/bin/bash
# gemma-3n-E4B at 10k, chained behind missing_10k_5080.sh.
#
# Held out of that driver because it will not run 3 processes: at UD-Q4_K_XL it
# is 5.02 GiB and three copies plus KV do not fit the 5080's 16303 MiB. Running
# it anyway, at whatever the card holds, because the ranking answers "what should
# we use today" -- and if you deployed this model on this card you would get the
# process count that fits, not the one the other arms happen to use. A model's
# own constraints are not a confound to be equalised away; MTP availability and
# thinking support are the same call.
#
# NPROC=0 = auto-size from measured resident VRAM, so the number is derived
# rather than guessed. Recorded in the arm's log either way.
#
# Chains rather than competes: two campaigns on one card is finding 17.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-5080}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/missing_10k.log"; }

say "=== gemma-3n chained: waiting for the 5080 to release"
while pgrep -f 'harness/missing_10k_5080.sh' >/dev/null 2>&1; do sleep 60; done
say "=== 5080 free; starting gemma-3n-E4B"

LABEL=gemma-3n-E4B-it.10k
REPO=unsloth/gemma-3n-E4B-it-GGUF:UD-Q4_K_XL
pred="$OUT/$LABEL.pred.jsonl"

if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
  say "SKIP $LABEL (banked)"
  exit 0
fi
[ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

say "--- $LABEL  $REPO  nproc=auto  port=8600"
t0=$(date +%s)
GOLD="$GOLD" OUT="$OUT" LABEL="$LABEL" REPO="$REPO" DRAFT="" \
  CARD=5080 NPROC=0 BASE_PORT=8600 CACHE_RAM_MIB=1024 \
  bash harness/shard_run.sh
rc=$?
t1=$(date +%s)
if [ $rc -ne 0 ]; then say "FAIL $LABEL (rc=$rc)"; exit 1; fi

python3 harness/score.py --gold "$GOLD" --pred "$pred" \
  --json-out "$OUT/$LABEL.score.json" >/dev/null 2>&1
f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$LABEL.score.json'))['strict']['f1'])" 2>/dev/null || echo "scorer-refused")
say "OK   $LABEL strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
say "=== GEMMA-3N COMPLETE ==="
