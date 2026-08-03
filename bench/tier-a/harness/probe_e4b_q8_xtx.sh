#!/bin/bash
# Measure resident VRAM for E4B Q8 + MTP on the XTX, to pin the shard count.
# Estimating from disk size was wrong by ~2x on the 5080 (predicted 5253 MiB for
# E2B Q8, measured 3244), so the XTX number is measured rather than carried over
# from the CUDA figure.
pkill -f llama-server >/dev/null 2>&1 || true
sleep 4
HF_HOME=/mnt/media/tierbench/hf nohup setsid \
  /mnt/media/tierbench/bin/llama-b10210/llama-server \
  -hf unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL -hfd unsloth/gemma-4-E4B-it-GGUF \
  --host 0.0.0.0 --port 8251 -c 8192 -np 1 --device Vulkan1 \
  --no-webui --no-mmproj -ngl 99 > /tmp/sz.log 2>&1 </dev/null &
for i in $(seq 1 60); do
  curl -sf --max-time 5 http://127.0.0.1:8251/health >/dev/null 2>&1 && { echo "healthy after ${i}0s"; break; }
  sleep 10
done
echo "--- devices (Vulkan1 must be the XTX, not the Phoenix iGPU):"
/mnt/media/tierbench/bin/llama-b10210/llama-server --list-devices 2>/dev/null | grep Vulkan1
echo "--- free VRAM now (total minus free = in use by our server):"
/mnt/media/tierbench/bin/llama-b10210/llama-server --list-devices 2>/dev/null | grep -oE '\(([0-9]+) MiB, ([0-9]+) MiB free\)'
pkill -f llama-server >/dev/null 2>&1 || true
