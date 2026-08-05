#!/bin/bash
# QAT against Unsloth Dynamic: does quantisation-aware training behave
# differently from a post-hoc dynamic quant at the same bit width?
#
# Google publishes gemma-4-{E2B,E4B}-it-qat-q4_0-gguf -- weights trained with
# quantisation in the loop, shipped as legacy q4_0. Everything in this benchmark
# so far runs unsloth's UD-Q4_K_XL, a post-hoc dynamic K-quant. Both are ~4 bit.
#
# PAIRING, and why it is only two arms rather than four. UD arms at exactly this
# configuration already exist and do not need re-running:
#
#   results/v5-rerun-gguf/gemma-4-E2B-it  1001 notes, nproc=1, no MTP, 5080, 0.6017
#   results/v5-rerun-gguf/gemma-4-E4B-it  1001 notes, nproc=1, no MTP, 5080, 0.6166
#
# So these arms copy that configuration exactly -- same tier, same process count,
# same card, same MTP setting, same prompt. The ONLY difference is the quant
# scheme, which is the question.
#
# NPROC=1 is inherited from that sweep rather than chosen. It also happens to
# remove the VRAM problem: E4B QAT is 5.15 GB and three copies are 15.5 GB of a
# 16303 MiB card, which is why the 8B-A1B arm had to be flagged incomparable.
# One copy fits with room.
#
# These arms are NOT comparable to the nproc=3 ranking table for the same reason
# the v5-rerun-gguf arms are not: process count is worth 0.0105 F1 (finding 19).
# The comparison they support is QAT vs UD within this configuration.
#
# Chains behind the 8B-A1B arm so the 5080 runs one campaign at a time.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/qat-vs-ud}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/qat.log"; }

say "=== waiting for the 8B-A1B arm to release the 5080"
while pgrep -f 'lfm25_8b_a1b_1k[.]sh' >/dev/null 2>&1; do sleep 60; done
say "=== 5080 free; QAT arms, $EXPECT notes, nproc=1, no MTP"

# label|repo:quant|UD counterpart to report alongside
ARMS="\
gemma-4-E2B-it.qat|google/gemma-4-E2B-it-qat-q4_0-gguf:q4_0|results/v5-rerun-gguf/gemma-4-E2B-it
gemma-4-E4B-it.qat|google/gemma-4-E4B-it-qat-q4_0-gguf:q4_0|results/v5-rerun-gguf/gemma-4-E4B-it"

while IFS='|' read -r label repo ud; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $repo  nproc=1  no MTP"
  t0=$(date +%s)
  # google names the file gemma-4-E2B_q4_0-it.gguf, which does not contain the
  # repo stem, so verify_model needs the override (see f9b95875c / SmolLM3).
  fam=$(echo "$label" | sed 's/\.qat$//' | sed 's/-it$//')
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" VERIFY_FAM="$fam" \
    CARD=5080 NPROC=1 BASE_PORT=8960 CACHE_RAM_MIB=1024 \
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

  python3 - "$OUT/$label.score.json" "$ud.score.json" "$label" <<'PY' | while read -r l; do say "$l"; done
import json, sys, os
q = json.load(open(sys.argv[1]))["strict"]
label = sys.argv[3]
try:
    u = json.load(open(sys.argv[2]))["strict"]
    print("OK   %s QAT=%.4f  UD=%.4f  delta=%+.4f" % (label, q["f1"], u["f1"], q["f1"] - u["f1"]))
except Exception:
    print("OK   %s QAT=%.4f  (UD counterpart unreadable)" % (label, q["f1"]))
PY
  say "     wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== QAT VS UD COMPLETE ==="
