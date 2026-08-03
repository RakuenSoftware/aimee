#!/bin/bash
# Remove only OUR servers from CT 140 and leave the container to its other user.
#
# CT 140 is shared. Another session is serving gemma-4-E4B-it-UD-Q6_K_XL on port
# 8099. Every arm in this harness began with
#
#     pct exec 140 -- pkill -f llama-server
#
# which kills EVERY llama-server in the container, including theirs. That is
# almost certainly what killed our own E2B Q4 arm mid-run (9725 transport
# errors), symmetrically, and it means this harness has been destroying another
# session's work all night.
#
# Our ports are 8110, 8115-8122, 8200-8203, 8250, 8300-8302, 8400-8403. Kill by
# port, never by process name.
for port in 8110 8115 8116 8117 8118 8119 8120 8121 8122 8200 8201 8202 8203 8250; do
  pct exec 140 -- bash -lc "pkill -f 'port $port ' 2>/dev/null" >/dev/null 2>&1
done
sleep 3
echo "--- surviving llama-servers in CT 140 (should be ONLY the other session's):"
pct exec 140 -- pgrep -a llama-server | grep -oE '\-\-port [0-9]+' | sort -u
echo "--- vram:"
nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader
