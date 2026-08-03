#!/bin/bash
# Re-run the three banked E4B 10k arms at --cache-ram 1024.
#
# They were produced at the 8192 default (finding 20). The E2B ladder now runs
# at 1024, and cache-ram is results-affecting -- whether a prefix is restored or
# recomputed decides the logits, the warm-server effect at 14/20 notes. So
# E2B-vs-E4B comparison at 10k is invalid until both sides share the value.
# Within-family comparisons on either side were never affected.
#
# Waits for the E2B ladder to finish rather than competing with it: both want
# the XTX, and two campaigns on one card is how CT 140 got wrecked (finding 17).
#
# The old arms are quarantined, not overwritten. They are valid measurements at
# a different setting, and the driver's resume check would otherwise treat them
# as banked and skip the re-run entirely.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-sharded}
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/rerun_e4b.log"; }

ARMS="\
E4B.UD-Q4_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL|8300
E4B.UD-Q6_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q6_K_XL|8300
E4B.UD-Q8_K_XL.10k|unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL|8300"

say "=== waiting for the E2B ladder to release the XTX"
while pgrep -f 'finish_10k_xtx[.]sh' >/dev/null 2>&1; do sleep 120; done
say "=== ladder finished; starting E4B re-run at cache-ram 1024"

Q="$OUT/quarantine/E4B-10k-cacheram8192-$(date -u +%Y%m%dT%H%MZ)"
mkdir -p "$Q"
while IFS='|' read -r label repo port; do
  [ -n "${label:-}" ] || continue
  for f in "$OUT/$label.pred.jsonl" "$OUT/${label%.10k}.10k.score.json"; do
    [ -e "$f" ] && mv "$f" "$Q/" && say "  quarantined $(basename "$f")"
  done
done <<< "$ARMS"

cat > "$Q/NOTE.md" <<'EOF'
# SUPERSEDED — E4B 10k arms measured at --cache-ram 8192 (the default)

Three complete, clean arms: Q4 0.6324, Q6 0.6450, Q8 0.6321, 10,000 rows each,
zero errored. Nothing wrong with them as measurements.

Superseded because `--cache-ram` is results-affecting and the E2B ladder now
runs at 1024 (finding 20). Cache reuse decides whether a prefix is restored or
recomputed, and those paths do not produce bit-identical logits -- the
warm-server effect, 14/20 notes. Arms compared to each other must share the
value, exactly like NPROC.

These remain valid for within-E4B comparison at 8192. They are not comparable to
any arm at 1024. Re-run under the same label at cache-ram 1024.
EOF

while IFS='|' read -r label repo port; do
  [ -n "${label:-}" ] || continue
  say "--- $label  $repo  nproc=3 cache-ram=1024"
  t0=$(date +%s)
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="${repo%%:*}" \
    CARD=xtx NPROC=3 BASE_PORT="$port" CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?; t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing"; continue; fi
  python3 harness/score.py --gold "$GOLD" --pred "$OUT/$label.pred.jsonl" \
      --json-out "$OUT/${label%.10k}.10k.score.json" >/dev/null 2>&1
  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/${label%.10k}.10k.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== E4B RERUN COMPLETE ==="
