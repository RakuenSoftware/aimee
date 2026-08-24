#!/bin/bash
# Start the memory module against aimee-kb's module bus.
#
# The kb creates the bus socket at <config dir>/kb-module-bus.sock during
# startup, so this must run AFTER the kb is up. In a real deployment the module
# supervisor starts it from the kb.modules manifest; here it is started by hand
# so the module's own stdout/stderr is visible.
# Run AS ROOT in the container.
set -u
SOCK=/root/.config/aimee/kb-module-bus.sock
for _ in $(seq 1 30); do
  [ -S "$SOCK" ] && break
  sleep 1
done
if [ ! -S "$SOCK" ]; then
  echo "module bus socket never appeared at $SOCK" >&2
  exit 1
fi
# Kill only THIS bus's instance. A bare `pkill -f aimee-module-memory` also
# takes down the server-side module, which runs the same binary against a
# different socket -- and then a RERANK probe reports the server module missing
# for no visible reason.
pkill -f "aimee-module-memory $SOCK" 2>/dev/null
sleep 1
cd /root
AIMEE_HOME=/root/.config/aimee \
  nohup /usr/local/libexec/aimee-modules/aimee-module-memory "$SOCK" \
  >/root/memory-module.log 2>&1 &
echo $! > /root/memory-module.pid
sleep 4
echo "module pid=$(cat /root/memory-module.pid)"
if kill -0 "$(cat /root/memory-module.pid)" 2>/dev/null; then
  echo "state: RUNNING"
else
  echo "state: EXITED"
fi
tail -8 /root/memory-module.log
