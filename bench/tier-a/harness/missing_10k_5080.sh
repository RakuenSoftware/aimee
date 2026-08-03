#!/bin/bash
# The models that have never been run on the 10k set.
#
# E2B and E4B are the only two models in this project with a large-tier arm.
# Everything else tops out at 1001 notes or at the 70-note data/gold.jsonl, so
# article 1's ranking currently compares a 1001-note granite against a 1001-note
# E2B taken by a different sweep at a different setting. These four close that.
#
# Runs on the 5080 in CT 140 while the XTX finishes article 2's ladder. Two
# cards, two campaigns -- NOT two campaigns on one card, which is finding 17.
#
# Settings match the banked 10k arms in results/10k-sharded/ so these are
# comparable to E2B and E4B rather than only to each other:
#
#   NPROC=3     the ladder arms are 3-process. Process count is worth 0.0105 F1
#               (finding 19), larger than most gaps being measured, so it cannot
#               vary across the models being ranked.
#   cache-ram   1024 MiB, matching the ladder (finding 20). Results-affecting.
#   thinking    on, as shard_run.sh sends unconditionally, same as the ladder.
#
# ONE DIFFERENCE, stated rather than buried: the ladder arms run MTP drafts and
# these cannot. Only gemma-4 publishes an mtp-*.gguf; granite-4.0-1b,
# granite-4.1-3b, gemma-3n-E4B and Qwen3-1.7B have none. MTP moves 26 of 100
# notes, so a granite-vs-E2B comparison drawn straight across this boundary has
# the draft head inside it. The clean comparison for that is E2B/E4B re-run
# without MTP at 10k, which does not exist yet and is not launched here.
#
# gemma-3n-E4B is NOT in this list. At UD-Q4_K_XL it is 5.02 GiB, and three
# copies plus KV do not fit the 5080's 16303 MiB. Running it at NPROC=1 while
# the other three run at 3 would put process count inside the ranking, which is
# the one thing the setting above exists to prevent.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-5080}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/missing_10k.log"; }

# label|repo|base_port
ARMS="\
granite-4.0-1b.10k|unsloth/granite-4.0-1b-GGUF:UD-Q4_K_XL|8600
granite-4.1-3b.10k|unsloth/granite-4.1-3b-GGUF:UD-Q4_K_XL|8600
Qwen3-1.7B.10k|unsloth/Qwen3-1.7B-GGUF:UD-Q4_K_XL|8600"

say "=== missing 10k arms on the 5080: 3 models, $EXPECT notes each, nproc=3, no MTP"
while IFS='|' read -r label repo port; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  if [ -e "$pred.errored" ]; then
    mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"
  fi

  say "--- $label  $repo  nproc=3  port=$port"
  t0=$(date +%s)
  # DRAFT exported EMPTY on purpose; shard_run.sh omits -hfd when it is.
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" \
    CARD=5080 NPROC=3 BASE_PORT="$port" CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next arm"; continue; fi

  python3 harness/score.py --gold "$GOLD" --pred "$pred" \
    --json-out "$OUT/$label.score.json" >/dev/null 2>&1
  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "scorer-refused")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== MISSING 10K COMPLETE ==="
