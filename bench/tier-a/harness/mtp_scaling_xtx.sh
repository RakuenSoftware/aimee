#!/bin/bash
# Why is MTP worth 1.59x in one measurement and 5.3x in another?
#
# finding 12: E2B, concurrency 1, ONE process, n=100 -- 27.0 notes/min sequential
#             against 43.0 with MTP. 1.59x.
# 10k ladder: E2B, THREE processes, n=10000 -- 68.5 notes/min with MTP, and the
#             no-MTP arm is tracking ~14. 5.3x, and worse than that: three
#             processes without MTP are running at HALF the throughput finding 12
#             measured from one.
#
# Three processes delivering less than one is the anomaly. The speedup ratio is
# downstream of it.
#
# HYPOTHESIS. At batch size 1 the card is memory-bandwidth-bound with compute
# idle -- article 3 already establishes this, and it is why both speculation and
# batching help at all. Concurrent processes each decoding at batch 1 contend for
# that same bandwidth, so per-stream throughput falls as processes are added.
# MTP issues fewer forward passes per accepted token, so it needs less bandwidth
# per token and degrades more slowly. If that is right, MTP's measured speedup is
# not a property of MTP alone -- it grows with process count, and quoting one
# number for it is as wrong as quoting one number across models (finding 12
# already found that for E4B vs E2B).
#
# PREDICTIONS, written before the run so they can be wrong:
#   1. per-stream tok/s falls monotonically with nproc, in BOTH modes.
#   2. it falls FASTER without MTP, so the mtp/nomtp ratio widens with nproc.
#   3. aggregate notes/min still rises with nproc (or the sharding is pointless),
#      but sublinearly, and the knee is what article 3's open item 1 asks for.
#   4. at nproc=1 the ratio lands near finding 12's 1.59x. If it does not, the
#      difference is NOT process count and this hypothesis is dead.
#
# THIS MEASURES SPEED ONLY. It runs a 200-note slice and the outputs are NOT
# scored -- a throughput number does not need the full corpus, and a 200-note F1
# would be meaningless at this benchmark's interval width. Every config sees the
# SAME 200 notes so the comparison is like-for-like.
#
# Runs on the XTX while the XTX works the no-MTP ladder. Different cards, so the
# absolute numbers are not comparable to the XTX arms -- the SHAPE of the curve
# is the result, not its height.
set -u
cd "$(dirname "$0")/.." || exit 1

FULL=${FULL:-data/corpora/v5/gold_large.jsonl}
OUT=${OUT:-results/mtp-scaling-xtx}
N=${N:-200}
mkdir -p "$OUT"
SLICE="$OUT/slice_${N}.jsonl"
head -n "$N" "$FULL" > "$SLICE"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/scaling.log"; }

REPO=unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL
DRAFT_REPO=unsloth/gemma-4-E2B-it-GGUF

say "=== MTP scaling on the XTX: E2B UD-Q4_K_XL, $N notes, nproc 1..4 x {mtp,nomtp}"
say "    speed only; these outputs are not scored"

for np in 1 2 3 4; do
  for mode in mtp nomtp; do
    label="scale.np${np}.${mode}"
    pred="$OUT/$label.pred.jsonl"
    if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$N" ]; then say "SKIP $label (banked)"; continue; fi
    if [ "$mode" = mtp ]; then draft="$DRAFT_REPO"; else draft=""; fi

    say "--- $label  nproc=$np  $mode"
    t0=$(date +%s)
    GOLD="$SLICE" OUT="$OUT" LABEL="$label" REPO="$REPO" DRAFT="$draft" \
      CARD=xtx NPROC="$np" BASE_PORT=8950 CACHE_RAM_MIB=1024 \
      bash harness/shard_run.sh
    rc=$?
    t1=$(date +%s)
    if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing"; continue; fi

    python3 - "$pred" "$((t1-t0))" "$label" <<'PY' | while read -r l; do say "$l"; done
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
wall = int(sys.argv[2]); label = sys.argv[3]
ps = [r["completion_tokens"]/(r["latency_ms"]/1000)
      for r in rows if r.get("completion_tokens") and r.get("latency_ms")]
tot = sum(r.get("completion_tokens") or 0 for r in rows)
print("RESULT %s n=%d per-stream=%.1f tok/s aggregate=%.1f tok/s notes/min=%.1f "
      "med_tok=%.0f med_lat=%.0fms wall=%ds"
      % (label, len(rows), statistics.median(ps) if ps else 0, tot/wall if wall else 0,
         len(rows)*60/wall if wall else 0,
         statistics.median([r.get("completion_tokens") or 0 for r in rows]),
         statistics.median([r.get("latency_ms") or 0 for r in rows]), wall))
PY
  done
done

say "=== SCALING SWEEP COMPLETE ==="
python3 - "$OUT" <<'PY' | while read -r l; do say "$l"; done
import json, glob, os, statistics, sys
out = sys.argv[1]
rows_by = {}
for p in sorted(glob.glob(os.path.join(out, "scale.np*.pred.jsonl"))):
    label = os.path.basename(p).replace(".pred.jsonl", "")
    _, np_, mode = label.split(".")
    rows = [json.loads(l) for l in open(p) if l.strip()]
    ps = [r["completion_tokens"]/(r["latency_ms"]/1000)
          for r in rows if r.get("completion_tokens") and r.get("latency_ms")]
    rows_by[(int(np_[2:]), mode)] = statistics.median(ps) if ps else 0
print("SUMMARY per-stream tok/s, and the mtp/nomtp ratio at each process count:")
for n in sorted({k[0] for k in rows_by}):
    m, nm = rows_by.get((n, "mtp")), rows_by.get((n, "nomtp"))
    if m and nm:
        print("  nproc=%d  mtp=%6.1f  nomtp=%6.1f  ratio=%.2fx" % (n, m, nm, m/nm))
PY
