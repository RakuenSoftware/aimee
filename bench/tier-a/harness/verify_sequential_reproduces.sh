#!/bin/bash
# THE CONTROL I SHOULD HAVE RUN FIRST.
#
# 197 of 1001 notes extracted different facts at 32 slots, and I attributed that
# to concurrency. That attribution is unsupported until the baseline is shown to
# reproduce ITSELF: if the same server, same config, same notes already disagree
# run to run, then the 197 says nothing about slots.
#
# Same model, same flags as the banked sequential arm -- -c 8192, NO -np (so the
# server's default 4 slots), client strictly sequential -- on the first N notes,
# compared byte-for-byte against what that arm produced for exactly those notes.
#
# Reproduces  -> the config is deterministic, and the 32-slot differences are
#                attributable to concurrency.
# Does NOT    -> the baseline itself is noisy, the 197 figure means nothing, and
#                every paired comparison in this benchmark needs re-examining.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
REF=${REF:?set REF to the sequential reference pred.jsonl}
N=${N:-20}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
MODEL=/opt/hf/e4b-q4.gguf
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/seq_reproduce.log"; }

head -n "$N" "$GOLD" > "$OUT/.repro_slice.jsonl"

say "restoring the ORIGINAL baseline config: -c 8192, no -np (default slots)"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
sleep 4
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $MODEL --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
ok=0
for _ in $(seq 1 60); do
  ssh -n -o ConnectTimeout=10 root@"$HOST" \
    "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
  sleep 10
done
[ "$ok" = 1 ] || { say "server never healthy"; exit 1; }
ssh -n -o ConnectTimeout=15 root@"$HOST" \
  "curl -sf http://$RIP:$PORT/props" 2>/dev/null | python3 -c "
import json,sys; print('  slots:', json.load(sys.stdin).get('total_slots'))" | tee -a "$OUT/seq_reproduce.log"

# TWICE, so run-to-run variation is visible even if both differ from the banked
# arm (which would point at the server restart rather than at decoding).
for pass in 1 2; do
  say "pass $pass: $N notes, strictly sequential"
  python3 harness/run_llamacpp.py --model "E4B.repro$pass" --gold "$OUT/.repro_slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$OUT/repro$pass.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
done

python3 - "$REF" "$OUT/repro1.pred.jsonl" "$OUT/repro2.pred.jsonl" <<'PY' | tee -a "$OUT/seq_reproduce.log"
import json,sys
def load(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
ref,r1,r2 = load(sys.argv[1]), load(sys.argv[2]), load(sys.argv[3])
ids=[i for i in r1 if i in ref]
def cmp(a,b,la,lb):
    same=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
    print(f"  {la} vs {lb}: identical raw {same}/{len(ids)}")
    return same
print(f"\ncomparing {len(ids)} notes")
a=cmp(ref,r1,"banked sequential arm","fresh pass 1")
b=cmp(ref,r2,"banked sequential arm","fresh pass 2")
c=cmp(r1,r2,"fresh pass 1        ","fresh pass 2")
print()
if a==len(ids) and b==len(ids):
    print("CONTROL PASSES: the sequential config reproduces the banked arm exactly.")
    print("The 32-slot differences are therefore attributable to concurrency.")
elif c==len(ids):
    print("Fresh passes agree with EACH OTHER but not with the banked arm.")
    print("Decoding is deterministic; something changed between then and now")
    print("(server restart, build, or model state). The 32-slot comparison is")
    print("confounded and must be re-run against a fresh sequential reference.")
else:
    print("CONTROL FAILS: the same config does not even reproduce itself.")
    print("The 197-note figure says nothing about concurrency, and every paired")
    print("comparison taken on this harness needs re-examining.")
PY
rm -f "$OUT/.repro_slice.jsonl"
say "=== CONTROL DONE ==="
