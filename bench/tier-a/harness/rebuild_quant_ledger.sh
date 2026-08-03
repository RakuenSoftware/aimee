#!/bin/bash
# Rebuild the Q4/Q6 replication ledger from scratch, on the 5080.
#
# WHY THIS EXISTS
#
# The Q4->Q6 comparison was run at least eight times across this campaign and
# the evidence for almost none of it survives. Nine corpus/model slots kept the
# Q4 arm and dropped the Q6 arm. The 69-note ladder of 2026-08-01 went into
# QUANT_DECISION.md as six F1 values and a bootstrap table with ZERO artifacts
# committed; commits 8e45aba76 and 5c6cf291f are markdown-only and those runs
# are unrecoverable. The claim "8 data points, 5 corpora" then propagated into
# an article as a measurement, sourced from a line reading "the operator reports
# having seen" -- about runs this harness did itself.
#
# BOTH HALVES, ALWAYS
#
# Re-running only the missing Q6 arms would be worthless: the surviving Q4 arms
# were produced on other cards, other configs, other prompt versions. A pair is
# only a pair if both halves ran under one configuration. So every comparison
# here re-runs Q4 AND Q6 together even where a Q4 arm already exists.
#
# NOT A REPRODUCTION
#
# The original ladder ran on the XTX under RADV Vulkan. This is CUDA. It will
# not return 0.7206 / 0.8062 and is not expected to. It is an independent
# replication, which is what the sign argument actually needs.
#
# EVERY ARM IS COMMITTED WHEN IT FINISHES
#
# Not at the end. The whole reason this script is necessary is that results sat
# uncommitted until the machine that held them moved on.
set -u
cd "$(dirname "$0")/.." || exit 1

OUT=${OUT:-results/quant-ledger}
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/ledger.log"; }

# corpus_label|gold path|quants|nproc_e2b|nproc_e4b
CORPORA="\
gold70|data/gold.jsonl|Q4 Q6 Q8|3|2
v5small|data/corpora/v5/gold_small.jsonl|Q4 Q6|3|2
v3small|data/corpora/v3/gold_small.jsonl|Q4 Q6|3|2"

commit_arm() {  # $1 label
  # Paths are relative to CWD, which is bench/tier-a because this script cd'd
  # there. Prefixing "bench/tier-a/" made every path bench/tier-a/bench/tier-a/*
  # and git add failed silently on the first arm. A commit step that cannot
  # commit is the exact defect this script was written to fix.
  local f="$OUT/$1.pred.jsonl" s="$OUT/$1.score.json"
  if [ ! -s "$f" ]; then say "  WARNING: no prediction file for $1, nothing to commit"; return 0; fi
  local err
  for _ in 1 2 3 4 5; do
    err=$(git add -f "$f" "$s" "$OUT/ledger.log" 2>&1 \
          && git commit -q -m "bench(quant-ledger): $1

Committed by rebuild_quant_ledger.sh the moment the arm finished, because the
predecessor of this run was written up and never committed at all." 2>&1)
    if [ $? -eq 0 ]; then say "  committed $1"; return 0; fi
    sleep 4   # another process holds index.lock
  done
  say "  WARNING: could not commit $1 -- COMMIT BY HAND. git said: $err"
}

say "=== quant ledger rebuild on the 5080"
while IFS='|' read -r corpus gold quants n2 n4; do
  [ -n "${corpus:-}" ] || continue
  [ -s "$gold" ] || { say "SKIP $corpus: no gold at $gold"; continue; }
  expect=$(wc -l < "$gold")
  for fam in E2B E4B; do
    nproc=$n2; [ "$fam" = E4B ] && nproc=$n4
    for q in $quants; do
      label="$corpus.$fam.UD-${q}_K_XL"
      pred="$OUT/$label.pred.jsonl"
      if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$expect" ]; then say "SKIP $label"; continue; fi
      say "--- $label  ($expect notes, $nproc procs)"
      GOLD="$gold" OUT="$OUT" LABEL="$label" \
        REPO="unsloth/gemma-4-${fam}-it-GGUF:UD-${q}_K_XL" \
        DRAFT="unsloth/gemma-4-${fam}-it-GGUF" \
        CARD=5080 NPROC="$nproc" BASE_PORT=8400 \
        bash harness/shard_run.sh || { say "FAIL $label"; continue; }
      python3 harness/score.py --gold "$gold" --pred "$pred" \
        --json-out "$OUT/$label.score.json" >/dev/null 2>&1
      f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
      say "OK   $label strictF1=$f1"
      commit_arm "$label"
    done
  done
done <<< "$CORPORA"

say "=== ledger: every pair that now has BOTH halves under one config"
python3 - "$OUT" <<'PY' | tee -a "$OUT/ledger.log"
import json,glob,os,sys,re,collections
out=sys.argv[1]; by=collections.defaultdict(dict)
for p in glob.glob(f"{out}/*.score.json"):
    m=re.match(r'(.+?)\.(E2B|E4B)\.UD-(Q\d)_K_XL\.score\.json', os.path.basename(p))
    if not m: continue
    try: by[(m.group(1),m.group(2))][m.group(3)]=json.load(open(p))["strict"]["f1"]
    except Exception: pass
pos=tot=0
for k in sorted(by):
    q=by[k]
    if "Q4" in q and "Q6" in q:
        d=q["Q6"]-q["Q4"]; tot+=1; pos+= d>0
        print(f"  {k[0]:9s} {k[1]}  Q4={q['Q4']:.4f} Q6={q['Q6']:.4f}  delta={d:+.4f}  {'+' if d>0 else '-'}")
print(f"\n  {pos}/{tot} pairs positive, from THIS run alone")
PY
say "=== REBUILD COMPLETE ==="
