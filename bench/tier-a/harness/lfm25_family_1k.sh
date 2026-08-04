#!/bin/bash
# The rest of the LFM2.5 range at 1001 notes: 230M, 1.2B-Instruct, VL-1.6B.
#
# Chains behind lfm25_quants_5080.sh (the 2.6B sweep) so the 5080 runs one
# campaign at a time -- two campaigns on one card is finding 17.
#
# TIER: gold_small, 1001 notes, which is the tier this model field was actually
# ranked at. gold_small is a strict subset of gold_large, so these arms are
# directly comparable to 1001-note subsets extracted from any banked 10k arm.
# They are NOT comparable to raw 10k figures: granite-4.0-1b reads 0.3911 on the
# small subset against 0.4215 on the full 10k.
#
# QUANTS: LiquidAI's own standard K-quants (Q4_K_M, Q6_K, Q8_0). Unsloth
# publishes Dynamic quants for some of these models but not for the 2.6B, so
# using unsloth where available would make the quant scheme vary WITHIN the LFM
# family sweep. Standard across all of them keeps the family internally
# comparable; the cross-model caveat against the UD-quantised gemma/granite arms
# is stated in the 2.6B driver and applies here too.
#
# NOT INCLUDED: LFM2.5-8B-A1B. At Q4_K_M it is 5.16 GB and three copies are
# 15.5 GB of a 16303 MiB card before any KV cache; Q6 and Q8 are 20.9 and 27.0
# GB. It is a MoE, so all experts stay resident and the gemma-3n lesson (5.02 GB
# file, 3414 MiB resident) does not apply. Running it at NPROC=1 while every
# other arm runs 3 would put process count inside the ranking -- worth 0.0105 F1
# by finding 19 -- so it is left out rather than quietly reconfigured.
#
# VL-1.6B is a VISION-language model run text-only under --no-mmproj. It will
# produce numbers, but it is a different class of model to rank against text
# extractors and the writeup should say so rather than listing it flat.
#
# 1.2B ships as Instruct and Thinking variants. Instruct is used here as the
# closer analogue to the other instruct-tuned models in the field. The Thinking
# variant is a separate question and deliberately not mixed in.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/lfm25-family}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/family.log"; }

say "=== waiting for the 2.6B sweep to release the 5080"
while pgrep -f 'lfm25_quants_5080[.]sh' >/dev/null 2>&1; do sleep 60; done
say "=== 5080 free; starting the rest of the LFM2.5 range, $EXPECT notes each"

# label|repo:quant  -- smallest model first so results land early
ARMS="\
LFM2.5-230M.Q4_K_M|LiquidAI/LFM2.5-230M-GGUF:Q4_K_M
LFM2.5-230M.Q6_K|LiquidAI/LFM2.5-230M-GGUF:Q6_K
LFM2.5-230M.Q8_0|LiquidAI/LFM2.5-230M-GGUF:Q8_0
LFM2.5-1.2B-Instruct.Q4_K_M|LiquidAI/LFM2.5-1.2B-Instruct-GGUF:Q4_K_M
LFM2.5-1.2B-Instruct.Q6_K|LiquidAI/LFM2.5-1.2B-Instruct-GGUF:Q6_K
LFM2.5-1.2B-Instruct.Q8_0|LiquidAI/LFM2.5-1.2B-Instruct-GGUF:Q8_0
LFM2.5-VL-1.6B.Q4_K_M|LiquidAI/LFM2.5-VL-1.6B-GGUF:Q4_K_M
LFM2.5-VL-1.6B.Q6_K|LiquidAI/LFM2.5-VL-1.6B-GGUF:Q6_K
LFM2.5-VL-1.6B.Q8_0|LiquidAI/LFM2.5-VL-1.6B-GGUF:Q8_0"

while IFS='|' read -r label repo; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $repo  nproc=3  no MTP"
  t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" \
    CARD=5080 NPROC=3 BASE_PORT=8990 CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next arm"; continue; fi

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

say "=== LFM2.5 FAMILY SWEEP COMPLETE ==="
python3 - "$OUT" <<'PY' | while read -r l; do say "$l"; done
import json, glob, os, sys
out = sys.argv[1]
print("SUMMARY LFM2.5 family at 1001 notes (strict F1):")
for p in sorted(glob.glob(os.path.join(out, "*.score.json"))):
    label = os.path.basename(p).replace(".score.json", "")
    try:
        d = json.load(open(p))["strict"]
        print("  %-30s f1=%.4f p=%.4f r=%.4f" % (label, d["f1"], d["precision"], d["recall"]))
    except Exception:
        pass
PY
