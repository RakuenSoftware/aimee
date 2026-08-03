#!/bin/bash
# The four remaining 10k arms, all on the XTX, one at a time.
#
# The 5080 is out of the plan entirely. CT 140 turned out to be shared with
# another session, and the two of us spent the night killing each other's
# servers: our E2B Q4 arm died with 9725 transport errors, and our own
# container-wide pkill was doing the same to them. Their server is left running.
#
# Shard counts are pinned at 3 for EVERY arm, and this is not a tuning knob.
#
# It was 4 for the E2B arms, on the reasoning that E4B was already banked at 3
# and no E2B arm had finished, so E2B was "free to choose". Both halves of that
# were wrong.
#
# 1. Process count changes the answer. Measured 2026-08-03 on the same corpus:
#    one process against three moves 349 of 1001 notes and 0.0105 strict F1.
#    That is larger than either quant effect this ladder exists to resolve, so
#    an E2B ladder at 4 could never have been compared to the E4B ladder at 3.
#    No arm is ever free to choose its process count; the campaign chooses once.
#
# 2. "Fits with room to spare" was a VRAM argument, and the binding limit here
#    is system RAM. Four servers at ~6.8 GB RSS on a 30 GB host left ~3 GB free
#    and the kernel killed a server twice mid-arm, silently -- the logs just
#    stop. Three fits with the room the old comment claimed.
#
# The abandoned 4-process attempt is quarantined under
# results/10k-sharded/quarantine/ with its own note.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/finish_xtx.log"; }

# label|repo|nproc|base_port
ARMS="\
E4B.UD-Q8_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL|3|8300
E2B.UD-Q4_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|3|8400
E2B.UD-Q6_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|3|8400
E2B.UD-Q8_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|3|8400"

say "=== XTX-only finish: 4 arms, $EXPECT notes each"
while IFS='|' read -r label repo nproc port; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  # A previous attempt's rejected output must not be mistaken for a fresh start,
  # but it is still raw output and deleting it loses the only record of how the
  # attempt failed. Move it aside with a timestamp instead.
  if [ -e "$pred.errored" ]; then
    mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"
  fi

  say "--- $label  nproc=$nproc  port=$port"
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" \
    DRAFT="${repo%%:*}" CARD=xtx NPROC="$nproc" BASE_PORT="$port" \
    bash harness/shard_run.sh
  rc=$?
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next arm"; continue; fi

  python3 harness/score.py --gold "$GOLD" --pred "$pred" \
    --json-out "$OUT/${label%.10k}.10k.score.json" >/dev/null 2>&1
  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/${label%.10k}.10k.score.json'))['strict']['f1'])" 2>/dev/null || echo "scorer-refused")
  say "OK   $label strictF1=$f1"
done <<< "$ARMS"
say "=== XTX FINISH COMPLETE ==="
