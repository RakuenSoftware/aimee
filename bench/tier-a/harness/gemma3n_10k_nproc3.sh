#!/bin/bash
# Re-take gemma-3n-E4B at 10k with NPROC=3, correcting the arm banked at 4.
#
# The first arm (0eef1b8ec) ran at nproc=4 because its driver passed NPROC=0 and
# let the sizer choose. Everything else in the comparison -- granite-4.0-1b,
# granite-4.1-3b, Qwen3-1.7B, and the whole E2B/E4B ladder -- ran at 3, and
# process count is worth 0.0105 F1 (finding 19), larger than several of the gaps
# being ranked. So the arm measured a configuration nothing else shares.
#
# The NPROC=0 choice rested on the model's 5.02 GiB file size. That was the wrong
# number: resident VRAM is 3414 MiB, the sizer fitted four, and three fit with
# room. Sizing a run from file size is the mistake, not the sizer.
#
# The nproc=4 arm is quarantined, not overwritten. It is a valid measurement at
# its own setting, and the resume check would otherwise treat it as banked and
# skip this re-run entirely -- the same trap rerun_e4b_10k_cacheram.sh handles.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/10k-5080}
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/missing_10k.log"; }

LABEL=gemma-3n-E4B-it.10k
REPO=unsloth/gemma-3n-E4B-it-GGUF:UD-Q4_K_XL
pred="$OUT/$LABEL.pred.jsonl"

Q="$OUT/quarantine/gemma-3n-10k-nproc4-$(date -u +%Y%m%dT%H%MZ)"
mkdir -p "$Q"
for f in "$pred" "$OUT/$LABEL.score.json"; do
  [ -e "$f" ] && mv "$f" "$Q/" && say "  quarantined $(basename "$f")"
done
cat > "$Q/NOTE.md" <<'EOF'
# SUPERSEDED — gemma-3n-E4B 10k arm measured at nproc=4

Complete, clean arm: strict 0.5429, 10,000 rows, 0 errored, 0 truncated,
parse_ok 9991. Nothing wrong with it as a measurement.

Superseded because every other arm it would be ranked against ran at nproc=3 --
granite-4.0-1b, granite-4.1-3b, Qwen3-1.7B and the entire E2B/E4B 10k ladder.
Process count is results-affecting and worth 0.0105 F1 (finding 19), which is
larger than several of the gaps this ranking is meant to resolve.

Remains valid for within-arm inspection and for any future nproc=4 comparison.
It is not comparable to the arms at 3. Re-run under the same label at NPROC=3.
EOF

say "=== gemma-3n re-take at NPROC=3 (correcting the nproc=4 arm)"
say "--- $LABEL  $REPO  nproc=3  port=8600"
t0=$(date +%s)
GOLD="$GOLD" OUT="$OUT" LABEL="$LABEL" REPO="$REPO" DRAFT="" \
  CARD=5080 NPROC=3 BASE_PORT=8600 CACHE_RAM_MIB=1024 \
  bash harness/shard_run.sh
rc=$?
t1=$(date +%s)
if [ $rc -ne 0 ]; then say "FAIL $LABEL (rc=$rc)"; exit 1; fi

if ! python3 harness/score.py --gold "$GOLD" --pred "$pred" \
      --json-out "$OUT/$LABEL.score.json" 2>"$OUT/$LABEL.score.err"; then
  if grep -q 'thinking:true' "$OUT/$LABEL.score.err"; then
    say "  thinking guard fired; scoring with --allow-thinking-off"
    python3 harness/score.py --gold "$GOLD" --pred "$pred" --allow-thinking-off \
      --json-out "$OUT/$LABEL.score.json" >/dev/null 2>&1
  else
    say "BLOCKED $LABEL -- $(tr '\n' ' ' < "$OUT/$LABEL.score.err" | cut -c1-300)"
    exit 1
  fi
fi
rm -f "$OUT/$LABEL.score.err"
f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$LABEL.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
say "OK   $LABEL strictF1=$f1 wall=$(( (t1-t0)/60 ))m  nproc=3"
say "=== GEMMA-3N NPROC=3 COMPLETE ==="
