#!/bin/bash
# Qwen3.6-35B-A3B — the other MoE ceiling data point, for the article.
#
# 35B total / 3B active, Apache-2.0. At NF4 it is ~19GB against 15.5GB of VRAM,
# so device_map=auto keeps most of it resident and offloads the remainder; expect
# it slower than the 26B but far short of the 74s a note that full bf16 offload
# cost on the 12B.
#
# Compare it to results/ceiling/google_gemma-4-E4B-it.nf4, NOT to the bf16 table:
# same quantisation, same gold set, same decoding. Cross-family at equal
# quantisation is the only honest reading available here.
#
# As with the Gemma MoE: this says nothing about CPU viability. 3B active
# parameters do not help a prefill-dominated workload that routes across most
# experts, and all 35B must still be resident.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/ceiling
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="Qwen/Qwen3.6-35B-A3B"

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
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --pred-key pred_nofloor \
        --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$OUT/$SLUG.log"
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -3 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_QWEN_MOE_DONE"
