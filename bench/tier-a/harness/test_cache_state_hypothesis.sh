#!/bin/bash
# Is the run-to-run drift caused by SERVER STATE rather than by decoding?
#
# The control produced a pattern that random nondeterminism does not explain:
#   banked arm vs fresh pass 1 : 20/20 identical
#   banked arm vs fresh pass 2 : 14/20
#   pass 1      vs      pass 2 : 14/20
#
# Pass 1 ran against a just-started server with empty slots, like the banked arm
# did. Pass 2 ran against slots still holding KV from pass 1. llama.cpp reuses a
# cached prompt prefix per slot, and every request here shares the same ~600
# token system prompt, so whether a request recomputes its prefix or reuses
# cached KV depends on what ran before it -- and those paths do not produce
# bit-identical logits.
#
# Prediction, stated before the test: pass 3 on a FRESHLY RESTARTED server will
# match pass 1 and the banked arm exactly, and pass 4 (no restart) will diverge
# again. If instead pass 3 also diverges, the cause is not cache state and the
# decoding itself is nondeterministic -- which is the worse answer.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-20}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
MODEL=/opt/hf/e4b-q4.gguf
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/cache_state.log"; }

head -n "$N" "$GOLD" > "$OUT/.cs_slice.jsonl"

start_fresh() {
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $MODEL --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 60); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 10
  done
  return 1
}

say "pass 3: FRESH server restart, then 20 notes"
start_fresh || { say "server never healthy"; exit 1; }
python3 harness/run_llamacpp.py --model E4B.pass3 --gold "$OUT/.cs_slice.jsonl" \
  --thinking --max-tokens 8192 --concurrency 1 \
  --out "$OUT/repro3.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1

say "pass 4: SAME server, no restart, same 20 notes again"
python3 harness/run_llamacpp.py --model E4B.pass4 --gold "$OUT/.cs_slice.jsonl" \
  --thinking --max-tokens 8192 --concurrency 1 \
  --out "$OUT/repro4.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1

python3 - "$OUT" <<'PY' | tee -a "$OUT/cache_state.log"
import json,sys,os
o=sys.argv[1]
def load(p): return {json.loads(l)["id"]: json.loads(l) for l in open(os.path.join(o,p))}
ref=load("E4B.UD-Q4_K_XL.v8.pred.jsonl")
p1,p2,p3,p4=load("repro1.pred.jsonl"),load("repro2.pred.jsonl"),load("repro3.pred.jsonl"),load("repro4.pred.jsonl")
ids=[i for i in p3 if i in ref]
def c(a,b,la,lb):
    s=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
    print(f"  {la:34s} vs {lb:22s} {s}/{len(ids)}")
    return s
print(f"\n{len(ids)} notes\n")
print("FRESH-START runs (should all agree if the cause is cache state):")
a=c(ref,p1,"banked arm (fresh start)","pass 1 (fresh)")
b=c(ref,p3,"banked arm (fresh start)","pass 3 (fresh)")
d=c(p1,p3,"pass 1 (fresh)","pass 3 (fresh)")
print("\nWARM runs (ran against a server that had already served):")
e=c(ref,p2,"banked arm (fresh start)","pass 2 (warm)")
f=c(p3,p4,"pass 3 (fresh)","pass 4 (warm)")
print()
if a==b==d==len(ids) and (e<len(ids) or f<len(ids)):
    print("CONFIRMED: fresh-start runs are bit-identical; warm runs drift.")
    print("The cause is SERVER CACHE STATE, not decoding and not concurrency.")
    print("Every arm must start from a freshly restarted server -- which the")
    print("sweep drivers already do, one restart per arm. The 32-slot comparison")
    print("is still confounded: it changed BOTH concurrency and warmth.")
elif a==b==d==len(ids):
    print("Fresh-start runs agree and warm runs also agree: cache state is not")
    print("the cause, and the earlier pass-2 divergence needs another explanation.")
else:
    print("NOT confirmed: fresh-start runs disagree with each other. Decoding")
    print("itself is nondeterministic on this stack, and no paired comparison")
    print("on this harness can be trusted without repeat measurements.")
PY
rm -f "$OUT/.cs_slice.jsonl"
say "=== CACHE STATE TEST DONE ==="
