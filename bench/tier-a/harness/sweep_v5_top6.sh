#!/bin/bash
# Article 1's blocker: re-run the top six models on the CURRENT corpus and
# ontology, at 1001 notes instead of 69.
#
# Every number in the 16-model table predates three fixes to the benchmark
# itself:
#   - the corpus template that phrased a hostname fact as "X runs on Y" and so
#     scored 0/28 the models that read the sentence correctly (finding 4)
#   - the seed ontology that did not define 19% of the gold's own predicates,
#     now 24 relations rather than 17 (finding 6)
#   - the prompt clause "No prose, no markdown." that silently suppressed
#     gemma-4-E4B's reasoning pass for entire runs (finding 1)
#
# So the existing ordering is a hypothesis about rank, not a measurement of
# level, and this sweep is what turns it back into a measurement.
#
# Sample size is the other half. At 69 notes the comparable 95% interval is near
# +/- 0.12, which cannot resolve granite-4.0-1b (0.592) against gemma-4-E2B
# (0.593), nor any ordering among the five models between 0.50 and 0.65. At 1001
# notes it is roughly +/- 0.03.
#
# Runs on the 5080 in CT 140, which is idle while the XTX works the 10k quant
# ladder. Deliberately uses run_hf.py with stock defaults -- bfloat16, greedy,
# max_new_tokens 512, thinking left to the model's own chat template -- because
# that is exactly how the original table was produced. A faster runtime would be
# a different configuration, and configuration changes move this benchmark by
# more than the effects it is measuring.
#
# Model order front-loads the claims most at risk: the E2B / granite-4.0-1b
# near-tie first, then the incumbent, then the rest.
set -u
cd "$(dirname "$0")/.." || exit 1

PY=${PY:-/opt/bench/bin/python}
export HF_HOME=${HF_HOME:-/opt/hf}
GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/v5-rerun}
mkdir -p "$OUT"

MODELS="
google/gemma-4-E2B-it
ibm-granite/granite-4.0-1b
google/gemma-4-E4B-it
ibm-granite/granite-4.1-3b
unsloth/gemma-3n-E4B-it
Qwen/Qwen3-1.7B
"

say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/sweep.log"; }
EXPECT=$(wc -l < "$GOLD")
say "=== v5 re-run: 6 models, $EXPECT notes each"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  LOG="$OUT/$SLUG.log"
  if [ -s "$PRED" ] && [ "$(wc -l < "$PRED")" -ge "$EXPECT" ]; then say "SKIP $M (banked)"; continue; fi
  say "--- $M"
  t0=$(date +%s)
  if ! $PY harness/run_hf.py --model "$M" --gold "$GOLD" --out "$PRED" >"$LOG" 2>&1; then
    say "FAIL $M -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-160)"
    # A partial file would be silently treated as banked by the SKIP above.
    mv -f "$PRED" "$PRED.failed.$(date -u +%Y%m%dT%H%M%SZ)" 2>/dev/null
    continue
  fi
  t1=$(date +%s)

  # Row count is not completion, and an errored row is not a prediction.
  got=$(wc -l < "$PRED")
  errs=$($PY -c "
import json,sys
print(sum(1 for l in open(sys.argv[1]) if json.loads(l).get('error')))" "$PRED" 2>/dev/null || echo 0)
  if [ "$got" -ne "$EXPECT" ] || [ "${errs:-0}" -gt 0 ]; then
    say "FAIL $M: rows=$got/$EXPECT errored=$errs, not banking"
    mv -f "$PRED" "$PRED.incomplete.$(date -u +%Y%m%dT%H%M%SZ)"
    continue
  fi

  # Two views of one run, as the original sweep did: what production commits,
  # and the same extraction with the confidence floor lifted. Article 1 quotes
  # the unfloored figure, because the floor zeroed four models that were not
  # zero (finding 1's sibling defect).
  $PY harness/score.py --gold "$GOLD" --pred "$PRED" \
      --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG"
  $PY harness/score.py --gold "$GOLD" --pred "$PRED" --pred-key pred_nofloor \
      --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$LOG"
  f1=$($PY -c "
import json;print('%.4f'%json.load(open('$OUT/$SLUG.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  nf=$($PY -c "
import json;print('%.4f'%json.load(open('$OUT/$SLUG.score.nofloor.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $M floored=$f1 nofloor=$nf wall=$(( (t1-t0)/60 ))m"
done
say "=== V5 RERUN COMPLETE ==="
