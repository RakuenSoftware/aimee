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
# hf draft repo (same repo) for MTP. Set DRAFT="" to run WITHOUT speculation.
#
# That is not a performance switch, it is a comparability one. MTP drafts exist
# only for gemma-4 in this model field -- granite-4.0-1b, granite-4.1-3b,
# gemma-3n-E4B and Qwen3-1.7B publish no mtp-*.gguf at all. Running the gemma
# arms with speculation and the rest without would make the draft head a variable
# ACROSS THE MODELS BEING RANKED, and MTP moves 26 of 100 notes relative to a
# sequential run. Any cross-model sweep therefore runs with DRAFT="" for all
# models or MTP for all models; it cannot mix.
DRAFT=${DRAFT-}
if [ -n "$DRAFT" ]; then DRAFT_ARG="-hfd $DRAFT"; else DRAFT_ARG=""; fi
CARD=${CARD:?set CARD to 5080 or xtx}
NPROC=${NPROC:-0}               # 0 = auto-size from measured VRAM
BASE_PORT=${BASE_PORT:-8200}
RESERVE_MIB=${RESERVE_MIB:-1800}  # headroom for fragmentation + the draft ctx
# llama-server keeps a HOST-side prompt cache of KV snapshots, and its default
# cap is 8192 MiB PER PROCESS. Nothing here ever set it, so three servers
# reserved 24 GiB and four reserved 32 GiB on a 31.4 GiB host. That is why two
# servers died mid-arm on 2026-08-03 leaving nothing in their logs but a
# truncated line, and why the surviving three sat at ~1 GB free with swap full.
#
# The limit on parallelism here was never VRAM. -ngl 99 puts the weights on the
# card: RssFile stays at ~158 MB while RssAnon reaches 7.4 GB, and the log says
# what it is doing -- "making room for prompt cache entry, removing oldest entry
# (size = 27.482 MiB)", over and over.
#
# Not 0. The ~600-token system prompt is shared by every note and is served from
# this cache; prompt eval logs 33 tokens, not 600. Disabling it re-evaluates the
# prefix per note at ~170 tok/s, about 3.5 s each.
#
# Sizing, measured rather than guessed. One cached entry is one request --
# ~600-token system prompt + ~50-token note + ~350 tokens of reasoning -- and
# weighs 25-32 MiB, so this model costs ~26 KiB of KV per token and a full 8192
# context is ~213 MiB. The 8192 MiB default is therefore ~38 full contexts of
# cache on a server with ONE slot, which is the actual absurdity.
#
# 512 MiB (~19 entries, ~2.4 contexts) fixed the memory problem but still logged
# 89 evictions in ten minutes, with prompt eval alternating 28 tokens on a prefix
# hit and ~515 on a miss. 1024 (~38 entries) roughly halves that for +512 MiB per
# process, which the host has: 22.4 GB free after the first fix. Misses are cheap
# next to decode (~150-195 ms against ~1.5 s), so this buys headroom, not speed.
#
# THIS VALUE CHANGES RESULTS. Cache reuse decides whether a prefix is recomputed
# or restored, and those paths do not produce bit-identical logits (the
# warm-server effect, 14/20 notes). Arms compared to each other must share it,
# exactly like NPROC.
CACHE_RAM_MIB=${CACHE_RAM_MIB:-1024}

case "$CARD" in
  # CT 140's address is asked for, not assumed. It was hardcoded to 192.168.0.5,
  # and when the container was rebuilt it came up on 192.168.0.119; the waiter
  # then polled a dead address for 110 minutes while the server was healthy the
  # whole time. A container's IP is a fact about the running system, so read it.
  5080) HOST=root@192.168.1.253; RUN="pct exec 140 -- bash -lc"
        EP=$(ssh -n -o ConnectTimeout=15 root@192.168.1.253 "pct exec 140 -- hostname -I" 2>/dev/null | awk '{print $1}')
        [ -z "$EP" ] && { echo "could not resolve CT140 address"; exit 1; }
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
  # The expected stem is derived from REPO generically. It used to grep for
  # E[24]B, which only exists in the gemma-4 names: for granite, Qwen or any
  # other family want_fam came back EMPTY and the case below degenerated to
  # "does the filename contain the quant", so a stale granite server answering
  # on a port we failed to claim would have passed. The check that exists to
  # catch a wrong model silently checked almost nothing for two thirds of the
  # model field.
  #
  #   unsloth/granite-4.0-1b-GGUF:UD-Q4_K_XL -> stem granite-4.0-1b, quant UD-Q4_K_XL
  local p=$1 want_fam want_q loaded
  # The stem is derived from REPO, which assumes the publisher names its files
  # after its repo. Not everyone does: ggml-org/SmolLM3-3B-GGUF ships
  # SmolLM3-Q4_K_M.gguf, with no size suffix, so the derived stem "SmolLM3-3B"
  # never matches and a correctly-loaded model is refused as wrong.
  #
  # That is a FALSE NEGATIVE in a guard whose whole job is catching false
  # positives (defect 30: a stale server answering with someone else's weights).
  # Weakening the match would give the stale-server case back, so instead the
  # caller may state the expected stem explicitly. Unset, behaviour is unchanged.
  want_fam=${VERIFY_FAM:-$(basename "${REPO%%:*}" | sed -E 's/-GGUF$//I')}
  want_q="${REPO##*:}"
  [ "$want_q" = "$REPO" ] && want_q=""   # no ':quant' in REPO
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
      "pct exec 140 -- bash -lc 'HF_HOME=$HFH nohup setsid $BIN -hf $REPO $DRAFT_ARG --host 0.0.0.0 --port $p -c 8192 -np 1 --cache-ram $CACHE_RAM_MIB --no-webui --no-mmproj -ngl 99 >/opt/tierA/shard-$LABEL-$p.log 2>&1 </dev/null &'" >/dev/null 2>&1
  else
    ssh -n -o ConnectTimeout=25 $HOST \
      "HF_HOME=$HFH nohup setsid $BIN -hf $REPO $DRAFT_ARG --host 0.0.0.0 --port $p -c 8192 -np 1 $DEV --cache-ram $CACHE_RAM_MIB --no-webui --no-mmproj -ngl 99 >/tmp/shard-$LABEL-$p.log 2>&1 </dev/null &" >/dev/null 2>&1
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
say "  -> running $NPROC processes, cache-ram ${CACHE_RAM_MIB} MiB each"

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

# Reap the client processes if this wrapper dies. Without this, killing
# shard_run.sh leaves its run_llamacpp.py children running: they are not in a
# process group that dies with the parent, and they keep issuing requests to
# BASE_PORT forever.
#
# That is not a tidiness problem, it is a correctness one. The next arm launched
# on the same BASE_PORT competes with the orphans for the same servers, and the
# only symptom is that it runs slowly -- the server's own per-task timings look
# healthy because each individual request is served normally, it just waited.
#
# Measured 2026-08-04: fifteen orphans accumulated across a day of stopped runs
# held the E2B Q4 no-MTP arm at 8.8 notes/min. Killing them took the same arm to
# 38.7 immediately, against 41.2 measured on ports nothing else was using. The
# gap was read as a 3x "unexplained residual" and produced a bogus 5.3x MTP
# speedup before anyone counted the python processes.
cleanup_children() {
  for cp in "${pids[@]:-}"; do
    [ -n "$cp" ] && kill "$cp" 2>/dev/null
  done
}
trap cleanup_children EXIT INT TERM

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
if [ $? -ne 0 ]; then say "FAIL $LABEL: merge rejected (see above), discarding"; rm -f "$OUT/$LABEL.pred.jsonl"; kill_servers; exit 1; fi

# Row count is not completion. An arm whose servers died mid-run still produces a
# row per note, each carrying a transport error, and this reported rows=10000/10000
# and "OK" for a file that was 97% connection failures. score.py caught it; the
# driver should not have needed rescuing.
#
# This used to discard the whole arm on a single errored row. That is the wrong
# trade at 10,000 notes: one dropped tunnel late in a four-hour arm threw away
# every clean row with it. Measured 2026-08-03, E2B Q4 10k -- the shard-3 server
# died, six notes errored, and the other 9,994 were about to be binned.
#
# So: re-run the errored notes against the servers that are still up, then judge
# what is left. The retry is legitimate rather than a fudge because this exact
# configuration reproduces itself byte-for-byte (finding 19: three runs of one
# arm, 1001/1001 identical), so a note re-answered here is the answer the clean
# run would have produced. What is NOT legitimate is quietly keeping an errored
# row, which is why anything still failing after the retry passes kills the arm.
errored_ids() {  # $1 = pred file -> ids on stdout
  python3 -c "
import json,sys
for l in open(sys.argv[1]):
    r=json.loads(l)
    if r.get('error'): print(r['id'])" "$1" 2>/dev/null
}

RETRY_PASSES=${RETRY_PASSES:-2}
pass=0
while :; do
  bad=$(errored_ids "$OUT/$LABEL.pred.jsonl")
  nbad=$(printf '%s' "$bad" | grep -c . || true)
  [ "${nbad:-0}" -eq 0 ] && break

  if [ "$pass" -ge "$RETRY_PASSES" ]; then
    pct=$(( nbad * 100 / $(wc -l < "$OUT/$LABEL.pred.jsonl") ))
    say "FAIL $LABEL: $nbad rows still errored (${pct}%) after $pass retry pass(es), discarding"
    say "  still failing: $(printf '%s' "$bad" | tr '\n' ' ')"
    mv "$OUT/$LABEL.pred.jsonl" "$OUT/$LABEL.pred.jsonl.errored"
    kill_servers
    exit 1
  fi
  pass=$((pass+1))
  say "  $nbad errored row(s), retry pass $pass: $(printf '%s' "$bad" | tr '\n' ' ')"

  # Re-run only those notes, on BASE_PORT. Health is checked first: the usual
  # cause of an errored row is that the server behind it died, and retrying into
  # a dead port just burns the retry budget.
  if ! ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 8 http://$EP:$BASE_PORT/health" >/dev/null 2>&1; then
    say "  FAIL: $BASE_PORT is not healthy, cannot retry"
    mv "$OUT/$LABEL.pred.jsonl" "$OUT/$LABEL.pred.jsonl.errored"
    kill_servers
    exit 1
  fi
  verify_model "$BASE_PORT" || { say "FAIL $LABEL: wrong model on $BASE_PORT at retry"; \
    mv "$OUT/$LABEL.pred.jsonl" "$OUT/$LABEL.pred.jsonl.errored"; kill_servers; exit 1; }

  printf '%s\n' "$bad" | grep . > "$SHARD_DIR/retry$pass.ids"
  python3 - "$GOLD" "$SHARD_DIR/retry$pass.ids" "$SHARD_DIR/retry$pass.gold.jsonl" <<'PY'
import json,sys
gold, idfile, out = sys.argv[1], sys.argv[2], sys.argv[3]
want={l.strip() for l in open(idfile) if l.strip()}
with open(out,"w") as fh:
    for line in open(gold):
        if json.loads(line)["id"] in want: fh.write(line)
PY
  python3 harness/run_llamacpp.py --model "$LABEL" --gold "$SHARD_DIR/retry$pass.gold.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$SHARD_DIR/retry$pass.out.jsonl" --base-url "http://$EP:$BASE_PORT" \
    >>"$OUT/shard_$LABEL.run.log" 2>&1

  # Splice the repaired rows in, preserving gold order. Only rows that came back
  # WITHOUT an error replace anything; a retry that failed again leaves the
  # original errored row in place so the loop can count it and give up.
  python3 - "$OUT/$LABEL.pred.jsonl" "$SHARD_DIR/retry$pass.out.jsonl" <<'PY'
import json,sys,os
pred, retry = sys.argv[1], sys.argv[2]
fixed={}
if os.path.exists(retry):
    for l in open(retry):
        r=json.loads(l)
        if not r.get("error"): fixed[r["id"]]=l
if fixed:
    lines=[]
    for l in open(pred):
        r=json.loads(l)
        lines.append(fixed.get(r["id"], l) if r.get("error") else l)
    with open(pred,"w") as fh: fh.writelines(lines)
print(f"spliced {len(fixed)} repaired row(s)")
PY
done

# Completion is a comparison, not a printout. got and exp were computed here and
# reported in the DONE line for weeks without ever being compared, so a merge
# that silently dropped rows -- the merge above counts them and says so -- still
# exited 0 and banked a short arm.
got=$(wc -l < "$OUT/$LABEL.pred.jsonl")
exp=$(wc -l < "$GOLD")
if [ "$got" -ne "$exp" ]; then
  say "FAIL $LABEL: incomplete arm, rows=$got/$exp, discarding"
  mv "$OUT/$LABEL.pred.jsonl" "$OUT/$LABEL.pred.jsonl.incomplete"
  kill_servers
  exit 1
fi
say "DONE $LABEL rows=$got/$exp wall=$(( (t1-t0)/60 ))m$(( (t1-t0)%60 ))s procs=$NPROC retries=$pass"
rm -rf "$SHARD_DIR"
kill_servers
