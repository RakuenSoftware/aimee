#!/bin/bash
# Hold the reaper's lease open for a long-running test. Run ON THE .252 HOST,
# backgrounded, for as long as the environment is wanted.
#
# /root/AGENTS.md: renewing slides the lease a full TTL from the moment you
# renew, with no cap, and the advice is to renew on a comfortable margin so a
# slow step never straddles the deadline. 4h TTL, renewed every 2h.
#
# This satisfies the LEASE clock only. The liveness clock -- 4h with under 10 MiB
# of combined disk+network activity -- is reset by the guest doing real work, not
# by this loop. An idle box still dies, by design.
#
# Usage: nohup keepalive-loop.sh ct:9078 >/var/log/aimee-keepalive-loop.log 2>&1 &
set -u
TARGET="${1:?usage: keepalive-loop.sh <ct:ID|vm:ID|path>}"
INTERVAL="${INTERVAL:-7200}"

while :; do
  if aimee-keepalive "$TARGET" >/dev/null 2>&1; then
    echo "$(date -Is) renewed $TARGET"
  else
    echo "$(date -Is) FAILED to renew $TARGET" >&2
  fi
  sleep "$INTERVAL"
done
