#!/bin/bash
# Restart one shard's llama-server on the XTX host. Usage: bash -s <port> <repo> <draft>
#
# Sent over ssh stdin rather than as a quoted argument: the command otherwise
# carries absolute remote paths, and nesting quotes through ssh mangles them
# (same reasoning as shard_run.sh kill_servers).
#
# Flags are copied verbatim from the surviving siblings so the restarted server
# is configured identically. A server that comes back with different settings is
# worse than one that stays down, because the arm keeps running either way.
PORT=${1:?port}
REPO=${2:-unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL}
DRAFT=${3:-unsloth/gemma-4-E2B-it-GGUF}

echo "--- last 25 lines of the previous log for $PORT ---"
tail -25 "/tmp/shard-restart-$PORT.log" 2>/dev/null \
  || ls -t /tmp/shard-*"$PORT"*.log 2>/dev/null | head -1 | xargs -r tail -25 \
  || echo "(no previous log found)"

if ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q .; then
  echo "port $PORT already listening, not starting a second server"
  exit 0
fi

HF_HOME=/mnt/media/tierbench/hf nohup setsid \
  /mnt/media/tierbench/bin/llama-b10210/llama-server \
  -hf "$REPO" -hfd "$DRAFT" \
  --host 0.0.0.0 --port "$PORT" -c 8192 -np 1 --device Vulkan1 \
  --no-webui --no-mmproj -ngl 99 \
  > "/tmp/shard-restart-$PORT.log" 2>&1 </dev/null &
echo "launched on $PORT"
