#!/bin/bash
# The overnight 10k ladder: E2B on the 5080, E4B on the XTX, both sharded.
#
# CONFIGURATION, and why each part is what it is:
#
#   MTP on          1.9x, and repeatable run-to-run (100/100 on both families).
#   N shards        isolated single-slot processes, NOT -np N. Slots batch into a
#                   shared forward pass and are irreproducible (44/60 between two
#                   identical runs); separate processes are 120/120.
#   N pinned        per CARD, from the measured largest arm. Auto-sizing per arm
#                   would give Q4 more processes than Q8, and arms under different
#                   shard counts are not comparable to each other.
#   concurrency 1   per process. Any in-process batching reintroduces the shared
#                   forward pass.
#
# These arms are a DIFFERENT CONFIGURATION from the 1001-note single-process arms
# measured earlier: sharding differs from single-process on 89/120 notes. They are
# comparable to each other, not to those. Recorded here so a later reader does not
# quietly cross-compare them.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/overnight.log"; }

E2B_SHARDS=${E2B_SHARDS:-4}
E4B_SHARDS=${E4B_SHARDS:-3}

run_card() {  # $1 card  $2 model  $3 shards  $4 base port
  local card=$1 model=$2 shards=$3 port=$4
  for q in Q4 Q6 Q8; do
    local label="${model}.UD-${q}_K_XL.10k"
    if [ -s "$OUT/$label.pred.jsonl" ] && \
       [ "$(wc -l < "$OUT/$label.pred.jsonl")" -ge "$(wc -l < "$GOLD")" ]; then
      say "[$card] SKIP $label"; continue
    fi
    say "[$card] START $label ($shards shards)"
    GOLD="$GOLD" OUT="$OUT" LABEL="$label" \
      REPO="unsloth/gemma-4-${model}-it-GGUF:UD-${q}_K_XL" \
      DRAFT="unsloth/gemma-4-${model}-it-GGUF" \
      CARD="$card" NPROC="$shards" BASE_PORT="$port" \
      bash harness/shard_run.sh >>"$OUT/overnight_$card.log" 2>&1
    local got=0
    [ -s "$OUT/$label.pred.jsonl" ] && got=$(wc -l < "$OUT/$label.pred.jsonl")
    if [ "$got" -ge "$(wc -l < "$GOLD")" ]; then
      python3 harness/score.py --gold "$GOLD" --pred "$OUT/$label.pred.jsonl" \
        --json-out "$OUT/$label.score.json" >/dev/null 2>&1
      local f1
      f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "scorer-refused")
      say "[$card] OK $label rows=$got strictF1=$f1"
    else
      say "[$card] FAIL $label rows=$got"
    fi
  done
  say "[$card] === DONE ==="
}

say "overnight 10k: E2B x$E2B_SHARDS on the 5080, E4B x$E4B_SHARDS on the XTX, in parallel"
run_card 5080 E2B "$E2B_SHARDS" 8200 &
P1=$!
run_card xtx  E4B "$E4B_SHARDS" 8300 &
P2=$!
wait $P1 $P2
say "=== OVERNIGHT COMPLETE ==="
