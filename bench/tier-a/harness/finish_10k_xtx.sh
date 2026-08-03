#!/bin/bash
# The four remaining 10k arms, all on the XTX, one at a time.
#
# The 5080 is out of the plan entirely. CT 140 turned out to be shared with
# another session, and the two of us spent the night killing each other's
# servers: our E2B Q4 arm died with 9725 transport errors, and our own
# container-wide pkill was doing the same to them. Their server is left running.
#
# Shard counts are pinned per FAMILY, not measured per arm. E4B Q4 and Q6 are
# already banked at 3 processes, so E4B Q8 runs at 3 or it is not comparable to
# them. No E2B arm has completed, so E2B is free to choose -- 4, which fits with
# room to spare, because an arm that OOMs at hour two costs more than the fourth
# process saves.
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
E2B.UD-Q4_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|4|8400
E2B.UD-Q6_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|4|8400
E2B.UD-Q8_K_XL.10k|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|4|8400"

say "=== XTX-only finish: 4 arms, $EXPECT notes each"
while IFS='|' read -r label repo nproc port; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  # A previous attempt's rejected output must not be mistaken for a fresh start.
  rm -f "$pred.errored"

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
