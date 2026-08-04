#!/bin/bash
# Does MTP change the GRADE, or only the wall clock?
#
# Every 10k arm banked so far runs MTP drafts. Speculative decoding is supposed
# to be output-preserving -- the draft is verified against the target model, so
# accepted tokens are the ones the target would have produced. This measures
# whether that holds on this task rather than assuming it, by re-taking the whole
# ladder with DRAFT="" and nothing else changed.
#
# Two reasons it might not hold, both already observed in this benchmark:
# batching and cache reuse change logits (finding 3, the warm-server effect at
# 14/20 notes), and MTP moves 26 of 100 notes relative to a sequential run --
# a number that was read as speed and never checked against the score.
#
# ALL SIX ARMS ON THE XTX, SEQUENTIALLY, GROUPED BY MODEL.
#
# Every MTP arm this pairs against is banked on the XTX at cache-ram 1024. A
# model's MTP and no-MTP sides must share a card or the card sits inside the
# comparison, and this benchmark moves more between configurations than between
# the things being compared. Putting the no-MTP side anywhere else would mean
# re-taking the MTP side there too -- six extra arms to answer the same question,
# and a second E2B lineage that could not be compared with the ladder of record.
#
# So this is the whole job, and it is serial by necessity rather than by choice.
# Splitting it across cards was tried twice and is wrong both ways: E4B-only here
# with E2B on the 5080 puts 3 arms on one card and 6 on the other, which is
# slower than 6 here, and it forks E2B into two incomparable sets.
#
# The 5080 is left free for unrelated work.
#
# Grouped by model rather than interleaved by quant: all three E2B arms, then all
# three E4B arms. Same total, and the E2B comparison completes as a set.
#
# Everything else is held to the banked arms exactly: nproc=3, cache-ram 1024,
# prompt v8, thinking on, same quants, same gold. The ONLY difference is DRAFT.
#
# Output goes to a separate directory so the MTP arms stay banked under their own
# labels and the pairing is by name across the two directories.
#
# Ordered Q4, Q6, Q8 so the cheapest pairing completes first.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-nomtp}
MTP_OUT=${MTP_OUT:-results/10k-sharded}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/nomtp.log"; }

# label|repo|base_port
ARMS="\
E2B.UD-Q4_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|8700
E2B.UD-Q6_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|8700
E2B.UD-Q8_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|8700
E4B.UD-Q4_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL|8700
E4B.UD-Q6_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q6_K_XL|8700
E4B.UD-Q8_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL|8700"

say "=== XTX: no-MTP ladder, 6 arms (E2B x3 then E4B x3), $EXPECT notes each"

while IFS='|' read -r label repo port; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $repo  nproc=3  NO MTP"
  t0=$(date +%s)
  # DRAFT exported EMPTY on purpose: this is the variable under test.
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" \
    CARD=xtx NPROC=3 BASE_PORT="$port" CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next arm"; continue; fi

  if ! python3 harness/score.py --gold "$GOLD" --pred "$pred" \
        --json-out "$OUT/$label.score.json" 2>"$OUT/$label.score.err"; then
    say "BLOCKED $label -- $(tr '\n' ' ' < "$OUT/$label.score.err" | cut -c1-300)"
    continue
  fi
  rm -f "$OUT/$label.score.err"

  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  # Print the paired MTP figure on the same line so the comparison is legible in
  # the log without opening two files.
  mtp=$(python3 -c "
import json;print('%.4f'%json.load(open('$MTP_OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "n/a")
  say "OK   $label noMTP=$f1  MTP=$mtp  wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== NO-MTP LADDER COMPLETE ==="
