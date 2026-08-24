#!/bin/bash
# Start run-suite.sh detached, writing to /root/suite.out.
#
# The suite now takes long enough that an ssh invocation can hit its own client
# timeout while the run is still going. When that happened the ssh session died
# but the run did NOT: several orphaned run-suite.sh processes piled up, each
# restarting daemons under the others, and the container ended up with duplicate
# aimee-server and aimee-kb processes fighting over the same sockets.
#
# Detaching makes the run independent of the caller's connection, so a timeout
# on the observing side can no longer corrupt the thing being observed.
# Run AS ROOT in the container.
set -u
export LC_ALL=C

# Refuse to stack runs -- that is what caused the mess in the first place.
if pgrep -f 'bash /root/run-suite.sh' >/dev/null 2>&1; then
  echo "a suite run is already in progress; not starting another"
  exit 1
fi

: > /root/suite.out
setsid nohup bash /root/run-suite.sh "$(cat /root/api-bearer.txt)" \
  >/root/suite.out 2>&1 </dev/null &
echo "started; poll /root/suite.out"
