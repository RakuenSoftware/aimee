#!/bin/bash
# Do two SHARDED runs agree with EACH OTHER?
#
# Sharding differs from a single process on 89-96/120 notes. That is the wrong
# bar. The same was true of MTP itself (74/100 against sequential) and MTP is
# still usable, because it is self-consistent. What disqualifies a configuration
# is disagreeing with ITSELF -- then two arms differ for reasons unrelated to
# their quant.
#
# Also runs 4 shards WITHOUT MTP, to locate the perturbation. The isolation test
# (plain, 2 processes, no MTP) was 60/60 identical, and the only difference here
# is MTP, so the likely culprit is speculation under contention: draft length
# adapts to timing, which changes the verification batch shape.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}; OUT=${OUT:?set OUT}; N=${N:-120}
HOST=admin@192.168.1.254
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
MODEL=/mnt/media/storage/models/gguf/gemma-4-E4B-it-UD-Q4_K_XL.gguf
REPO=unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
DRAFT=unsloth/gemma-4-E4B-it-GGUF
BASE=8400; NPROC=4
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/shard_repeat.log"; }

head -n "$N" "$GOLD" > "$OUT/.sr_gold.jsonl"
kill_all() { ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f llama-server" >/dev/null 2>&1 || true; sleep 5; }

start_one() {  # $1 port  $2 use_mtp
  local spec="" m="-m $MODEL"
  if [ "$2" = 1 ]; then spec="-hfd $DRAFT"; m="-hf $REPO"; fi
  ssh -n -o ConnectTimeout=25 "$HOST" \
    "HF_HOME=/mnt/media/tierbench/hf nohup setsid $BIN $m $spec --host 0.0.0.0 --port $1 -c 8192 -np 1 --device Vulkan1 --no-webui --no-mmproj -ngl 99 >/tmp/sr-$1.log 2>&1 </dev/null &" >/dev/null 2>&1
  for _ in $(seq 1 160); do
    ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$1/health" >/dev/null 2>&1 && break
    sleep 15
  done
  pkill -f "ssh -N -L $1:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -L "$1:127.0.0.1:$1" "$HOST" >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$1/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

run_shards() {  # $1 tag  $2 use_mtp
  kill_all
  for i in $(seq 0 $((NPROC-1))); do
    start_one $((BASE+i)) "$2" || { say "  $1: server $i failed"; return 1; }
  done
  rm -rf "$OUT/.sr"; mkdir -p "$OUT/.sr"
  python3 - "$OUT/.sr_gold.jsonl" "$OUT/.sr" "$NPROC" <<'PY'
import sys
g,b,n = sys.argv[1], sys.argv[2], int(sys.argv[3])
f=[open(f"{b}/s{i}.jsonl","w") for i in range(n)]
for i,l in enumerate(open(g)): f[i%n].write(l)
[x.close() for x in f]
PY
  local t0 t1 pids=()
  t0=$(date +%s)
  for i in $(seq 0 $((NPROC-1))); do
    python3 harness/run_llamacpp.py --model sr --gold "$OUT/.sr/s$i.jsonl" --thinking \
      --max-tokens 8192 --concurrency 1 --out "$OUT/.sr/o$i.jsonl" \
      --base-url "http://127.0.0.1:$((BASE+i))" >/dev/null 2>&1 &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p"; done
  t1=$(date +%s)
  python3 - "$OUT/.sr_gold.jsonl" "$OUT/.sr" "$NPROC" "$OUT/sr_$1.pred.jsonl" <<'PY'
import sys,json
g,b,n,o = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
r={}
for i in range(n):
    for l in open(f"{b}/o{i}.jsonl"): r[json.loads(l)["id"]]=l
with open(o,"w") as fh:
    for l in open(g):
        gid=json.loads(l)["id"]
        if gid in r: fh.write(r[gid])
PY
  say "  $1: $((t1-t0))s"
}

run_shards mtp_a 1
run_shards mtp_b 1
run_shards plain_a 0
run_shards plain_b 0
kill_all

python3 - "$OUT" <<'PY' | tee -a "$OUT/shard_repeat.log"
import json,sys,os
o=sys.argv[1]
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(os.path.join(o,p))}
def cmp(a,b):
    A,B=L(a),L(b); ids=[i for i in A if i in B]
    return sum(1 for i in ids if A[i]["raw"]==B[i]["raw"]), len(ids)
m=cmp("sr_mtp_a.pred.jsonl","sr_mtp_b.pred.jsonl")
p=cmp("sr_plain_a.pred.jsonl","sr_plain_b.pred.jsonl")
print("\n4 shards, run vs run (the bar that matters)\n")
print(f"  with MTP    : {m[0]}/{m[1]}")
print(f"  without MTP : {p[0]}/{p[1]}")
print()
if p[0]==p[1] and m[0]<m[1]:
    print("Plain sharding is REPEATABLE; MTP under contention is not.")
    print("=> shard WITHOUT MTP, or keep MTP and stay single-process.")
elif m[0]==m[1] and p[0]==p[1]:
    print("Both repeatable. Sharding is usable as its own configuration, and MTP")
    print("composes with it.")
elif m[0]==m[1]:
    print("MTP sharding repeatable but plain is not -- unexpected; re-check.")
else:
    print("NEITHER repeatable under sharding. Multi-process is unusable for this")
    print("benchmark, and today's single-process arms stand.")
PY
rm -rf "$OUT/.sr" "$OUT/.sr_gold.jsonl"
say "=== SHARD REPEATABILITY DONE ==="
