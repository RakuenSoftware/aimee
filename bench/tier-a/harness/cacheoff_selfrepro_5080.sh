#!/bin/bash
# The control that should have run before defect 40 had an explanation at all.
#
# Defect 40 observed 529/1001 byte identity for one model run on gold_small
# against the same notes inside gold_mid, and attributed it to prompt-cache
# history. That is now REFUTED: with --cache-ram 0 the same comparison gives
# 499/1001, marginally worse rather than ~1001/1001 as predicted.
#
# Two candidate mechanisms are dead (predecessor identity 44.8 vs 48.3%, cache
# history 49.9 vs 52.8%). Before proposing a third, test the assumption both of
# them rested on: that this configuration reproduces ITSELF.
#
# The 1001/1001 three-way self-reproduction result in the series was measured
# with the cache ON at nproc=3 on the XTX. Cache OFF at nproc=1 on the 5080 has
# never been checked. If this run does not match E2B.qat.cacheoff.small
# byte-for-byte, then corpus composition was never the variable: the arm is
# simply not deterministic in this configuration, and every cross-corpus number
# in defect 40 is measuring that instead.
#
# Identical to the banked arm in every respect: same model, quant, card, nproc,
# prompt, cache setting, gold tier.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=results/cache-isolation
GOLD=data/corpora/v5/gold_small.jsonl
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/cache.log"; }

say "=== self-reproduction control: E2B qat, cache OFF, gold_small, run 2"
say "    prediction: 1001/1001 against run 1 if the configuration is deterministic"
t0=$(date +%s)
GOLD="$GOLD" OUT="$OUT" LABEL=E2B.qat.cacheoff.small.run2 \
  REPO=google/gemma-4-E2B-it-qat-q4_0-gguf:q4_0 DRAFT="" VERIFY_FAM=gemma-4-E2B \
  CARD=5080 NPROC=1 BASE_PORT=8980 CACHE_RAM_MIB=0 \
  bash harness/shard_run.sh
rc=$?; t1=$(date +%s)
[ $rc -ne 0 ] && { say "FAIL run2 rc=$rc"; exit 1; }
say "OK   run2 wall=$(( (t1-t0)/60 ))m"
python3 - "$OUT/E2B.qat.cacheoff.small.pred.jsonl" "$OUT/E2B.qat.cacheoff.small.run2.pred.jsonl" <<'PY' 2>&1 | tee -a "$OUT/cache.log"
import json, sys
a = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[1])}
b = {json.loads(l)['id']: json.loads(l) for l in open(sys.argv[2])}
s = sorted(set(a) & set(b))
same = sum(1 for i in s if (a[i].get('raw') or '') == (b[i].get('raw') or ''))
print()
print("same corpus, same config, two runs: %d/%d identical (%.1f%%)" % (same, len(s), 100.0*same/len(s)))
print("cross-corpus, cache OFF          : 499/1001 (49.9%)")
print()
if same >= 0.99*len(s):
    print("VERDICT: the configuration IS deterministic, so corpus composition is real")
    print("         and its mechanism is still unidentified.")
else:
    print("VERDICT: the configuration does NOT reproduce itself. Corpus composition")
    print("         was never the variable; defect 40 measured nondeterminism.")
PY
say "=== SELF-REPRO CONTROL COMPLETE ==="
