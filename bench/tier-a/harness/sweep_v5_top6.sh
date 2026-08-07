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
# ladder. Uses run_hf.py at bfloat16, greedy -- the runtime the original table
# used, because a faster runtime is a different configuration and this benchmark
# moves more between configurations than between the models it compares.
#
# THINKING IS SENT EXPLICITLY, and this is not a preference.
#
# The first attempt at this sweep left --thinking unset and produced
# gemma-4-E2B at 0.4841 unfloored, against 0.6138 for the same model on the same
# 1001 notes through llama.cpp. bf16 scoring 0.13 BELOW Q4 is backwards, and the
# cause was in every row: reasoning_chars > 0 in 0 of 1001, median completion 32
# tokens.
#
# Both families default thinking OFF in the chat template -- gemma-4 resolves
# `enable_thinking | default(false)` -- so an unset flag is a choice, not a
# neutral position. Production sends the field explicitly and defaults it ON
# (src/provider_client.c:71, asserted in test_provider_client.c:45), and every
# llama.cpp arm in this campaign passes --thinking. Leaving it unset measures a
# configuration nobody ships and nothing else in the series uses.
#
# max_new_tokens rises to 8192 with it. 512 was "ample for this schema" when
# the median completion was 32 tokens; with reasoning restored the median is
# nearer 390 and 512 would truncate the tail. 8192 is the production cap.
# Truncation is checked below rather than assumed away.
#
# The thinking-off attempt is kept under results/v5-rerun/quarantine-thinking-off
# rather than deleted. It is a valid measurement of the wrong configuration.
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
  if ! $PY harness/run_hf.py --model "$M" --gold "$GOLD" --out "$PRED" \
         --thinking --max-new-tokens 8192 >"$LOG" 2>&1; then
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

  # Thinking was requested. Whether it happened is a separate fact, and the
  # whole campaign exists because nobody checked it for 10,000 notes. Not every
  # model here has a reasoning channel, so zero is reported loudly rather than
  # treated as failure -- but no number leaves this loop without its reasoning
  # and truncation counts attached.
  reasoned=$($PY -c "
import json,sys
print(sum(1 for l in open(sys.argv[1]) if (json.loads(l).get('reasoning_chars') or 0) > 0))" "$PRED" 2>/dev/null || echo 0)
  trunc=$($PY -c "
import json,sys
print(sum(1 for l in open(sys.argv[1]) if json.loads(l).get('truncated')))" "$PRED" 2>/dev/null || echo 0)
  [ "${reasoned:-0}" -eq 0 ] && say "  WARNING $M: --thinking sent, 0/$got rows reasoned. Either this model has no reasoning channel or the flag did not take. Do not compare it to a run that reasoned."
  [ "${trunc:-0}" -gt $(( got / 100 )) ] && say "  WARNING $M: $trunc/$got truncated at 8192 tokens; the tail is being cut."

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
  say "OK   $M floored=$f1 nofloor=$nf reasoned=$reasoned/$got trunc=$trunc wall=$(( (t1-t0)/60 ))m"
done
say "=== V5 RERUN COMPLETE ==="
