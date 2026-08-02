#!/bin/bash
# Find the highest concurrency that does NOT change the model's answers.
#
# 32 slots gave 4.54x wall-clock and changed the extracted facts on 197 of 1001
# notes. That is unusable for a quant comparison whose expected effect is ~0.01
# F1: the noise is an order of magnitude larger than the signal.
#
# But the perturbation should scale with how much the batch composition varies,
# so the interesting question is not "parallel or sequential" -- it is where the
# line sits. If 4 slots is byte-identical and 2x faster, that is most of the win
# for none of the cost.
#
# Each config is scored against the SEQUENTIAL 1001-note E4B Q4 arm already
# banked, on the same first N notes, so the reference is real data rather than a
# fresh sequential run that would itself need trusting.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
REF=${REF:?set REF to the sequential reference pred.jsonl}
N=${N:-200}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
MODEL=/opt/hf/e4b-q4.gguf
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/safe_concurrency.log"; }

# The first N notes of the gold, so the slice matches the reference exactly.
head -n "$N" "$GOLD" > "$OUT/.slice.jsonl"

for NP in 2 4 8 16; do
  say "=== serving with -np $NP"
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  # Context is sized per slot rather than shared, matching how the arms would run.
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $MODEL --host 0.0.0.0 --port $PORT -c $((NP*3072)) -np $NP --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  ok=0
  for _ in $(seq 1 60); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 10
  done
  [ "$ok" = 1 ] || { say "  -np $NP: server never healthy"; continue; }

  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "E4B.np$NP" --gold "$OUT/.slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency "$NP" \
    --out "$OUT/conc$NP.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
  t1=$(date +%s)
  say "  -np $NP: $N notes in $((t1-t0))s"

  python3 - "$REF" "$OUT/conc$NP.pred.jsonl" "$NP" "$((t1-t0))" <<'PY' | tee -a "$OUT/safe_concurrency.log"
import json,sys
ref,new,np_,secs=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4])
A={json.loads(l)["id"]:json.loads(l) for l in open(ref)}
B={json.loads(l)["id"]:json.loads(l) for l in open(new)}
ids=[i for i in B if i in A]
same=sum(1 for i in ids if A[i]["raw"]==B[i]["raw"])
def tr(r): return {(str(f.get("subject","")).strip().lower(),str(f.get("relation","")).strip().lower(),
                    str(f.get("object","")).strip().lower()) for f in (r.get("pred_nofloor") or [])}
tsame=sum(1 for i in ids if tr(A[i])==tr(B[i]))
seq_s=sum(A[i]["latency_ms"] for i in ids)/1000
print(f"    identical raw {same}/{len(ids)} | identical facts {tsame}/{len(ids)} "
      f"| speedup {seq_s/max(secs,1):.2f}x")
PY
done
rm -f "$OUT/.slice.jsonl"
say "=== CONCURRENCY SWEEP DONE ==="
