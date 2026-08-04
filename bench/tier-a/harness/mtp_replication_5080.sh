#!/bin/bash
# Does MTP change the grade? Asked a second time, on the other card.
#
# The XTX lane (nomtp_ladder_xtx.sh) pairs new no-MTP arms against the MTP arms
# already banked there. This lane asks the same question end to end on the 5080:
# it runs BOTH sides itself, so the comparison never crosses hardware and the
# answer does not depend on the XTX's banked arms being comparable to anything.
#
# Two independent answers is the point. If MTP moves the score on one card and
# not the other, that is a finding about the interaction rather than about MTP,
# and a single lane could not have told them apart.
#
# E2B only. E4B at Q6/Q8 is 7-8 GiB on disk and three copies plus KV are not a
# safe bet on a 16303 MiB card; the XTX lane covers E4B.
#
# Arms are ordered in PAIRS -- Q4 mtp, Q4 nomtp, Q6 mtp, ... -- so each quant's
# comparison completes before the next starts. A lane ordered by mode would give
# no usable pairing until it was half done.
#
# Held to the banked arms exactly: nproc=3, cache-ram 1024, prompt v8, thinking
# on, same quants, same gold. Within a pair the ONLY difference is DRAFT.
#
# NPROC is pinned, so the sizer measures resident VRAM but does not veto: an arm
# that cannot fit three copies fails loudly on "server never healthy" and the
# driver moves to the next arm rather than silently running a different shape.
# Q8 is the one at risk and is ordered last for that reason.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-5080-e2b}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/replication.log"; }

# label|repo|draft ("" = no MTP)
ARMS="\
E2B.UD-Q4_K_XL.mtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|unsloth/gemma-4-E2B-it-GGUF
E2B.UD-Q4_K_XL.nomtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|
E2B.UD-Q6_K_XL.mtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|unsloth/gemma-4-E2B-it-GGUF
E2B.UD-Q6_K_XL.nomtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|
E2B.UD-Q8_K_XL.mtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|unsloth/gemma-4-E2B-it-GGUF
E2B.UD-Q8_K_XL.nomtp|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|"

say "=== MTP replication on the 5080: E2B, 3 quants x {mtp,nomtp}, $EXPECT notes each"

while IFS='|' read -r label repo draft; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  if [ -n "$draft" ]; then mode="MTP"; else mode="no-MTP"; fi
  say "--- $label  $repo  nproc=3  $mode"
  t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="$draft" \
    CARD=5080 NPROC=3 BASE_PORT=8800 CACHE_RAM_MIB=1024 \
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
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m  $mode"

  # When a pair completes, print both sides and the delta on one line so the
  # comparison is legible in the log without opening two files.
  case "$label" in
    *.nomtp)
      base="${label%.nomtp}"
      python3 - "$OUT" "$base" <<'PY' | while read -r l; do say "$l"; done
import json, sys, os
out, base = sys.argv[1], sys.argv[2]
try:
    m = json.load(open(os.path.join(out, base + ".mtp.score.json")))["strict"]
    n = json.load(open(os.path.join(out, base + ".nomtp.score.json")))["strict"]
except Exception:
    sys.exit(0)
print("PAIR %s  MTP=%.4f  noMTP=%.4f  delta=%+.4f" % (base, m["f1"], n["f1"], n["f1"] - m["f1"]))
PY
      ;;
  esac
done <<< "$ARMS"
say "=== MTP REPLICATION COMPLETE ==="
