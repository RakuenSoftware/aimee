#!/bin/bash
# Gemma 4 26B-A4B on the GPU — an article data point, not a deployment candidate.
#
# 26B in bf16 is ~52GB against 15.5GB of VRAM. Offloading it would run at roughly
# 5-8 hours for 70 notes (the 12B probe managed 74s a note offloaded), so this
# runs NF4-quantised with device_map=auto: resident where it fits, spilled to
# CPU beyond that. NF4 alone is not enough — 26B at NF4 is ~19GB against 15.5GB
# of VRAM, so pinning device_map=cuda OOMs on load.
#
# Quantisation is a confound, so E4B is re-run at NF4 as a control. The 26B
# number is only meaningful against E4B-NF4, never against the bf16 table. That
# pairing is the whole point of running the control at all.
#
# On CPU relevance: none. A 26B MoE has 3.8B active parameters, but this task is
# prefill-dominated (400 tokens in, ~48 out) and a long prefill routes across most
# experts, so the active-parameter saving largely evaporates exactly where our
# cost is. It also needs all 26B resident. This is a GPU shape.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/ceiling
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

# control first: it is small, fast, and if it fails the 26B number is unusable.
MODELS="
google/gemma-4-E4B-it
google/gemma-4-26B-A4B-it
"

for M in $MODELS; do
  SLUG="$(echo "$M" | tr '/' '_').nf4"
  PRED="$OUT/$SLUG.pred.jsonl"
  [ -s "$PRED" ] && { echo "SKIP $M"; continue; }
  echo "=== RUN $M (NF4) ==="
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       --load-4bit --device auto --gpu-budget 13GiB >"$OUT/$SLUG.log" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$OUT/$SLUG.log"
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --no-alias \
        --json-out "$OUT/$SLUG.score.noalias.json" >/dev/null 2>>"$OUT/$SLUG.log"
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -3 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_MOE_DONE"
