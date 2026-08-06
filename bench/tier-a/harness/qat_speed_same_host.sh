#!/bin/bash
# Is QAT intrinsically faster, or was the 1.7x a rented host?
#
# On rented 3090s, gemma-4-12B QAT probed at ~232 tok/s and non-QAT at ~137, with
# draft acceptance ~86% and ~90% respectively. Acceptance being HIGHER on the
# slower arm rules out speculation efficiency as the cause. The QAT file is
# 6.26 GiB against 6.86 GiB, a 9% difference that does not explain 1.7x.
#
# Every comparison so far put the two models on DIFFERENT machines, and the same
# arm has been measured at 84.4 to 131.9 tok/s across five placements, so host
# variance is large enough to produce the whole effect. This runs both models
# SEQUENTIALLY on one card with everything else identical: same server binary,
# same context, same cache setting, same probe text, same MTP draft family.
#
# Candidate mechanism if QAT really is faster: unsloth's dynamic quant assigns
# bit widths per tensor by sensitivity, so the same UD-Q4_K_XL label can produce a
# different tensor-type mix on QAT weights. i-quant tensors dequantise more slowly
# than K-quants, which would show up as speed with no change in acceptance.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=results/ct140
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/qat_speed.log"; }

HOST=root@192.168.1.253
EP=$(ssh -n -o ConnectTimeout=15 $HOST "pct exec 140 -- hostname -I" 2>/dev/null | awk '{print $1}')
PORT=8992
BIN=/opt/llama.cpp/build-cuda/bin/llama-server
HFH=/opt/hf
LOGDIR=/opt/tierA

probe_one() {   # $1 label, $2 repo, $3 draft
  local LBL=$1 REPO=$2 DRAFT=$3
  say "--- $LBL"
  ssh -n -o ConnectTimeout=25 $HOST \
    "pct exec 140 -- bash -lc 'for p in \$(pgrep -f \"port $PORT\"); do kill \$p; done; sleep 3; true'" >/dev/null 2>&1
  ssh -n -o ConnectTimeout=25 $HOST \
    "pct exec 140 -- bash -lc 'HF_HOME=$HFH nohup setsid $BIN -hf $REPO -hfd $DRAFT --host 0.0.0.0 --port $PORT -c 8192 -np 1 --cache-ram 1024 --no-webui --no-mmproj -ngl 99 >$LOGDIR/qat-$LBL.log 2>&1 </dev/null &'" >/dev/null 2>&1

  # wait on evidence, not on a clock: healthy, or the server process is gone
  while true; do
    if ssh -n -o ConnectTimeout=10 $HOST "curl -sf --max-time 5 http://$EP:$PORT/health" >/dev/null 2>&1; then break; fi
    if ! ssh -n -o ConnectTimeout=10 $HOST "pct exec 140 -- bash -lc 'pgrep -f \"port $PORT\" >/dev/null'" 2>/dev/null; then
      say "    FAIL: server for $LBL is not running"; ssh -n $HOST "pct exec 140 -- bash -lc 'tail -5 $LOGDIR/qat-$LBL.log'" 2>&1 | tail -5
      return 1
    fi
    sleep 15
  done
  LOADED=$(ssh -n $HOST "curl -s http://$EP:$PORT/props" | python3 -c "import json,sys;print((json.load(sys.stdin).get('model_path') or '').split('/')[-1])" 2>/dev/null)
  say "    loaded $LOADED"

  pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -L "$PORT:$EP:$PORT" "$HOST" >/dev/null 2>&1 </dev/null &
  sleep 6

  python3 - "$LBL" "$PORT" <<'PY' 2>&1 | tee -a "$OUT/qat_speed.log"
import json, sys, urllib.request, statistics
lbl, port = sys.argv[1], sys.argv[2]
PROMPT = ("Extract every durable fact from this note as JSON: "
          "Ingrid Okonkwo works for Wexford Analytics in Trondheim and is the release manager.")
tps, acc, tot_d, tot_a = [], [], 0, 0
for i in range(6):
    body = json.dumps({"model": "g", "messages": [{"role": "user", "content": PROMPT}],
                       "max_tokens": 600, "temperature": 0}).encode()
    req = urllib.request.Request("http://127.0.0.1:%s/v1/chat/completions" % port,
                                 data=body, headers={"Content-Type": "application/json"})
    try:
        d = json.loads(urllib.request.urlopen(req, timeout=300).read())
    except Exception as e:
        print("   probe %d failed: %s" % (i, e)); continue
    t = d.get("timings") or {}
    tps.append(t.get("predicted_per_second") or 0)
    dn, da = t.get("draft_n") or 0, t.get("draft_n_accepted") or 0
    tot_d += dn; tot_a += da
print("   %-28s tok/s median=%.1f  (%s)" % (lbl, statistics.median(tps) if tps else 0,
      " ".join("%.0f" % x for x in tps)))
print("   %-28s draft accepted %d/%d = %.1f%%" % (lbl, tot_a, tot_d, 100.0*tot_a/tot_d if tot_d else 0))
PY
}

say "=== same-host QAT speed test on the 5080 (LXC 140), both models, one card"
probe_one qat     unsloth/gemma-4-12B-it-qat-GGUF:UD-Q4_K_XL  unsloth/gemma-4-12B-it-qat-GGUF:MTP/mtp-gemma-4-12B-it-Q8_0.gguf
probe_one nonqat  unsloth/gemma-4-12b-it-GGUF:UD-Q4_K_XL      unsloth/gemma-4-12b-it-GGUF:MTP/mtp-gemma-4-12b-it-Q8_0.gguf
say "=== SAME-HOST QAT SPEED TEST COMPLETE ==="
