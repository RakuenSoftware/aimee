#!/bin/bash
# Diagnostics for models that scored zero, to separate "cannot do the task" from
# "my harness handicapped it".
#
# LFM2-350M-Extract entered a degenerate repetition loop on 60 of 70 notes,
# hitting the 512-token cap mid-object so the JSON never closed. Two questions
# that a zero score alone cannot answer:
#
#   1. Does it terminate at all given production's real 8192 cap, or is the loop
#      unbounded? If unbounded, the 512 cap did not cause the failure, it only
#      made it cheaper to observe.
#   2. Is the loop rescuable with a repetition penalty? Production sets none, so
#      a win here is not a result we can bank -- but it changes whether the model
#      is worth a second look under a modified provider config.
#
# Same questions for SmolLM2, which produced the {"facts":...} wrapper zero times.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/diagnostics
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

head -12 data/gold.jsonl > /tmp/diag.jsonl

probe() {  # $1=model $2=tag $3=extra
  local M=$1 TAG=$2 EXTRA=${3:-}
  local SLUG="$(echo "$M" | tr '/' '_').$TAG"
  echo "=== $M [$TAG] ==="
  if $PY harness/run_hf.py --model "$M" --gold /tmp/diag.jsonl \
       --out "$OUT/$SLUG.pred.jsonl" $EXTRA > "$OUT/$SLUG.log" 2>&1; then
    $PY - "$OUT/$SLUG.pred.jsonl" <<'EOF'
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])]
n = len(rs)
print(f"  notes={n} schema_ok={sum(r['schema_ok'] for r in rs)} "
      f"truncated={sum(r['truncated'] for r in rs)} "
      f"median_tokens={sorted(r['completion_tokens'] for r in rs)[n//2]} "
      f"median_ms={sorted(r['latency_ms'] for r in rs)[n//2]:.0f}")
EOF
  else
    echo "  FAIL: $(tail -2 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-160)"
  fi
}

for M in LiquidAI/LFM2-350M-Extract HuggingFaceTB/SmolLM2-360M-Instruct; do
  probe "$M" "cap512"    "--max-new-tokens 512"
  probe "$M" "cap2048"   "--max-new-tokens 2048"
  probe "$M" "reppen1.1" "--max-new-tokens 512 --repetition-penalty 1.1"
done
echo "SWEEP_DIAGNOSTICS_DONE"
