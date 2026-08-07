#!/bin/bash
# The 12B and 31B QAT pairs at 3k, one pair per card, both halves of a pair on
# the SAME card.
#
# Why same-card matters here: the 1k pairs came back +0.0100 and +0.0108, both
# with intervals straddling zero, and the rented-vs-local calibration bound is
# +/-0.019 at n=1001 -- wider than either delta. A pair split across hardware
# therefore cannot resolve, no matter how many notes it runs. Keeping each pair
# on one card removes that term entirely.
#
# Why 3k: the interval narrows with sqrt(n). The 31B pair's half-width was 0.0124
# at n=1001; at n=3002 it should fall to about 0.0072, which would put the whole
# interval above zero IF the point estimate holds. That is the run's registered
# prediction, written down before it starts so it cannot be adjusted afterwards.
# The 12B pair's half-width was 0.019 and goes to about 0.011, which is marginal;
# it runs because the card is otherwise idle, not because it is expected to
# resolve.
#
# Card assignment is forced by VRAM, not preference:
#   12B    6.26 / 6.86 GiB   -> both halves fit the 5080's 15.92 GiB
#   31B   16.10 / 17.53 GiB  -> neither half fits the 5080; XTX only
#   26B   13.27 / 15.84 GiB  -> QAT fits the 5080, non-QAT does not, so the pair
#                               cannot run there at all. It is NOT in this script.
#
# The 5080-to-XTX bound has never been measured (the +/-0.019 figure is
# CUDA-to-CUDA, rented 3090 against local 5080; the XTX is Vulkan on a different
# llama.cpp build). Nothing here compares across the two cards, and nothing
# derived from it should.
set -u
cd "$(dirname "$0")/.." || exit 1
CARD=${1:?usage: mid3k_pairs.sh 5080|xtx}
OUT=results/mid3k
mkdir -p "$OUT"
GOLD=data/corpora/v5/gold_mid.jsonl
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/mid3k_$CARD.log"; }

case "$CARD" in
  5080) PORT=8300
        A_LBL=gemma-4-12B-it.qat.mid3k;  A_REPO=unsloth/gemma-4-12B-it-qat-GGUF:UD-Q4_K_XL
        A_DRAFT=unsloth/gemma-4-12B-it-qat-GGUF:MTP/mtp-gemma-4-12B-it-Q8_0.gguf; A_FAM=gemma-4-12B-it-qat
        B_LBL=gemma-4-12B-it.nonqat.mid3k; B_REPO=unsloth/gemma-4-12b-it-GGUF:UD-Q4_K_XL
        B_DRAFT=unsloth/gemma-4-12b-it-GGUF:MTP/mtp-gemma-4-12b-it-Q8_0.gguf; B_FAM=gemma-4-12b-it ;;
  xtx)  PORT=8810
        A_LBL=gemma-4-31B-it.qat.mid3k;  A_REPO=unsloth/gemma-4-31B-it-qat-GGUF:UD-Q4_K_XL
        A_DRAFT=unsloth/gemma-4-31B-it-qat-GGUF:MTP/mtp-gemma-4-31B-it-Q8_0.gguf; A_FAM=gemma-4-31B-it-qat
        B_LBL=gemma-4-31B-it.nonqat.mid3k; B_REPO=unsloth/gemma-4-31B-it-GGUF:UD-Q4_K_XL
        B_DRAFT=unsloth/gemma-4-31B-it-GGUF:MTP/mtp-gemma-4-31B-it-Q8_0.gguf; B_FAM=gemma-4-31B-it ;;
  *) echo "CARD must be 5080 or xtx"; exit 1 ;;
esac

run_half() {   # $1 label, $2 repo, $3 draft, $4 verify-fam
  local LBL=$1 REPO=$2 DRAFT=$3 FAM=$4
  local PRED="$OUT/$LBL.pred.jsonl"
  if [ -s "$PRED" ] && [ "$(wc -l < "$PRED")" -ge "$EXPECT" ]; then say "SKIP $LBL (banked)"; return 0; fi
  say "--- $LBL on $CARD, nproc=1, MTP, $EXPECT notes"
  local t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$LBL" REPO="$REPO" DRAFT="$DRAFT" VERIFY_FAM="$FAM" \
    CARD="$CARD" NPROC=1 BASE_PORT="$PORT" CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh || { say "FAIL $LBL"; return 1; }
  local t1=$(date +%s)
  if ! python3 harness/score.py --gold "$GOLD" --pred "$PRED" --json-out "$OUT/$LBL.score.json" 2>"$OUT/$LBL.err"; then
    if grep -q 'thinking:true' "$OUT/$LBL.err"; then
      python3 harness/score.py --gold "$GOLD" --pred "$PRED" --allow-thinking-off --json-out "$OUT/$LBL.score.json" >/dev/null 2>&1
    else
      say "BLOCKED $LBL: $(tr '\n' ' ' < "$OUT/$LBL.err" | cut -c1-200)"; return 1
    fi
  fi
  rm -f "$OUT/$LBL.err"
  say "OK $LBL $(python3 -c "
import json;s=json.load(open('$OUT/$LBL.score.json'))['strict']
print('F1=%.4f P=%.4f R=%.4f'%(s['f1'],s['precision'],s['recall']))") wall=$(( (t1-t0)/60 ))m"
}

say "=== 3k QAT pair on the $CARD"
run_half "$A_LBL" "$A_REPO" "$A_DRAFT" "$A_FAM" || exit 1
run_half "$B_LBL" "$B_REPO" "$B_DRAFT" "$B_FAM" || exit 1

say "=== paired bootstrap, QAT against non-QAT, n=$EXPECT, same card ==="
python3 harness/bootstrap_ci.py --gold "$GOLD" \
  --pred "nonQAT=$OUT/$B_LBL.pred.jsonl" \
  --pred "QAT=$OUT/$A_LBL.pred.jsonl" --boot 20000 2>&1 | tee -a "$OUT/mid3k_$CARD.log"
say "=== $CARD 3k PAIR COMPLETE ==="
