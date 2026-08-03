#!/bin/bash
# Does exposing the ontology's type signatures actually reduce Class-C drift?
#
# The prompt now sends "device_has_ip (device->ip)" instead of "device_has_ip".
# The signature was always in the seed ontology; it simply was not shown to the
# model, which then had to guess the naming convention and reasonably produced
# has_ip — staged as a provisional rel_type on a Class-C edge rather than
# committed as the validated Class-B edge.
#
# Measured against results/gpu (bare names) on the same gold set, same decoding.
# The number that matters is the novel-predicate rate, not F1: the fix targets
# predicate naming, so a large F1 move would be a surprise worth investigating.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/promptfix
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
google/gemma-4-E4B-it
google/gemma-4-E2B-it
ibm-granite/granite-4.1-3b
ibm-granite/granite-4.0-1b
Qwen/Qwen3.5-0.8B
"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  [ -s "$PRED" ] && { echo "SKIP $M"; continue; }
  echo "=== RUN $M ==="
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" --signature-prompt \
       >"$OUT/$SLUG.log" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$OUT/$SLUG.log"
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --pred-key pred_nofloor \
        --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$OUT/$SLUG.log"
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -3 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_PROMPTFIX_DONE"
