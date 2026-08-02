#!/bin/bash
# Does MTP actually pay, and does it change the answers?
#
# Speculative decoding verifies every drafted token against the target model, so
# under greedy it should be OUTPUT-IDENTICAL -- unlike batching, which changes
# the numerics. If that holds here it is free throughput and every future sweep
# should use it.
#
# The server must be FRESHLY started: this harness established that greedy is
# bit-reproducible across restarts but drifts against a warm server (exactly 6 of
# 20 notes, reproducibly), because llama.cpp reuses a cached prompt prefix per
# slot. Comparing a warm MTP run against the cold banked arm would manufacture
# differences that have nothing to do with speculation.
#
# Concurrency 1 throughout, so speculation is the only variable against the
# banked sequential arm.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
REF=${REF:?set REF to the banked sequential pred.jsonl}
N=${N:-100}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8116
REPO=unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
DRAFT_REPO=unsloth/gemma-4-E4B-it-GGUF
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/mtp_speedup.log"; }

head -n "$N" "$GOLD" > "$OUT/.mtp_slice.jsonl"

serve() {  # $1 = "mtp" | "plain"
  ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- pkill -f llama-server" >/dev/null 2>&1 || true
  sleep 5
  local spec=""
  [ "$1" = "mtp" ] && spec="-hfd $DRAFT_REPO"
  ssh -n -o ConnectTimeout=25 root@"$HOST" \
    "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $REPO $spec --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 > /opt/tierA/mtp-$1.log 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 10
  done
  return 1
}

for mode in plain mtp; do
  say "=== $mode: fresh server"
  serve "$mode" || { say "  server never healthy"; continue; }
  spec=$(ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin); print((d[0] if d else {}).get('speculative'))" 2>/dev/null)
  say "  speculative=$spec"
  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "E4B.$mode" --gold "$OUT/.mtp_slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$OUT/mtp_$mode.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
  t1=$(date +%s)
  say "  $N notes in $((t1-t0))s"
  echo "$mode $((t1-t0))" >> "$OUT/.mtp_times"
done

python3 - "$REF" "$OUT/mtp_plain.pred.jsonl" "$OUT/mtp_mtp.pred.jsonl" "$OUT/.mtp_times" <<'PY' | tee -a "$OUT/mtp_speedup.log"
import json,sys
def load(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
ref,plain,mtp = load(sys.argv[1]), load(sys.argv[2]), load(sys.argv[3])
times = dict(l.split() for l in open(sys.argv[4]) if l.strip())
ids=[i for i in mtp if i in ref and i in plain]
def same(a,b): return sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
print(f"\n{len(ids)} notes\n")
print("fidelity (raw completions, byte for byte):")
print(f"   banked arm vs plain (no MTP) : {same(ref,plain)}/{len(ids)}")
print(f"   banked arm vs MTP            : {same(ref,mtp)}/{len(ids)}")
print(f"   plain      vs MTP            : {same(plain,mtp)}/{len(ids)}")
tp, tm = int(times.get("plain",0)), int(times.get("mtp",0))
print(f"\nthroughput:")
print(f"   plain {tp:4d}s   MTP {tm:4d}s   speedup {tp/max(tm,1):.2f}x")
print()
if same(plain,mtp)==len(ids) and tm < tp:
    print(f"VERDICT: MTP is byte-identical to plain decoding and {tp/max(tm,1):.2f}x faster.")
    print("Free throughput -- unlike parallel slots, which changed 197/1001 notes.")
elif same(plain,mtp)==len(ids):
    print("VERDICT: byte-identical but NOT faster. The draft acceptance rate is too")
    print("low to pay for the verification, or speculation is not actually running.")
else:
    print(f"VERDICT: MTP changed {len(ids)-same(plain,mtp)} note(s). Verification is not")
    print("exact on this path, so it is not the free speedup it should be.")
PY
rm -f "$OUT/.mtp_slice.jsonl" "$OUT/.mtp_times"
say "=== DONE ==="
