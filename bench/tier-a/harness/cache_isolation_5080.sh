#!/bin/bash
# The discriminating test for defect 40, which is currently a hypothesis.
#
# DEFECT 40 OBSERVED: gemma-4-E2B-it-qat run on gold_small (1001) and on gold_mid
# (3002, which strictly contains gold_small) produces byte-identical output on
# only 529 of the 1001 shared notes. Same card, same quant, same nproc=1, same
# prompt. 47% of outputs differ on identical inputs.
#
# THE HYPOTHESIS, and it is only that: --cache-ram 1024 holds roughly 38 context
# entries (finding 20's arithmetic), so the state carried into any request is the
# last ~38 notes rather than the last one. Between a 1001-note and a 3002-note
# corpus almost every note has a different 38-note history, which predicts the
# uniform churn that was observed.
#
# WHY IT IS NOT YET A MEASUREMENT. The obvious cheaper test already failed:
# splitting the churn by whether the immediately preceding note was the same gave
# 44.8% against 48.3%, which explains nothing. That refutes "predecessor" but does
# not confirm "38-note history" -- both are consistent with uniform churn, and so
# is any other mechanism that varies with position. Defect 40 says so explicitly
# and this run is what closes it.
#
# THE TEST. Re-run both corpora with the prompt cache disabled (--cache-ram 0).
# Everything else is held: same model, same quant, same card, same nproc=1, same
# prompt v8, no MTP.
#
# PREDICTION REGISTERED BEFORE THE RUN, because this campaign has already fitted
# one story to a bug and retracted it:
#
#   If cache history is the mechanism, the two cache-off arms must agree on the
#   shared 1001 notes at or very near 1001/1001, against 529/1001 with the cache
#   on. Anything materially below that falsifies the hypothesis and defect 40's
#   explanation gets withdrawn rather than softened.
#
# It also closes article 3's open item 3 -- "isolate concurrency from cache reuse"
# -- from the cache side, and gives a free control the campaign has never had:
# arm A against the banked cache-on gold_small arm (0.6406) measures what the
# prompt cache is worth in F1, which is currently assumed to be nothing.
#
# TIERS. gold_small and gold_mid only. No new corpora, no subsets, no re-scoring
# of anything already banked.
#
# COST. The cache-on mid arm ran 3002 notes in 116m (~26 notes/min). Disabling
# the cache re-evaluates the ~600-token system prompt per note (finding 20), so
# expect roughly 19-22 notes/min: about 50m for gold_small and 2.5-3h for
# gold_mid. Estimated, not measured -- the log records the real figure.
set -u
cd "$(dirname "$0")/.." || exit 1

OUT=${OUT:-results/cache-isolation}
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/cache.log"; }

say "=== defect 40 discriminating test: same model, prompt cache OFF, two tiers"
say "    prediction: shared-1001 byte identity ~1001/1001 (cache on gave 529)"

REPO=google/gemma-4-E2B-it-qat-q4_0-gguf:q4_0
FAM=gemma-4-E2B

# label|gold
ARMS="\
E2B.qat.cacheoff.small|data/corpora/v5/gold_small.jsonl
E2B.qat.cacheoff.mid|data/corpora/v5/gold_mid.jsonl"

while IFS='|' read -r label gold; do
  [ -n "${label:-}" ] || continue
  expect=$(wc -l < "$gold")
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$expect" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $expect notes  nproc=1  no MTP  CACHE_RAM_MIB=0"
  t0=$(date +%s)
  GOLD="$gold" OUT="$OUT" LABEL="$label" REPO="$REPO" DRAFT="" VERIFY_FAM="$FAM" \
    CARD=5080 NPROC=1 BASE_PORT=8980 CACHE_RAM_MIB=0 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing"; continue; fi

  if ! python3 harness/score.py --gold "$gold" --pred "$pred" \
        --json-out "$OUT/$label.score.json" 2>"$OUT/$label.score.err"; then
    if grep -q 'thinking:true' "$OUT/$label.score.err"; then
      say "  thinking guard fired; re-scoring with --allow-thinking-off"
      python3 harness/score.py --gold "$gold" --pred "$pred" --allow-thinking-off \
        --json-out "$OUT/$label.score.json" >/dev/null 2>&1
    else
      say "BLOCKED $label -- $(tr '\n' ' ' < "$OUT/$label.score.err" | cut -c1-300)"
      continue
    fi
  fi
  rm -f "$OUT/$label.score.err"

  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1 wall=$(( (t1-t0)/60 ))m  rate=$(python3 -c "print('%.1f'%($expect/(($t1-$t0)/60.0)))")/min"
done <<< "$ARMS"

say "=== both cache-off arms done; the test follows ==="
A="$OUT/E2B.qat.cacheoff.small.pred.jsonl"
B="$OUT/E2B.qat.cacheoff.mid.pred.jsonl"
if [ -s "$A" ] && [ -s "$B" ]; then
  python3 - "$A" "$B" <<'PY' 2>&1 | tee -a "$OUT/cache.log"
import json, sys
a = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[1])}
b = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[2])}
shared = sorted(set(a) & set(b))
same = sum(1 for i in shared if (a[i].get('raw') or '') == (b[i].get('raw') or ''))
print()
print("shared notes                     %d" % len(shared))
print("byte-identical, cache OFF        %d/%d  (%.1f%%)" % (same, len(shared), 100.0*same/len(shared)))
print("byte-identical, cache ON (d40)   529/1001  (52.8%)")
print()
if same >= 0.99 * len(shared):
    print("VERDICT: cache history CONFIRMED as the mechanism for defect 40.")
elif same > 700:
    print("VERDICT: PARTIAL. The cache explains much of the churn but not all;")
    print("         a second mechanism remains and defect 40 stays a hypothesis.")
else:
    print("VERDICT: REFUTED. The cache is not the mechanism. Defect 40's")
    print("         explanation must be withdrawn, not softened.")
PY
  say "--- control: cache OFF vs banked cache ON, gold_small (what the cache costs in F1)"
  python3 harness/bootstrap_ci.py --gold data/corpora/v5/gold_small.jsonl \
    --pred "cache_on=results/qat-vs-ud/gemma-4-E2B-it.qat.pred.jsonl" \
    --pred "cache_off=$A" --boot 20000 2>&1 | tee -a "$OUT/cache.log"
fi
say "=== CACHE ISOLATION COMPLETE ==="
