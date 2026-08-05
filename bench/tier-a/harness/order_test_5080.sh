#!/bin/bash
# The third hypothesis for defect 40, with its prediction registered first.
#
# WHAT IS KNOWN. The observation is real: the same 1001 notes score 0.6406 run
# alone and 0.6327 inside gold_mid, with 529/1001 byte identity. Two explanations
# are dead. Predecessor identity: 44.8% churn with the same predecessor against
# 48.3% with a different one. Prompt-cache history: with --cache-ram 0 the
# cross-corpus identity is 499/1001, no better than the 529/1001 with it on.
#
# WHAT IS NEW. The self-reproduction control now says the configuration IS
# deterministic: the same corpus run twice, cache off, gives 1001/1001. So the
# churn is not nondeterminism, and with the prompt cache disabled the only thing
# still differing between a gold_small run and a gold_mid run is WHERE each note
# sits in the sequence. llama-server keeps a live KV context per slot across
# requests; --cache-ram governs the prompt cache, not that.
#
# THE TEST. Same 1001 notes, same everything, SHUFFLED ORDER (seed 20260805, so
# the shuffle itself reproduces). Scoring is unaffected because score.py matches
# on id, and this is the same gold set rather than a new one.
#
# PREDICTION, REGISTERED BEFORE THE RUN:
#   If sequence position is the mechanism, byte identity against the unshuffled
#   run should be FAR below 1001/1001 and in the neighbourhood of the 499/1001
#   seen across corpora.
#   If it comes back at or near 1001/1001, position is NOT the mechanism, this
#   hypothesis dies with the other two, and I stop proposing mechanisms until
#   something else changes.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=results/cache-isolation
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/cache.log"; }
say "=== order test: same 1001 notes, shuffled, cache OFF, 5080, nproc=1"
say "    prediction: near 499/1001 if position is the mechanism; ~1001/1001 kills it"
t0=$(date +%s)
GOLD=.scratch/gold_small_shuffled.jsonl OUT="$OUT" LABEL=E2B.qat.cacheoff.shuffled \
  REPO=google/gemma-4-E2B-it-qat-q4_0-gguf:q4_0 DRAFT="" VERIFY_FAM=gemma-4-E2B \
  CARD=5080 NPROC=1 BASE_PORT=8980 CACHE_RAM_MIB=0 \
  bash harness/shard_run.sh
rc=$?; t1=$(date +%s)
[ $rc -ne 0 ] && { say "FAIL order test rc=$rc"; exit 1; }
say "OK   shuffled run wall=$(( (t1-t0)/60 ))m"
python3 - "$OUT/E2B.qat.cacheoff.small.pred.jsonl" "$OUT/E2B.qat.cacheoff.shuffled.pred.jsonl" <<'PY' 2>&1 | tee -a "$OUT/cache.log"
import json, sys
a = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[1])}
b = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[2])}
s = sorted(set(a) & set(b))
same = sum(1 for i in s if (a[i].get('raw') or '') == (b[i].get('raw') or ''))
print()
print("same notes, shuffled order, cache OFF: %d/%d identical (%.1f%%)" % (same, len(s), 100.0*same/len(s)))
print("same notes, same order,     cache OFF: 1001/1001 (100.0%)")
print("same notes, inside gold_mid, cache OFF: 499/1001 (49.9%)")
print()
if same >= 0.99*len(s):
    print("VERDICT: position is NOT the mechanism. Third hypothesis dead. Stop")
    print("         proposing mechanisms until something else changes.")
elif same <= 0.70*len(s):
    print("VERDICT: position IS the mechanism. Corpus composition acts through")
    print("         sequence position, and a subset is not a run because the notes")
    print("         sit somewhere else in the queue.")
else:
    print("VERDICT: PARTIAL. Position explains some of the churn and not all.")
PY
say "=== ORDER TEST COMPLETE ==="
