#!/bin/bash
# Repair an arm that was banked with errored rows, by re-running only those notes.
#
# shard_run.sh now does this inline, but an arm produced by the OLD driver has
# already been moved aside to <LABEL>.pred.jsonl.errored and its servers may or
# may not still be up. This repairs such a file after the fact.
#
# Legitimacy: this configuration reproduces itself byte-for-byte (ARTICLE_NOTES
# finding 19 -- three independent runs of one arm, 1001/1001 raw identical), so
# a note re-answered on an identically configured server is the answer the clean
# run would have produced. That argument does NOT extend to a different slot
# count, a different card or a different build, so the model is verified on the
# port before anything is written.
#
# Corrections are additive: the original .errored file is never overwritten, and
# a .repair.json note records which ids were replaced and when.
set -u
cd "$(dirname "$0")/.." || exit 1

PRED=${PRED:?set PRED to the .errored (or .pred.jsonl) file}
GOLD=${GOLD:?set GOLD}
LABEL=${LABEL:?set LABEL}
BASE_URL=${BASE_URL:?set BASE_URL e.g. http://127.0.0.1:8400}
OUT_PRED=${OUT_PRED:?set OUT_PRED — where the repaired arm should land}
WANT_FAM=${WANT_FAM:-}     # e.g. E2B  (optional but checked when set)
WANT_Q=${WANT_Q:-}         # e.g. UD-Q4_K_XL

say() { echo "[$(date -u +%H:%M:%SZ)] $*"; }

[ -s "$PRED" ] || { say "FAIL: $PRED missing or empty"; exit 1; }
[ -e "$OUT_PRED" ] && { say "FAIL: $OUT_PRED already exists, refusing to overwrite"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-.}/repair.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT

python3 -c "
import json,sys
for l in open(sys.argv[1]):
    r=json.loads(l)
    if r.get('error'): print(r['id'])" "$PRED" > "$WORK/ids"
n=$(grep -c . "$WORK/ids" || true)
if [ "${n:-0}" -eq 0 ]; then say "nothing to repair in $PRED"; exit 0; fi
say "$n errored row(s) to repair: $(tr '\n' ' ' < "$WORK/ids")"

# A health check proves something is listening, not that it is the right model.
curl -sf --max-time 8 "$BASE_URL/health" >/dev/null 2>&1 || { say "FAIL: $BASE_URL not healthy"; exit 1; }
loaded=$(curl -sf --max-time 8 "$BASE_URL/props" 2>/dev/null \
  | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])" 2>/dev/null)
say "  serving: ${loaded:-<none>}"
[ -n "$loaded" ] || { say "FAIL: no /props from $BASE_URL"; exit 1; }
if [ -n "$WANT_FAM" ]; then case "$loaded" in *"$WANT_FAM"*) ;; *) say "FAIL: expected $WANT_FAM, got $loaded"; exit 1;; esac; fi
if [ -n "$WANT_Q" ];   then case "$loaded" in *"$WANT_Q"*)   ;; *) say "FAIL: expected $WANT_Q, got $loaded";   exit 1;; esac; fi

python3 - "$GOLD" "$WORK/ids" "$WORK/gold.jsonl" <<'PY'
import json,sys
gold, idfile, out = sys.argv[1], sys.argv[2], sys.argv[3]
want={l.strip() for l in open(idfile) if l.strip()}
with open(out,"w") as fh:
    for line in open(gold):
        if json.loads(line)["id"] in want: fh.write(line)
PY

say "re-running $n note(s) against $BASE_URL"
python3 harness/run_llamacpp.py --model "$LABEL" --gold "$WORK/gold.jsonl" \
  --thinking --max-tokens 8192 --concurrency 1 \
  --out "$WORK/out.jsonl" --base-url "$BASE_URL" || { say "FAIL: runner exited nonzero"; exit 1; }

cp "$PRED" "$WORK/pred.jsonl"
python3 - "$WORK/pred.jsonl" "$WORK/out.jsonl" "$WORK/repaired.ids" <<'PY'
import json,sys,os
pred, retry, idsout = sys.argv[1], sys.argv[2], sys.argv[3]
fixed={}
if os.path.exists(retry):
    for l in open(retry):
        r=json.loads(l)
        if not r.get("error"): fixed[r["id"]]=l
lines=[]
for l in open(pred):
    r=json.loads(l)
    lines.append(fixed.get(r["id"], l) if r.get("error") else l)
open(pred,"w").writelines(lines)
open(idsout,"w").write("\n".join(sorted(fixed)))
print(f"spliced {len(fixed)} repaired row(s)")
PY

left=$(python3 -c "
import json,sys
print(sum(1 for l in open(sys.argv[1]) if json.loads(l).get('error')))" "$WORK/pred.jsonl")
if [ "${left:-1}" -ne 0 ]; then say "FAIL: $left row(s) still errored after repair; not banking"; exit 1; fi

got=$(wc -l < "$WORK/pred.jsonl"); exp=$(wc -l < "$GOLD")
[ "$got" -eq "$exp" ] || { say "FAIL: incomplete after repair, rows=$got/$exp"; exit 1; }

cp "$WORK/pred.jsonl" "$OUT_PRED"
python3 - "$OUT_PRED.repair.json" "$LABEL" "$PRED" "$WORK/repaired.ids" "$loaded" "$BASE_URL" <<'PY'
import json,sys,datetime
out,label,src,idsf,loaded,url = sys.argv[1:7]
json.dump({
 "label": label,
 "repaired_from": src,
 "repaired_ids": [i for i in open(idsf).read().split("\n") if i],
 "served_model": loaded,
 "base_url": url,
 "repaired_at_utc": datetime.datetime.utcnow().isoformat()+"Z",
 "rationale": "errored rows re-run on an identically configured single-slot server; "
              "this configuration is byte-reproducible (ARTICLE_NOTES finding 19)",
}, open(out,"w"), indent=2)
PY
say "OK: banked $OUT_PRED (rows=$got/$exp), original preserved at $PRED"
