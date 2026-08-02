#!/bin/bash
# Are two MTP runs identical to EACH OTHER?
#
# MTP is 1.83x and differs from sequential on 26 of 100 notes, because
# verification feeds several tokens per forward pass and that changes the target's
# batch shape -- the same root cause as parallel slots.
#
# That does not settle whether it is usable. A quant ladder compares arms to each
# other, so what it needs is IDENTICAL CONDITIONS ACROSS ARMS, not identity with
# a sequential baseline. If two fresh MTP runs are bit-identical then the ladder
# can take the 1.83x and simply not be compared to the sequential arms. If they
# are not, MTP is unusable at any speed, because two arms would disagree for
# reasons unrelated to their quant.
#
# Both runs use a FRESHLY restarted server: this harness established that a warm
# server drifts (6 of 20 notes, reproducibly) regardless of anything else.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:?set OUT}
GOLD=${GOLD:?set GOLD}
N=${N:-100}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8116
REPO=${REPO:-unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL}
DRAFT=${DRAFT:-unsloth/gemma-4-E4B-it-GGUF}
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/mtp_selfconsistent.log"; }

head -n "$N" "$GOLD" > "$OUT/.sc_slice.jsonl"

run_once() {  # $1 = output suffix
  ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- pkill -f llama-server" >/dev/null 2>&1 || true
  sleep 5
  ssh -n -o ConnectTimeout=25 root@"$HOST" \
    "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && break
    sleep 10
  done
  local spec
  spec=$(ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin); print((d[0] if d else {}).get('speculative'))" 2>/dev/null)
  say "  run $1: speculative=$spec"
  python3 harness/run_llamacpp.py --model "E4B.mtp$1" --gold "$OUT/.sc_slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$OUT/mtp_sc${TAG:-e4b}$1.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
}

say "two MTP runs, fresh server each time"
run_once 1
run_once 2

python3 - "$OUT/mtp_sc${TAG:-e4b}1.pred.jsonl" "$OUT/mtp_sc${TAG:-e4b}2.pred.jsonl" <<'PY' | tee -a "$OUT/mtp_selfconsistent.log"
import json,sys
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
a,b = L(sys.argv[1]), L(sys.argv[2])
ids=[i for i in a if i in b]
s=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
print(f"\nMTP run 1 vs MTP run 2 (both fresh): {s}/{len(ids)} identical\n")
if s==len(ids):
    print("SELF-CONSISTENT. A ladder run entirely under MTP is internally")
    print("comparable, so the 1.83x is usable -- those arms simply cannot be")
    print("compared against the sequential arms already banked.")
else:
    print(f"NOT self-consistent: {len(ids)-s} note(s) differ between two runs of the")
    print("SAME configuration. MTP is unusable for this benchmark at any speed --")
    print("two arms would disagree for reasons that have nothing to do with quant.")
PY
rm -f "$OUT/.sc_slice.jsonl"
say "=== DONE ==="
