#!/bin/bash
# Restore the E2B UD-Q4 lane on 8112 and retire the reference-Q8 control on 8114.
#
# The Q8 control existed to answer one question -- whether the thinking
# suppression was Unsloth's repack or the model -- and it answered it, so its
# ~8GB of VRAM is better spent on the E2B arm. The card is 16GB and three
# servers do not fit.
pct exec 140 -- bash -lc 'pkill -f "port 8114"; sleep 3; true'
pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m /opt/hf/e2b-q4.gguf --host 0.0.0.0 --port 8112 -c 8192 --no-webui --no-mmproj -ngl 99 > /opt/tierA/e2b-q4.log 2>&1 </dev/null & disown; sleep 5; echo launched'
pct exec 140 -- bash -lc 'pgrep -a llama-server; tail -3 /opt/tierA/e2b-q4.log'
