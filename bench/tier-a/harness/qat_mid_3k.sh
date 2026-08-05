#!/bin/bash
# QAT E2B against QAT E4B at the mid tier, to resolve a gap 1001 notes could not.
#
# THE QUESTION. E2B is architecturally a nested submodel of E4B, so E4B should
# dominate. Across five head-to-head configurations it does, four times, by
# +0.0008 to +0.0149 -- and once, under QAT, it goes the other way by -0.0213.
# Paired bootstrap at n=1001 puts that at 95% CI [-0.0445, +0.0015]: the interval
# barely contains zero, so the result is marginal rather than clean either way.
# 1001 notes cannot settle it.
#
# n=3002 narrows the interval by roughly sqrt(3), from about +/-0.024 to about
# +/-0.014, which is enough to resolve a 0.021 gap if it is real.
#
# WHY ONLY TWO ARMS. The question is E2B vs E4B *within* QAT. Both arms run on
# the same card in the same configuration, so that comparison carries no
# confound and needs no UD counterpart. UD is already covered at 1001 (nproc=1,
# 5080, results/v5-rerun-gguf) and at 10k (nproc=3, XTX, results/10k-sharded and
# results/10k-nomtp), and since gold_small is a strict subset of gold_mid which
# is a strict subset of gold_large, the banked UD 10k arms already contain these
# 3002 notes and can be scored on them later if a QAT-vs-UD read at this tier is
# wanted. Nothing needs re-running.
#
# CONFIGURATION matches the 1001-note QAT arms exactly -- nproc=1, no MTP,
# cache-ram 1024, prompt v8, 5080 -- so gold_small being a subset of gold_mid
# means the existing 1001-note results are contained in these and act as a
# consistency check. If the 3002-note arms disagree with their own first 1001
# notes, something is wrong with the run rather than with the models.
#
# VERIFY_FAM is required: google names the files gemma-4-E2B_q4_0-it.gguf, which
# does not contain the stem derived from the repo name (see f9b95875c).
#
# gold_mid has never been used by anything in this project. These are the first
# arms on it.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_mid.jsonl}
OUT=${OUT:-results/qat-mid-3k}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/qat_mid.log"; }

say "=== QAT at the mid tier: E2B and E4B, $EXPECT notes, nproc=1, no MTP"
say "    resolves an E2B/E4B gap that n=1001 left marginal (CI [-0.0445,+0.0015])"

ARMS="\
gemma-4-E2B-it.qat.mid|google/gemma-4-E2B-it-qat-q4_0-gguf:q4_0|gemma-4-E2B
gemma-4-E4B-it.qat.mid|google/gemma-4-E4B-it-qat-q4_0-gguf:q4_0|gemma-4-E4B"

while IFS='|' read -r label repo fam; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $repo  nproc=1  no MTP  VERIFY_FAM=$fam"
  t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" VERIFY_FAM="$fam" \
    CARD=5080 NPROC=1 BASE_PORT=8940 CACHE_RAM_MIB=1024 \
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

  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"

say "=== QAT MID COMPLETE; paired bootstrap follows ==="
if [ -s "$OUT/gemma-4-E2B-it.qat.mid.pred.jsonl" ] && [ -s "$OUT/gemma-4-E4B-it.qat.mid.pred.jsonl" ]; then
  python3 harness/bootstrap_ci.py --gold "$GOLD" \
    --pred "E2B_qat_mid=$OUT/gemma-4-E2B-it.qat.mid.pred.jsonl" \
    --pred "E4B_qat_mid=$OUT/gemma-4-E4B-it.qat.mid.pred.jsonl" \
    --boot 20000 2>&1 | tee -a "$OUT/qat_mid.log"
fi
