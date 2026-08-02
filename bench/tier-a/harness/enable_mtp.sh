#!/bin/bash
# Get MTP actually enabled, rather than loaded-and-ignored.
#
# Why the obvious way fails: --mtp is registered for LLAMA_EXAMPLE_DOWNLOAD only,
# so llama-server has no such flag, and the speculative type is inferred from the
# DOWNLOAD PLAN rather than from the model file (common/arg.cpp:549-560). Worse,
# passing the head with -md actively suppresses that inference:
#
#   "an explicit draft file selection (e.g. -md with -hfd) disables the sidecar
#    resolution of the draft repo"   -> plan_spec.mtp = {}
#
# So -m + -md is the one combination that cannot reach MTP, which is what we ran.
#
# The repo path is what sets it: -hf <repo> with no -md lets the planner resolve
# the sidecar and set types = {DRAFT_MTP}. Unsloth is used rather than ggml-org
# because it ships the UD quants the whole ladder is measured on -- switching to
# ggml-org's plain quant to get MTP would trade the confound we are removing for
# a new one.
#
# Unverified going in: whether the resolver finds unsloth's MTP/ SUBDIRECTORY
# layout, or only a sidecar at the repo root. That is what this checks.
set -u
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8116
REPO=${REPO:-unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL}
# The DRAFT repo is what triggers sidecar discovery (common/arg.cpp:398-406):
# plan_spec is built from params.speculative.draft.mparams, so with no -hfd
# there is nothing to resolve an MTP head from. Same repo, no -md.
DRAFT_REPO=${DRAFT_REPO:-unsloth/gemma-4-E4B-it-GGUF}
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/enable_mtp.log"; }

say "serving: -hf $REPO -hfd $DRAFT_REPO (draft REPO set, no -md)"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- pkill -f 'llama-server' " >/dev/null 2>&1 || true
sleep 4
# HF_HOME points at the existing model store so a cached blob is reused instead
# of re-downloading 5 GiB.
ssh -n -o ConnectTimeout=25 root@"$HOST" \
  "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $REPO -hfd $DRAFT_REPO --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 > /opt/tierA/mtp-enable.log 2>&1 </dev/null &'" >/dev/null 2>&1

say "waiting for load (a first fetch can take a few minutes)"
ok=0
for _ in $(seq 1 120); do
  ssh -n -o ConnectTimeout=10 root@"$HOST" \
    "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
  sleep 15
done
if [ "$ok" != 1 ]; then
  say "server never healthy; last lines:"
  ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- tail -30 /opt/tierA/mtp-enable.log" 2>&1 | tee -a "$OUT/enable_mtp.log"
  exit 1
fi

say "what the server resolved:"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- grep -iE 'mtp|sidecar|draft|spec|resolved|downloading' /opt/tierA/mtp-enable.log | head -20" \
  2>&1 | sed 's/^/    /' | tee -a "$OUT/enable_mtp.log"

say "is speculation actually ON?"
ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin); s=d[0] if d else {}
print('    slots      :', len(d))
print('    speculative:', s.get('speculative'))
print('    spec types :', (s.get('params') or {}).get('speculative.types'))
" 2>&1 | tee -a "$OUT/enable_mtp.log"
say "=== DONE ==="
