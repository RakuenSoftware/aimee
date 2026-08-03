#!/bin/bash
# Run one arm across N ISOLATED single-slot servers on one card.
#
# This is the only parallelism that keeps results repeatable. Measured:
#   32 slots in one process : 44/60 identical between two runs   -- unusable
#   2 isolated processes    : 60/60 identical, even with the card saturated
# The difference is that slots batch requests into a SHARED forward pass, so a
# sequence's logits depend on who else is in the batch, while separate processes
# have separate contexts and never share a GEMM. Contention changes timing, not
# arithmetic.
#
# So: N servers, each -np 1, each with its own MTP draft head, and the corpus
# split N ways. Every note is still answered by a single-slot server exactly as
# it would be alone, which is why the merged output is byte-identical to a
# sequential run of the same arm.
#
# Shards are round-robin by line, not contiguous blocks: the corpus is ordered by
# domain and category, so contiguous blocks would give one shard all the negation
# notes and another all the infra notes, and the shards would finish at wildly
# different times.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
LABEL=${LABEL:?set LABEL}
REPO=${REPO:?set REPO}          # hf repo:quant
DRAFT=${DRAFT:?set DRAFT}       # hf draft repo (same repo) for MTP
CARD=${CARD:?set CARD to 5080 or xtx}
NPROC=${NPROC:-0}               # 0 = auto-size from measured VRAM
BASE_PORT=${BASE_PORT:-8200}
RESERVE_MIB=${RESERVE_MIB:-1800}  # headroom for fragmentation + the draft ctx

case "$CARD" in
  5080) HOST=root@192.168.1.253; RUN="pct exec 140 -- bash -lc"; EP=192.168.0.5
        BIN=/opt/llama.cpp/build-cuda/bin/llama-server; HFH=/opt/hf; DEV=""
        TOTAL=$(ssh -n -o ConnectTimeout=15 $HOST "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits" 2>/dev/null) ;;
  xtx)  HOST=admin@192.168.1.254; RUN="bash -lc"; EP=127.0.0.1
        BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server; HFH=/mnt/media/tierbench/hf
        DEV="--device Vulkan1"   # Vulkan0 is a 16GB iGPU; defect 30
        TOTAL=24560 ;;
  *) echo "CARD must be 5080 or xtx"; exit 1 ;;
esac
# Shard scratch is per LABEL. It used to be a single $OUT/.shards shared by every
# run, so two cards working at once wrote into the same out0/out1/out2 files and
# the merge -- which keys by note id -- silently produced a MIXTURE of E2B and
# E4B predictions scored as one model. Measured mid-run: out0 held 615 E2B rows
# and 682 E4B rows.
SHARD_DIR="$OUT/.shards-$LABEL"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/shard_$LABEL.log"; }

kill_servers() {
  # Kill by LISTENING SOCKET, never by command line.
  #
  # Two bugs live here, both already paid for.
  #
  # `pkill -f llama-server` kills every server in the container. CT 140 is
  # shared; that killed another session's work all night and ours in return.
  #
  # The fix for that, `pkill -f "port $p "`, is ALSO broken: the shell running
  # the pkill has "port 8400 " in its own command line, so pkill matches itself
  # and the kill sequence dies before reaching the server. Measured: a stale
  # E4B server survived on 8400, a new E2B server failed to bind, the health
  # check passed against the SURVIVOR, and shard 0 of an "E2B" arm was answered
  # by E4B. The merge check did not catch it because it compares the --model
  # LABEL, which every shard shares, not the model actually loaded.
  #
  # ss resolves the pid from the socket. Nothing matches on text.
  #
  # Sent over stdin as a heredoc rather than as a quoted argument. The script
  # contains both single and double quotes, and nesting them through
  # ssh -> pct exec -> bash -c silently mangles the result; a heredoc has no
  # quoting to get wrong. (Note: no -n on ssh, stdin is the script.)
  local lo=$BASE_PORT hi=$((BASE_PORT+7)) remote
  if [ "$CARD" = 5080 ]; then remote="pct exec 140 -- bash -s"; else remote="bash -s"; fi
  ssh -o ConnectTimeout=20 "$HOST" "$remote" >/dev/null 2>&1 <<EOF || true
for p in \$(seq $lo $hi); do
  pid=\$(ss -ltnpH "sport = :\$p" 2>/dev/null | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
  [ -n "\$pid" ] && kill -9 "\$pid" 2>/dev/null
done
true
EOF
  sleep 5
}

verify_model() {  # $1 = port. Confirm the server there loaded the quant we asked for.
  # A health check proves something is listening, not that it is the right
  # model. A stale server on a port we failed to claim answers /health happily
  # and silently substitutes its own weights for an entire arm.
  local p=$1 want_fam want_q loaded
  want_fam=$(echo "$REPO" | grep -oE 'E[24]B')
  want_q=$(echo "$REPO"   | grep -oE 'UD-Q[0-9]_K_XL')
  loaded=$(ssh -n -o ConnectTimeout=15 $HOST "curl -sf --max-time 8 http://$EP:$p/props" 2>/dev/null \
           | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])" 2>/dev/null)
  if [ -z "$loaded" ]; then say "  FAIL: port $p served no /props"; return 1; fi
  case "$loaded" in
    *"$want_fam"*"$want_q"*) return 0 ;;
    *) say "  FAIL: port $p loaded '$loaded', expected $want_fam / $want_q"; return 1 ;;
  esac
}

start_one() {  # $1 = port
  local p=$1
  if [ "$CARD" = 5080 ]; then
    ssh -n -o ConnectTimeout=25 $HOST \
      "pct exec 140 -- bash -lc 'HF_HOME=$HFH nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $p -c 8192 -np 1 --no-webui --no-mmproj -ngl 99 >/opt/tierA/shard-$LABEL-$p.log 2>&1 </dev/null &'" >/dev/null 2>&1
  else
    ssh -n -o ConnectTimeout=25 $HOST \
      "HF_HOME=$HFH nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $p -c 8192 -np 1 $DEV --no-webui --no-mmproj -ngl 99 >/tmp/shard-$LABEL-$p.log 2>&1 </dev/null &" >/dev/null 2>&1
  fi
  # Health must be checked at the address the server actually listens on. On the
  # 5080 the server runs INSIDE CT 140 (192.168.0.5); curling 127.0.0.1 on the
  # PVE host finds nothing, waits out the loop, and reports a healthy server as
  # dead. That cost three E2B arms and two hours of idle GPU.
  local hc="$EP"
  for _ in $(seq 1 160); do
    ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$hc:$p/health" >/dev/null 2>&1 && return 0
    sleep 15
  done
  return 1
}

tunnel_one() {  # xtx only: serving ports are firewalled off-host
  [ "$CARD" = 5080 ] && return 0
  pkill -f "ssh -N -L $1:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -L "$1:127.0.0.1:$1" "$HOST" >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$1/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

vram_used() {
  if [ "$CARD" = 5080 ]; then
    ssh -n -o ConnectTimeout=15 $HOST "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits" 2>/dev/null
  else
    # There is NO VRAM probe on this host. rocm-smi is not installed -- the card
    # is driven through Vulkan, not ROCm -- so this has returned empty for every
    # XTX arm this session ("one instance uses  MiB of 24560 MiB" in each log)
    # and auto-sizing has never once worked here. 2>/dev/null hid it.
    #
    # Pass NPROC explicitly for CARD=xtx. The fallback below picks 2, which is
    # not a measurement and is not comparable to an arm that measured its own.
    ssh -n -o ConnectTimeout=15 $HOST "command -v rocm-smi >/dev/null 2>&1 && rocm-smi --showmeminfo vram 2>/dev/null | grep -i 'used' | grep -oE '[0-9]{6,}' | head -1" 2>/dev/null \
      | awk '{printf "%d", $1/1048576}'
  fi
}

kill_servers
say "sizing: starting one server to measure resident VRAM"
start_one "$BASE_PORT" || { say "FAIL: first server never healthy"; exit 1; }
tunnel_one "$BASE_PORT" || { say "FAIL: tunnel"; exit 1; }
verify_model "$BASE_PORT" || { say "FAIL $LABEL: wrong model on $BASE_PORT"; exit 1; }
PER=$(vram_used)
say "  one instance uses ${PER} MiB of ${TOTAL} MiB"

if [ "$NPROC" = 0 ]; then
  if [ -n "$PER" ] && [ "$PER" -gt 0 ] 2>/dev/null; then
    NPROC=$(( (TOTAL - RESERVE_MIB) / PER ))
    [ "$NPROC" -lt 1 ] && NPROC=1
    # Beyond ~6 the card is bandwidth-saturated and extra processes only add
    # contention; capped rather than discovered per-arm to keep the night simple.
    [ "$NPROC" -gt 6 ] && NPROC=6
  else
    NPROC=2
    say "  WARNING: no VRAM reading on $CARD (rocm-smi absent on the XTX host);"
    say "           defaulting to $NPROC. This arm's shard count is a GUESS."
  fi
fi
say "  -> running $NPROC processes"

for i in $(seq 1 $((NPROC-1))); do
  p=$((BASE_PORT+i))
  start_one "$p" || { say "FAIL: server on $p never healthy"; exit 1; }
  tunnel_one "$p" || { say "FAIL: tunnel $p"; exit 1; }
  verify_model "$p" || { say "FAIL $LABEL: wrong model on $p, refusing to run"; exit 1; }
done
say "  all $NPROC servers up; VRAM now $(vram_used) MiB"

# Round-robin shards so every shard sees the same category mix.
python3 - "$GOLD" "$SHARD_DIR" "$NPROC" <<'PY'
import sys, os, itertools
gold, base, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(base, exist_ok=True)
fhs=[open(f"{base}/shard{i}.jsonl","w") for i in range(n)]
for i,line in enumerate(open(gold)):
    fhs[i % n].write(line)
for f in fhs: f.close()
PY

say "START $LABEL across $NPROC shards"
t0=$(date +%s)
pids=()
for i in $(seq 0 $((NPROC-1))); do
  p=$((BASE_PORT+i))
  python3 harness/run_llamacpp.py --model "$LABEL" --gold "$SHARD_DIR/shard$i.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$SHARD_DIR/out$i.jsonl" --base-url "http://$EP:$p" >>"$OUT/shard_$LABEL.run.log" 2>&1 &
  pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
t1=$(date +%s)

# Merge back into gold order: paired scoring zips files together, so order is
# not cosmetic.
python3 - "$GOLD" "$SHARD_DIR" "$NPROC" "$OUT/$LABEL.pred.jsonl" <<'PY'
import sys, json
gold, base, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
rows={}
for i in range(n):
    for line in open(f"{base}/out{i}.jsonl"):
        r=json.loads(line); rows[r["id"]]=line
missing=0
with open(out,"w") as fh:
    for line in open(gold):
        gid=json.loads(line)["id"]
        if gid in rows: fh.write(rows[gid])
        else: missing+=1
print(f"merged {len(rows)} rows, {missing} missing")
PY

# A merged arm must contain exactly ONE model. This is the check that would have
# caught the shared-scratch bug immediately instead of after two hours.
python3 - "$OUT/$LABEL.pred.jsonl" "$LABEL" <<'PY'
import json,sys,collections,os
# Distinguish "produced nothing" from "produced a mixture": reporting a missing
# file as contamination pointed the investigation at the wrong bug.
if not os.path.exists(sys.argv[1]) or os.path.getsize(sys.argv[1]) == 0:
    sys.exit("EMPTY: no rows were produced")
c=collections.Counter(json.loads(l).get("model","?") for l in open(sys.argv[1]))
if len(c) > 1:
    sys.exit(f"CONTAMINATED: {sys.argv[1]} holds {dict(c)} -- more than one model")
if c and next(iter(c)) != sys.argv[2]:
    sys.exit(f"WRONG MODEL: {sys.argv[1]} holds {dict(c)}, expected {sys.argv[2]}")
PY
if [ $? -ne 0 ]; then say "FAIL $LABEL: merge rejected (see above), discarding"; rm -f "$OUT/$LABEL.pred.jsonl"; exit 1; fi
# Row count is not completion. An arm whose servers died mid-run still produces a
# row per note, each carrying a transport error, and this reported rows=10000/10000
# and "OK" for a file that was 97% connection failures. score.py caught it; the
# driver should not have needed rescuing.
errs=$(python3 -c "
import json
print(sum(1 for l in open('$OUT/$LABEL.pred.jsonl') if json.loads(l).get('error')))" 2>/dev/null || echo 0)
if [ "${errs:-0}" -gt 0 ]; then
  pct=$(( errs * 100 / $(wc -l < "$OUT/$LABEL.pred.jsonl") ))
  say "FAIL $LABEL: $errs errored rows (${pct}%), discarding"
  mv "$OUT/$LABEL.pred.jsonl" "$OUT/$LABEL.pred.jsonl.errored"
  exit 1
fi
got=$(wc -l < "$OUT/$LABEL.pred.jsonl")
exp=$(wc -l < "$GOLD")
say "DONE $LABEL rows=$got/$exp wall=$(( (t1-t0)/60 ))m$(( (t1-t0)%60 ))s procs=$NPROC"
rm -rf "$SHARD_DIR"
kill_servers
