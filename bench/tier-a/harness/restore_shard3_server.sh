#!/bin/bash
# Restart the shard-3 llama-server for the E2B.UD-Q4_K_XL.10k arm on the XTX host.
#
# Sent to the XTX host over ssh stdin, not as a quoted argument. Written to a
# file and piped because the command text otherwise carries an absolute remote
# path, and because nesting quotes through ssh mangles them (see shard_run.sh
# kill_servers for the same reasoning).
#
# The flags below are copied verbatim from the surviving siblings on 8400-8402
# so the restarted server is configured identically. Do not "tidy" them.
HF_HOME=/mnt/media/tierbench/hf nohup setsid \
  /mnt/media/tierbench/bin/llama-b10210/llama-server \
  -hf unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL \
  -hfd unsloth/gemma-4-E2B-it-GGUF \
  --host 0.0.0.0 --port 8403 -c 8192 -np 1 --device Vulkan1 \
  --no-webui --no-mmproj -ngl 99 \
  > /tmp/shard-E2B-10k-8403-restart.log 2>&1 </dev/null &
echo "launched"
