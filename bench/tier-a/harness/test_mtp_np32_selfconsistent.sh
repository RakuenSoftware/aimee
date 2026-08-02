#!/bin/bash
# MTP + 32 slots together: is it repeatable?
#
# Repeatability is the requirement, not identity with a sequential run. Each of
# these configurations perturbs outputs relative to sequential -- MTP 26 of 100,
# 32 slots ~20% -- but a ladder only needs every arm measured under the SAME
# conditions. What would disqualify a configuration is disagreeing with ITSELF,
# because then two arms differ for reasons unrelated to their quant.
#
# Two open questions this answers:
#   1. does speculation survive -np 32? With -md it silently disabled at 32
#      slots; the -hfd path enabled it at the default 4, and 32 is untested.
#   2. is the combination deterministic run to run, on a fresh server each time?
#
# Concurrency is the client-side in-flight count and matches the server's slot
# count, so the batch composition is as variable as it will ever be -- the worst
# case for repeatability, which is the right thing to test.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:?set OUT}
GOLD=${GOLD:?set GOLD}
N=${N:-100}
SLOTS=${SLOTS:-32}
CTX=${CTX:-98304}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8116
REPO=${REPO:-unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL}
DRAFT=${DRAFT:-unsloth/gemma-4-E4B-it-GGUF}
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/mtp_np32.log"; }

head -n "$N" "$GOLD" > "$OUT/.np32_slice.jsonl"

run_once() {  # $1 = suffix
  ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- pkill -f llama-server" >/dev/null 2>&1 || true
  sleep 5
  ssh -n -o ConnectTimeout=25 root@"$HOST" \
    "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS --no-webui --no-mmproj -ngl 99 > /opt/tierA/np32-mtp-$1.log 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && break
    sleep 10
  done
  # Confirm BOTH that speculation survived -np 32 and which model is loaded --
  # --model is only a label, and a stale server would give a meaningless pass.
  ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/props" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin)
print('    model :', (d.get('model_path') or '').split('/')[-1])
print('    slots :', d.get('total_slots'))" 2>/dev/null | tee -a "$OUT/mtp_np32.log"
  ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin); s=d[0] if d else {}
print('    speculative:', s.get('speculative'), '| n_ctx/slot:', s.get('n_ctx'))" 2>/dev/null | tee -a "$OUT/mtp_np32.log"

  local t0 t1
  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "E4B.np32mtp$1" --gold "$OUT/.np32_slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency "$SLOTS" \
    --out "$OUT/np32mtp$1.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
  t1=$(date +%s)
  say "  run $1: $N notes in $((t1-t0))s"
  echo "$((t1-t0))" >> "$OUT/.np32_times"
}

rm -f "$OUT/.np32_times"
say "MTP + $SLOTS slots, two fresh runs"
run_once 1
run_once 2

python3 - "$OUT/np32mtp1.pred.jsonl" "$OUT/np32mtp2.pred.jsonl" "$OUT/.np32_times" \
         "$OUT/E4B.UD-Q4_K_XL.v8.pred.jsonl" <<'PY' | tee -a "$OUT/mtp_np32.log"
import json,sys,statistics
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
a,b = L(sys.argv[1]), L(sys.argv[2])
times=[int(x) for x in open(sys.argv[3]) if x.strip()]
ref=L(sys.argv[4])
ids=[i for i in a if i in b]
s=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
def tr(r): return {(str(f.get("subject","")).strip().lower(),str(f.get("relation","")).strip().lower(),
                    str(f.get("object","")).strip().lower()) for f in (r.get("pred_nofloor") or [])}
ts=sum(1 for i in ids if tr(a[i])==tr(b[i]))
rs=sum(1 for i in ids if i in ref and a[i]["raw"]==ref[i]["raw"])
seq=sum(ref[i]["latency_ms"] for i in ids if i in ref)/1000
print(f"\n{len(ids)} notes, MTP + 32 slots\n")
print(f"  run 1 vs run 2, raw completions : {s}/{len(ids)}")
print(f"  run 1 vs run 2, extracted facts : {ts}/{len(ids)}")
print(f"  run 1 vs banked SEQUENTIAL arm  : {rs}/{len(ids)}  (expected to differ)")
if times:
    print(f"\n  wall: {times} s   sequential equivalent {seq:.0f} s"
          f"   speedup {seq/max(min(times),1):.2f}x")
print()
if s==len(ids):
    print("REPEATABLE. Every arm run under this configuration is comparable to")
    print("every other, which is what the ladder needs. Not comparable to the")
    print("sequential arms -- that is expected and acceptable.")
elif ts==len(ids):
    print(f"Raw text differs on {len(ids)-s} note(s) but every extracted triple")
    print("matches, so scores would be identical. Usable, with the caveat recorded.")
else:
    print(f"NOT REPEATABLE: {len(ids)-ts} note(s) extract different facts between two")
    print("runs of the SAME configuration. Unusable at any speed -- two arms would")
    print("disagree for reasons that have nothing to do with their quant.")
PY
rm -f "$OUT/.np32_slice.jsonl" "$OUT/.np32_times"
say "=== DONE ==="
