#!/bin/bash
# LFM2.5-2.6B across its quant range, on the 5080.
#
# LiquidAI released this on 2026-08-01. It is a Liquid Foundation Model -- a
# hybrid convolution/attention architecture rather than a plain transformer --
# and llama.cpp loads it, which was checked before any card time was spent:
# one server on Q4_K_M came up healthy and returned well-formed
# {"facts":[...]} on a real corpus note.
#
# TWO DIFFERENCES FROM THE REST OF THE FIELD, both stated rather than buried,
# because both are the kind of thing this benchmark has been bitten by before.
#
#   QUANT SCHEME. Every other arm here runs Unsloth Dynamic quants
#   (UD-Q4_K_XL / UD-Q6_K_XL / UD-Q8_K_XL). There is no unsloth repo for this
#   model -- only 230M, 1.2B, VL-1.6B and 8B-A1B exist -- so these are standard
#   K-quants from LiquidAI's own GGUF repo: Q4_K_M, Q6_K, Q8_0. Dynamic quants
#   allocate bits per layer differently, so a cross-model comparison at "Q4" has
#   the quantisation scheme inside it. Within this model the three arms are
#   comparable to each other, which is what the quant question needs.
#
#   REASONING IS UNCONDITIONAL. The chat template carries <think> tags but no
#   enable_thinking branch, so the thinking flag is inert -- it reasons whatever
#   is asked. Recorded because three models in this field (granite x2,
#   gemma-3n) emit no reasoning at all and needed --allow-thinking-off; this one
#   is the opposite case and needs no flag.
#
# No MTP: no mtp-*.gguf is published, same as every model outside gemma-4.
#
# TIER: gold_small, 1001 notes. This is the tier the model field was actually
# ranked at -- sweep_v5_top6_gguf.sh ran the original six there, and only four
# models were later escalated to 10k. At ~21 notes/min measured on this card at
# nproc=3, a 10k arm is 7.9h and a 1001-note arm is 48 minutes.
#
# gold_small is a STRICT SUBSET of gold_large, so every banked 10k arm already
# contains its own 1001-note result and can be scored on exactly these notes for
# comparison. What must NOT happen is comparing a 1001 figure against a raw 10k
# one: granite-4.0-1b reads 0.3911 on the small subset against 0.4215 on the full
# 10k, a gap of 0.0304 -- three times the noise threshold. The small tier is a
# harder slice, not a sample of the large one.
#
# Settings otherwise match the banked arms: nproc=3, cache-ram 1024, prompt v8.
#
# THROUGHPUT, measured properly on the live run rather than projected:
# median 910 completion tokens at 8517ms with three processes, ~107 tok/s per
# stream, 21.1 notes/min. Two earlier estimates were wrong and both are worth
# recording as the same mistake at different sample sizes:
#   1 note   -> 2305 tokens, projected ~20h/arm
#   8 notes  -> 420 tokens at 1307ms on ONE server, projected 1.2h/arm
# The second was wrong twice over: the sample under-counted tokens by half, and
# it multiplied single-stream throughput by 3 to get nproc=3. The scaling sweep
# run the same afternoon (defect 35 write-up) shows per-stream falls 359 -> 148
# tok/s from np1 to np3 on this card. The data to catch it already existed.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/lfm25-2.6b}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/lfm25.log"; }

# label|repo:quant
ARMS="\
LFM2.5-2.6B.Q4_K_M|LiquidAI/LFM2.5-2.6B-GGUF:Q4_K_M
LFM2.5-2.6B.Q6_K|LiquidAI/LFM2.5-2.6B-GGUF:Q6_K
LFM2.5-2.6B.Q8_0|LiquidAI/LFM2.5-2.6B-GGUF:Q8_0"

say "=== LFM2.5-2.6B quant sweep on the 5080: Q4_K_M, Q6_K, Q8_0; $EXPECT notes each"

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
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next quant"; continue; fi

  if ! python3 harness/score.py --gold "$GOLD" --pred "$pred" \
        --json-out "$OUT/$label.score.json" 2>"$OUT/$label.score.err"; then
    say "BLOCKED $label -- $(tr '\n' ' ' < "$OUT/$label.score.err" | cut -c1-300)"
    continue
  fi
  rm -f "$OUT/$label.score.err"

  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"

say "=== LFM2.5 QUANT SWEEP COMPLETE ==="
python3 - "$OUT" <<'PY' | while read -r l; do say "$l"; done
import json, glob, os, sys
out = sys.argv[1]
print("SUMMARY LFM2.5-2.6B by quant (strict F1):")
for p in sorted(glob.glob(os.path.join(out, "*.score.json"))):
    label = os.path.basename(p).replace(".score.json", "")
    try:
        d = json.load(open(p))["strict"]
        print("  %-24s f1=%.4f p=%.4f r=%.4f" % (label, d["f1"], d["precision"], d["recall"]))
    except Exception:
        pass
PY
