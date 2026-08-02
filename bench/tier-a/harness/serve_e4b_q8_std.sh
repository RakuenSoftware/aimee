#!/bin/bash
# Serve the REFERENCE (non-Unsloth-Dynamic) gemma-4-E4B Q8_0 on the bench CT.
#
# Every E4B number in this benchmark was taken on an Unsloth Dynamic quant
# (UD-Q4_K_XL / UD-Q6_K_XL / UD-Q8_K_XL). The thinking-suppression finding is
# therefore confounded with the quantisation: it is not yet known whether the v4
# prompt suppresses reasoning in gemma-4-E4B, or only in Unsloth's repack of it.
#
# ggml-org/gemma-4-E4B-it-Q8_0 is the llama.cpp reference conversion, so it is
# the closest thing to a stock release and the right control. Served on 8114
# alongside the UD-Q4 lane on 8113 so both can be probed in one pass.
set -euo pipefail
pct exec 140 -- bash -lc '
  nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server \
    -m /opt/hf/e4b-q8-std.gguf --host 0.0.0.0 --port 8114 \
    -c 8192 --no-webui --no-mmproj -ngl 99 \
    > /opt/tierA/e4b-q8-std.log 2>&1 </dev/null &
  echo launched
'
