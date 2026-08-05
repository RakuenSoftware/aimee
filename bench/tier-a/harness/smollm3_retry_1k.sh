#!/bin/bash
# Re-run SmolLM3-3B after verify_model refused it on a naming convention.
#
# Both SmolLM3 arms failed with:
#   FAIL: port 8990 loaded 'SmolLM3-Q4_K_M.gguf', expected SmolLM3-3B / Q4_K_M
#
# The correct file was loaded. verify_model derives the expected stem from the
# repo name -- ggml-org/SmolLM3-3B-GGUF -> "SmolLM3-3B" -- but ggml-org publishes
# SmolLM3-Q4_K_M.gguf with no size suffix, so the stem never matches.
#
# That is a false negative in a guard built to catch false positives (defect 30,
# a stale server answering with someone else's weights). Relaxing the match would
# hand the stale-server case back, so shard_run.sh now accepts an explicit
# VERIFY_FAM override instead. Unset, its behaviour is unchanged; here it is set
# to the stem this publisher actually uses.
#
# Everything else matches the newcomers sweep: 1001 notes on gold_small,
# nproc=3, cache-ram 1024, prompt v8, no MTP, Q4_K_M and Q8_0 only (ggml-org
# publishes no Q6_K).
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/newcomers-1k}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/newcomers.log"; }

say "=== SmolLM3 retry: waiting for the newcomers sweep to release the 5080"
while pgrep -f 'newcomers_1k[.]sh' >/dev/null 2>&1; do sleep 30; done
say "=== 5080 free; retrying SmolLM3-3B with VERIFY_FAM=SmolLM3"

ARMS="\
SmolLM3-3B.Q4_K_M|ggml-org/SmolLM3-3B-GGUF:Q4_K_M
SmolLM3-3B.Q8_0|ggml-org/SmolLM3-3B-GGUF:Q8_0"

while IFS='|' read -r label repo; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi

  say "--- $label  $repo  nproc=3  no MTP  VERIFY_FAM=SmolLM3"
  t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" VERIFY_FAM=SmolLM3 \
    CARD=5080 NPROC=3 BASE_PORT=8990 CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing"; continue; fi

  if ! python3 harness/score.py --gold "$GOLD" --pred "$pred" \
        --json-out "$OUT/$label.score.json" 2>"$OUT/$label.score.err"; then
    if grep -q 'thinking:true' "$OUT/$label.score.err"; then
      say "  thinking guard fired; re-scoring with --allow-thinking-off"
      python3 harness/score.py --gold "$GOLD" --pred "$pred" --allow-thinking-off \
        --json-out "$OUT/$label.score.json" >/dev/null 2>&1
    else
      say "BLOCKED $label -- $(tr '\n' ' ' < "$OUT/$label.score.err" | cut -c1-300)"
      continue
    fi
  fi
  rm -f "$OUT/$label.score.err"

  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== SMOLLM3 RETRY COMPLETE ==="
