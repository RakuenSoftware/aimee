#!/bin/bash
# What does quantisation cost, and what does it buy back in RAM?
#
# We ship Q4_K_M and every quality number in the benchmark was measured at Q8_0.
# That gap has been carried in the docs as an unquantified caveat: "expect the
# shipped configuration to be somewhat worse than the table says, by an amount we
# have not quantified". This lane quantifies it.
#
# Unsloth Dynamic (UD) quants throughout rather than the stock k-quants. They
# quantise different tensors to different widths instead of applying one width
# uniformly, which is the variant we would ship if we ship Q4 at all, so it is
# the one worth measuring.
#
# Thinking is ON. That is the shipped configuration (kb_curator_provider.c:198
# leaves the disable flag off) and it is worth +0.12 F1 on E2B, so a
# quantisation sweep with thinking off would measure a system we do not run.
#
# --no-mmproj on every arm. gemma-4 ships its vision/audio encoder as a SEPARATE
# mmproj GGUF, so the text weights are already minimal — but llama-server pulls
# and loads the projector unless told not to. Every sweep in this benchmark
# passes this flag and the shipped compose/curator-llm service does NOT, which
# costs ~0.5GB of resident memory in production for a text-only task. Measuring
# with the flag and shipping without it is exactly the benchmark/product drift
# this lane exists to close.
#
# Memory is recorded per arm, not inferred from the file size on disk. A GGUF's
# size on disk is not its resident footprint: the KV cache for -c 8192 is
# allocated on top and is identical across quants, so RAM saved is always less
# than bytes saved.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=${OUT:-results/quant}
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8097}
MAXTOK=8192
# DEVICE pins the backend device. On a box with more than one GPU this is not
# optional: .254 enumerates the Phoenix iGPU as Vulkan0 and the 7900 XTX as
# Vulkan1, and llama.cpp defaults to the first. An unpinned run there measures
# an 8GB integrated GPU sharing host RAM, which is how this benchmark previously
# recorded a 30B MoE at 0.68 tok/s and read it as a broken architecture.
DEVICE=${DEVICE:-}
DEVARG=""
[ -n "$DEVICE" ] && DEVARG="--device $DEVICE"

# label|repo:quant
MODELS=(
  "E2B.UD-Q4_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL"
  "E2B.UD-Q6_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL"
  "E2B.UD-Q8_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL"
  "E4B.UD-Q4_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL"
  "E4B.UD-Q6_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q6_K_XL"
  "E4B.UD-Q8_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }

  echo "=== SERVE $LABEL ($REPO) ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj -ngl 99 $DEVARG \
      >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 360); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 10
  done

  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --thinking --max-tokens $MAXTOK \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
          --json-out "$OUT/$LABEL.score.json" >/dev/null 2>>"$LOG"
      if [ -s "$OUT/$LABEL.score.json" ]; then echo "OK   $LABEL"
      else echo "FAIL $LABEL -> scorer refused"; rm -f "$PRED"; fi
    else
      echo "FAIL $LABEL -> runner error"; rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> server never healthy: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-160)"
  fi

  # Resident footprint. Two independent sources, because neither alone answers
  # "how much RAM does this cost me":
  #
  #   VmHWM  - peak host RSS of the server process, read from /proc BEFORE the
  #            kill. This is the host-RAM number a user feels, and it is the one
  #            the GGUF's size on disk does NOT predict: mmap means resident can
  #            be lower than the file, while the KV cache and compute buffers
  #            push it higher.
  #   buffers- llama.cpp's own load report, which splits GPU from CPU and names
  #            the KV cache separately. The KV cache is identical across quants
  #            at a fixed -c, so RAM saved by a smaller quant is always LESS than
  #            bytes saved on disk.
  #
  # Recorded even on failure: an arm that would not fit is itself a result.
  hwm_kib=$(grep -oE '^VmHWM:[[:space:]]+[0-9]+' "/proc/$SRV/status" 2>/dev/null | grep -oE '[0-9]+')
  # Match CUDA0 or Vulkan0/1 — the buffer line is named for the backend, and a
  # CUDA-only pattern silently yields null on the Vulkan host.
  model_mib=$(grep -oE '(CUDA[0-9]|Vulkan[0-9]) model buffer size *= *[0-9.]+ MiB' "$LOG" | tail -1 | grep -oE '[0-9.]+' | tail -1)
  served_on=$(grep -oE 'using device [A-Za-z0-9]+' "$LOG" | tail -1 | sed 's/using device //')
  kv_mib=$(grep -oiE 'KV self size *= *[0-9.]+ MiB' "$LOG" | tail -1 | grep -oE '[0-9.]+' | tail -1)
  cpu_mib=$(grep -oE 'CPU model buffer size *= *[0-9.]+ MiB' "$LOG" | tail -1 | grep -oE '[0-9.]+' | tail -1)
  printf '{"model":"%s","repo":"%s","thinking":true,"no_mmproj":true,"ctx":8192,"device_requested":"%s","device_used":"%s","peak_host_rss_mib":%s,"gpu_model_mib":%s,"cpu_model_mib":%s,"kv_mib":%s}\n' \
    "$LABEL" "$REPO" "${DEVICE:-default}" "${served_on:-unknown}" "$( [ -n "${hwm_kib:-}" ] && echo $((hwm_kib/1024)) || echo null )" \
    "${model_mib:-null}" "${cpu_mib:-null}" "${kv_mib:-null}" \
    > "$OUT/$LABEL.device.json"

  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  # 6 quants of two models is ~30GB of downloads against a disk that has been
  # near full before. Keep only the weights currently under test.
  [ -x harness/prune_models.sh ] && KEEP="$REPO" HF_HOME="$HF_HOME" \
      bash harness/prune_models.sh 2>/dev/null | tail -1
  df -h . | tail -1
done
# Quality against memory, in one table, because neither number decides alone.
OUT="$OUT" $PY - <<'EOF'
import json, glob, os
out = os.environ["OUT"]
rows = []
for f in sorted(glob.glob(f"{out}/*.device.json")):
    d = json.load(open(f))
    lab = d["model"]
    s = f"{out}/{lab}.score.json"
    sc = json.load(open(s)) if os.path.exists(s) else None
    rows.append((lab, sc, d))
hdr = ("arm", "F1", "P", "R", "hostRSS", "gpuMiB", "kvMiB")
print("\n%-22s %7s %7s %7s %9s %8s %7s" % hdr)
for lab, sc, d in rows:
    if sc:
        st = sc["strict"]
        f1, p, r = "%.4f" % st["f1"], "%.4f" % st["precision"], "%.4f" % st["recall"]
    else:
        f1, p, r = "REFUSED", "-", "-"
    print("%-22s %7s %7s %7s %9s %8s %7s" % (
        lab, f1, p, r,
        d.get("peak_host_rss_mib"), d.get("gpu_model_mib"), d.get("kv_mib")))
EOF
echo "SWEEP_QUANT_DONE"
