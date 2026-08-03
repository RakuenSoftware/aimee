#!/bin/bash
# How much does ONE arm move when you just run it again?
#
# THE QUESTION THIS SETTLES
#
# E2B UD-Q4_K_XL on the same 1001 notes, same prompt v8, same card, same MTP,
# concurrency 1, scored the same way, twice:
#
#     v8-baseline (1 server)        strict F1 0.6114
#     quant-ledger (3 servers)      strict F1 0.6213
#
# 649 of 1001 raw completions identical. A 0.0099 F1 swing on what was supposed
# to be the same measurement -- larger than BOTH quant effects the campaign has
# been chasing (E2B Q6-Q4 measured +0.0065 and -0.0039 on this very corpus).
#
# If an arm cannot reproduce its own F1 to better than 0.01, then no Q4-vs-Q6
# comparison at that scale means anything, and the "8 replications, same sign"
# argument was reading noise.
#
# WHAT IS CONTROLLED
#
# The two runs above differ in ONE thing: 1 server vs 3 isolated servers. So
# there are two candidate explanations and this separates them.
#
#   A. SAME CONFIG TWICE. 3 servers, run twice. If these agree exactly, process
#      isolation IS reproducible and the v8-baseline gap is a config difference
#      -- which means those two arms were never comparable and comparing them
#      was the error.
#   B. ONE SERVER TWICE. 1 server, run twice, matching v8-baseline's config.
#      Establishes the single-server noise floor independently.
#
# Article 3 claims isolation reproduces sequential exactly, on evidence of 60
# notes and 2 processes. This tests it at 1001 notes and 3 processes.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/noise-floor}
REPO=${REPO:-unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL}
DRAFT=${DRAFT:-unsloth/gemma-4-E2B-it-GGUF}
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/noise.log"; }

run_one() {  # $1 label  $2 nproc
  # Separate statements: under `set -u`, a variable declared earlier in the
  # SAME `local` is not yet visible, so `local a="$1" p="$OUT/$a"` aborts with
  # "a: unbound variable". That killed this script one line into its first arm.
  local label="$1"
  local n="$2"
  local pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$(wc -l < "$GOLD")" ]; then say "SKIP $label"; return 0; fi
  say "--- $label ($n proc)"
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$REPO" DRAFT="$DRAFT" \
    CARD=5080 NPROC="$n" BASE_PORT=8400 bash harness/shard_run.sh || { say "FAIL $label"; return 1; }
  python3 harness/score.py --gold "$GOLD" --pred "$pred" --json-out "$OUT/$label.score.json" >/dev/null 2>&1
  local f1; f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  say "OK   $label strictF1=$f1"
  git add -f "$pred" "$OUT/$label.score.json" "$OUT/noise.log" 2>/dev/null \
    && git commit -q -m "bench(noise-floor): $label" 2>/dev/null && say "  committed $label"
}

say "=== noise floor: same arm, repeated"
run_one shard3_run1 3
run_one shard3_run2 3
run_one single_run1 1
run_one single_run2 1

say "=== comparison"
python3 - "$OUT" "$GOLD" <<'PY' | tee -a "$OUT/noise.log"
import json,sys,os,itertools
out,gold=sys.argv[1],sys.argv[2]
def load(p):
    return {json.loads(l)["id"]: json.loads(l) for l in open(p)} if os.path.exists(p) else None
def f1(l):
    p=f"{out}/{l}.score.json"
    return json.load(open(p))["strict"]["f1"] if os.path.exists(p) else None
def tr(r):
    return {(str(f.get("subject","")).strip().lower(),str(f.get("relation","")).strip().lower(),
             str(f.get("object","")).strip().lower()) for f in (r.get("pred_nofloor") or r.get("pred") or [])}
labs=["shard3_run1","shard3_run2","single_run1","single_run2"]
# include the two pre-existing runs of this identical arm
extra={"v8baseline_1srv":"results/v8-baseline/E2B.UD-Q4_K_XL.mtp.pred.jsonl",
       "ledger_3srv":"results/quant-ledger/v5small.E2B.UD-Q4_K_XL.pred.jsonl"}
data={l:load(f"{out}/{l}.pred.jsonl") for l in labs}
data.update({k:load(v) for k,v in extra.items()})
data={k:v for k,v in data.items() if v}
print("\n  strict F1 per run:")
for l in labs:
    v=f1(l)
    if v is not None: print(f"    {l:16s} {v:.4f}")
print("\n  pairwise raw identity:")
for a,b in itertools.combinations(sorted(data),2):
    ids=[i for i in data[a] if i in data[b]]
    if not ids: continue
    raw=sum(1 for i in ids if data[a][i].get("raw")==data[b][i].get("raw"))
    fac=sum(1 for i in ids if tr(data[a][i])==tr(data[b][i]))
    print(f"    {a:16s} vs {b:16s}  raw {raw:4d}/{len(ids)}  facts {fac:4d}/{len(ids)}")
PY
say "=== NOISE FLOOR DONE ==="
