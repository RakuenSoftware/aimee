#!/bin/bash
# Gemma 4 26B-A4B at bf16 with CPU offload.
#
# Three NF4 attempts failed: device_map=cuda OOMed, device_map=auto was refused
# by bitsandbytes without llm_int8_enable_fp32_cpu_offload, and enabling that hit
# "Tensor.item() cannot be called on meta tensors". Rather than keep patching the
# quantised path, this runs bf16 offloaded — the same way the 12B probe ran.
#
# That is the better experiment anyway: bf16 is directly comparable to the whole
# bf16 table including the 12B's 0.815, with no quantisation confound and no need
# to read it against the NF4 control.
#
# Cost is time. The 12B managed 74s a note offloaded; 26B has ~2x the weights to
# stream, so expect roughly 2-4 hours for 70 notes. Latency from this run is
# meaningless and must not reach a speed table.
#
# 35B is deliberately NOT here: it needs ~70GB and the host runs 45 other
# containers, so taking that much RAM risks starving live services.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/ceiling
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

M="google/gemma-4-26B-A4B-it"
SLUG="$(echo "$M" | tr '/' '_')"
PRED="$OUT/$SLUG.pred.jsonl"
if [ -s "$PRED" ]; then echo "SKIP $M"; else
  echo "=== RUN $M (bf16, offloaded) ==="
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       --device auto >"$OUT/$SLUG.log" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$OUT/$SLUG.log"
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -3 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
fi
echo "SWEEP_MOE_BF16_DONE"
