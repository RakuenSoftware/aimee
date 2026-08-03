#!/bin/bash
# Prove sharding is free before committing a night to it.
#
# Two things must hold, and only one of them has been shown:
#
#   REPEATABLE  -- two isolated processes are 60/60 identical even with the card
#                  saturated. Established.
#   EQUIVALENT  -- the MERGED output of N shards is identical to a single-process
#                  run of the same notes. NOT established: the isolation test ran
#                  the same notes on one server while another was merely busy; it
#                  never checked that splitting a corpus and rejoining it gives
#                  the same answers.
#
# If equivalence holds, the overnight sharded runs are directly comparable to
# every single-process arm measured today, and the speedup is free. If it does
# not, sharded arms are a third configuration and can only be compared to each
# other.
#
# Also measures the actual scaling, which is the number that decides how many
# processes to run: 1, 2, 3, 4 shards over the same notes.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-120}
HOST=admin@192.168.1.254
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
REPO=unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
DRAFT=unsloth/gemma-4-E4B-it-GGUF
BASE=8300
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/shard_validate.log"; }

head -n "$N" "$GOLD" > "$OUT/.sv_gold.jsonl"

kill_all() { ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f llama-server" >/dev/null 2>&1 || true; sleep 5; }

start_one() {
  ssh -n -o ConnectTimeout=25 "$HOST" \
    "HF_HOME=/mnt/media/tierbench/hf nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $1 -c 8192 -np 1 --device Vulkan1 --no-webui --no-mmproj -ngl 99 >/tmp/sv-$1.log 2>&1 </dev/null &" >/dev/null 2>&1
  for _ in $(seq 1 160); do
    ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$1/health" >/dev/null 2>&1 && break
    sleep 15
  done
  pkill -f "ssh -N -L $1:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -L "$1:127.0.0.1:$1" "$HOST" >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$1/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

run_sharded() {  # $1 = nproc
  local n=$1
  kill_all
  for i in $(seq 0 $((n-1))); do start_one $((BASE+i)) || { say "  n=$n: server $i failed"; return 1; }; done
  rm -rf "$OUT/.sv"; mkdir -p "$OUT/.sv"
  python3 - "$OUT/.sv_gold.jsonl" "$OUT/.sv" "$n" <<'PY'
import sys
gold, base, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
fhs=[open(f"{base}/s{i}.jsonl","w") for i in range(n)]
for i,l in enumerate(open(gold)): fhs[i%n].write(l)
for f in fhs: f.close()
PY
  local t0 t1 pids=()
  t0=$(date +%s)
  for i in $(seq 0 $((n-1))); do
    python3 harness/run_llamacpp.py --model "sv$n" --gold "$OUT/.sv/s$i.jsonl" \
      --thinking --max-tokens 8192 --concurrency 1 \
      --out "$OUT/.sv/o$i.jsonl" --base-url "http://127.0.0.1:$((BASE+i))" >/dev/null 2>&1 &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p"; done
  t1=$(date +%s)
  python3 - "$OUT/.sv_gold.jsonl" "$OUT/.sv" "$n" "$OUT/sv_n$n.pred.jsonl" <<'PY'
import sys, json
gold, base, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
rows={}
for i in range(n):
    for line in open(f"{base}/o{i}.jsonl"):
        rows[json.loads(line)["id"]]=line
with open(out,"w") as fh:
    for line in open(gold):
        gid=json.loads(line)["id"]
        if gid in rows: fh.write(rows[gid])
PY
  echo "$n $((t1-t0))" >> "$OUT/.sv_times"
  say "  n=$n: $((t1-t0))s"
}

rm -f "$OUT/.sv_times"
for n in 1 2 3 4; do run_sharded $n; done
kill_all

python3 - "$OUT" <<'PY' | tee -a "$OUT/shard_validate.log"
import json,sys,os
o=sys.argv[1]
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(os.path.join(o,p))}
ref=L("sv_n1.pred.jsonl")
times=dict(l.split() for l in open(os.path.join(o,".sv_times")))
print("\nEQUIVALENCE: merged N-shard output vs single process\n")
base=int(times["1"])
print(f"  {'shards':>6s} {'identical':>12s} {'wall':>7s} {'speedup':>8s}")
for n in ("1","2","3","4"):
    f=f"sv_n{n}.pred.jsonl"
    if not os.path.exists(os.path.join(o,f)): continue
    cur=L(f); ids=[i for i in ref if i in cur]
    same=sum(1 for i in ids if ref[i]["raw"]==cur[i]["raw"])
    t=int(times[n])
    print(f"  {n:>6s} {f'{same}/{len(ids)}':>12s} {t:6d}s {base/t:7.2f}x")
allsame=all(
    sum(1 for i in ref if i in L(f'sv_n{n}.pred.jsonl') and ref[i]["raw"]==L(f'sv_n{n}.pred.jsonl')[i]["raw"])==len(ref)
    for n in ("2","3","4") if os.path.exists(os.path.join(o,f"sv_n{n}.pred.jsonl")))
print()
print("SHARDING IS FREE: merged output is byte-identical to a single process, so\n"
      "sharded arms are directly comparable to every arm measured today."
      if allsame else
      "SHARDING CHANGES OUTPUT: sharded arms are a separate configuration and can\n"
      "only be compared to each other, not to today's single-process arms.")
PY
rm -rf "$OUT/.sv" "$OUT/.sv_gold.jsonl" "$OUT/.sv_times"
say "=== VALIDATION DONE ==="
