#!/bin/bash
# Other first-party 1B-4B models worth adding to the field, at 1001 notes.
#
# Chains behind lfm25_family_1k.sh. One campaign per card (finding 17).
#
# HOW THESE TWO WERE CHOSEN, and what was rejected, because the rejections are
# most of the work. Surveyed HF for GGUF models in the 1B-4B band created since
# 2026-04, then filtered to FIRST-PARTY releases only -- community fine-tunes,
# abliterations and merges are out. What was left:
#
#   openbmb/MiniCPM5-1B      text-generation, 926k downloads, official GGUF.
#   HuggingFaceTB/SmolLM3-3B text-generation, GGUF from ggml-org (llama.cpp's
#                            own org). Older release (2025-07) but never tested
#                            here and squarely in the band.
#
# Rejected, with reasons:
#   microsoft/Fara1.5-4B          pipeline is image-text-to-text -- a vision/
#                                 agentic model, not a text extractor.
#   Qwen/Qwen3.5-2B               also image-text-to-text.
#   mistralai/Shieldstral-1.0-3B  a guardrail model, no pipeline tag, 166 dl.
#   ibm-granite/granite-swash-2b  text-generation but no GGUF published.
#   Qwen3.6 / gemma-4             no first-party variant under 5B with a GGUF.
#                                 gemma-4's small end (E2B/E4B) is already here.
#
# QUANTS: Q4_K_M and Q8_0 only. NEITHER of these repos publishes Q6_K, so this
# is a two-point quant sweep rather than the three-point one the LFM models get.
# Stated because an absent middle point is easy to misread as a gap in the data.
#
# TIER: gold_small, 1001 notes -- the tier this model field was ranked at, and
# comparable to 1001-note subsets extracted from any banked 10k arm. Never
# against a raw 10k figure.
#
# Settings match the field: nproc=3, cache-ram 1024, prompt v8, no MTP (neither
# publishes an mtp-*.gguf).
#
# Load risk is real and handled rather than assumed: MiniCPM5 may be an
# architecture this llama.cpp build does not know. shard_run.sh fails loudly on
# "server never healthy" and this driver logs FAIL and moves to the next arm.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/newcomers-1k}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/newcomers.log"; }

say "=== waiting for the LFM2.5 family sweep to release the 5080"
while pgrep -f 'lfm25_family_1k[.]sh' >/dev/null 2>&1 || pgrep -f 'lfm25_quants_5080[.]sh' >/dev/null 2>&1; do sleep 60; done
say "=== 5080 free; starting newcomers, $EXPECT notes each"

# label|repo:quant
ARMS="\
MiniCPM5-1B.Q4_K_M|openbmb/MiniCPM5-1B-GGUF:Q4_K_M
MiniCPM5-1B.Q8_0|openbmb/MiniCPM5-1B-GGUF:Q8_0
SmolLM3-3B.Q4_K_M|ggml-org/SmolLM3-3B-GGUF:Q4_K_M
SmolLM3-3B.Q8_0|ggml-org/SmolLM3-3B-GGUF:Q8_0"

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
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- llama.cpp may not know this architecture; continuing"; continue; fi

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

say "=== NEWCOMERS SWEEP COMPLETE ==="
