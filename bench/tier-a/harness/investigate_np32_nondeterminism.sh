#!/bin/bash
# WHY are 32-slot runs not reproducible? Isolate the cause rather than accept it.
#
# Established: two runs of -np 32 + MTP agree on only 75/100 extracted facts,
# while MTP at concurrency 1 is 100/100 and plain sequential is bit-identical
# across restarts. Something about serving many sequences at once is not
# reproducible, and "batching" is a description, not a cause.
#
# Four candidates, separated by construction:
#
#   A. SLOT COUNT alone. -np 32 but only ONE request in flight. If this is
#      reproducible, allocating 32 slots is harmless and the problem is traffic.
#   B. CONCURRENCY. -np 32 with N in flight, swept 2/4/8/32 to find where
#      reproducibility breaks -- a threshold is more informative than a verdict.
#   C. CONTINUOUS BATCHING. -np 32, 32 in flight, --no-cont-batching. If this
#      restores reproducibility the culprit is mid-flight batch reshaping.
#   D. KV LAYOUT. --kv-unified vs not, same traffic. Cross-sequence cache
#      interference rather than batch shape.
#
# Each config runs the SAME notes twice from a FRESH server and is judged only on
# whether it agrees with ITSELF. Identity with sequential is not the question.
# CT 140 is SHARED. Never `pkill -f llama-server` here: it kills every
# server in the container, including other sessions'. Kill by port.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-60}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8118
REPO=unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/np32_rootcause.log"; }

head -n "$N" "$GOLD" > "$OUT/.rc_slice.jsonl"

# $1 label  $2 server flags  $3 client concurrency
trial() {
  local label="$1" flags="$2" conc="$3"
  for pass in 1 2; do
    ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- bash -lc 'for p in $(seq 8100 8420); do pkill -f \"port $p \" 2>/dev/null; done; true'" >/dev/null 2>&1 || true
    sleep 5
    ssh -n -o ConnectTimeout=25 root@"$HOST" \
      "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $REPO --host 0.0.0.0 --port $PORT $flags --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
    local ok=0
    for _ in $(seq 1 90); do
      ssh -n -o ConnectTimeout=10 root@"$HOST" \
        "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
      sleep 10
    done
    [ "$ok" = 1 ] || { say "  $label: server never healthy"; return; }
    python3 harness/run_llamacpp.py --model "rc.$label" --gold "$OUT/.rc_slice.jsonl" \
      --thinking --max-tokens 8192 --concurrency "$conc" \
      --out "$OUT/rc_${label}_$pass.pred.jsonl" --base-url "http://$RIP:$PORT" >/dev/null 2>&1
  done
  python3 - "$OUT/rc_${label}_1.pred.jsonl" "$OUT/rc_${label}_2.pred.jsonl" "$label" <<'PY' | tee -a "$OUT/np32_rootcause.log"
import json,sys
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
a,b,lab = L(sys.argv[1]), L(sys.argv[2]), sys.argv[3]
ids=[i for i in a if i in b]
raw=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
def tr(r): return {(str(f.get("subject","")).strip().lower(),str(f.get("relation","")).strip().lower(),
                    str(f.get("object","")).strip().lower()) for f in (r.get("pred_nofloor") or [])}
fac=sum(1 for i in ids if tr(a[i])==tr(b[i]))
verdict = "REPEATABLE" if raw==len(ids) else ("facts-stable" if fac==len(ids) else "NOT repeatable")
print(f"  {lab:34s} raw {raw:3d}/{len(ids)}  facts {fac:3d}/{len(ids)}  -> {verdict}")
PY
}

say "A. slot count alone: -np 32, ONE request in flight"
trial "np32_conc1"        "-c 98304 -np 32"                     1
say "B. concurrency sweep at -np 32"
trial "np32_conc2"        "-c 98304 -np 32"                     2
trial "np32_conc8"        "-c 98304 -np 32"                     8
trial "np32_conc32"       "-c 98304 -np 32"                    32
say "C. continuous batching off, same traffic"
trial "np32_conc32_nocb"  "-c 98304 -np 32 --no-cont-batching"  32
say "D. KV layout"
trial "np32_conc32_kvu"   "-c 98304 -np 32 --kv-unified"        32

rm -f "$OUT/.rc_slice.jsonl"
say "=== ROOT CAUSE SWEEP DONE ==="
