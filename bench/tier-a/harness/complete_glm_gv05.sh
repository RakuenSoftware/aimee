#!/bin/bash
# GLM-4.7-Flash is 69/70. Run the one missing note and merge it in.
#
# The gap is not a model failure: all 69 rows carry no error and no truncation.
# Note gv05 was lost because defect 29 unlinked the predictions file mid-run and
# the runner died on FileNotFoundError at exit, after the rescue loop had already
# copied 69 rows out of /proc. Re-running the whole corpus would cost another
# 2.5 hours to reproduce data that is already sound.
#
# The single note is run through the same server config as sweep_glm_cuda.sh, so
# the merged file stays one run rather than two spliced lanes.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/thinking
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8087}

LABEL=GLM-4.7-Flash
REPO=unsloth/GLM-4.7-Flash-GGUF:Q8_0
PRED="$OUT/$LABEL.pred.jsonl"
PART="$OUT/$LABEL.gv05.pred.jsonl"
GOLD1=/tmp/gold_gv05.jsonl
LOG="$OUT/$LABEL.gv05.server.log"

MISSING=$($PY - <<'EOF'
import json
gold=[json.loads(l)["id"] for l in open("data/gold.jsonl")]
have={json.loads(l)["id"] for l in open("results/thinking/GLM-4.7-Flash.pred.jsonl")}
print(" ".join(i for i in gold if i not in have))
EOF
)
if [ -z "$MISSING" ]; then echo "COMPLETE already: nothing missing"; exit 0; fi
echo "MISSING: $MISSING"

$PY - "$MISSING" <<'EOF' > "$GOLD1"
import json,sys
want=set(sys.argv[1].split())
for l in open("data/gold.jsonl"):
    if json.loads(l)["id"] in want: sys.stdout.write(l)
EOF
echo "gold subset rows: $(wc -l < "$GOLD1")"

echo "=== SERVE $LABEL (CUDA, Q8_0, experts on CPU) ==="
$SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj \
    -ngl 99 -ot ".ffn_.*_exps.=CPU" >"$LOG" 2>&1 &
SRV=$!
ready=0
for _ in $(seq 1 360); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
  kill -0 $SRV 2>/dev/null || break
  sleep 10
done
if [ "$ready" != 1 ]; then
  echo "FAIL server never healthy: $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; exit 1
fi

$PY harness/run_llamacpp.py --model "$LABEL" --gold "$GOLD1" \
    --thinking --max-tokens 8192 --out "$PART" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1
rc=$?
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; sleep 5

if [ "$rc" != 0 ] || [ ! -s "$PART" ]; then echo "FAIL runner rc=$rc"; exit 1; fi

# Merge in gold order. A row that errored or truncated is merged anyway: the
# scorer is the thing that decides validity, and hiding a bad row here would be
# the scorer-bypass this bench has already been bitten by.
$PY - <<'EOF'
import json
gold=[json.loads(l)["id"] for l in open("data/gold.jsonl")]
rows={}
for f in ("results/thinking/GLM-4.7-Flash.pred.jsonl",
          "results/thinking/GLM-4.7-Flash.gv05.pred.jsonl"):
    for l in open(f):
        r=json.loads(l); rows[r["id"]]=r
out=[rows[i] for i in gold if i in rows]
with open("results/thinking/GLM-4.7-Flash.pred.jsonl","w") as fh:
    for r in out: fh.write(json.dumps(r)+"\n")
print("merged rows:", len(out))
bad=[r["id"] for r in out if r.get("error") or r.get("truncated")]
print("rows with error/truncated:", bad or "none")
EOF

$PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
    --json-out "$OUT/$LABEL.score.json" >/dev/null 2>&1
if [ -s "$OUT/$LABEL.score.json" ]; then echo "OK   $LABEL scored"; else echo "FAIL scorer refused"; fi
date -u +%FT%TZ > /opt/GLM_GV05_DONE
echo GLM_GV05_CHAIN_DONE
