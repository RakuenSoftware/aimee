#!/bin/bash
# Install and start the memory module on aimee-server's bus.
#
# The server hosts its OWN module bus, independent of the kb's:
# obs_bus_configure_daemon_module_runtime("server", config_default_dir()) gives
# <config dir>/server-module-bus.sock and <config dir>/modules.d/server. So the
# module runs once per daemon, with its own grant, against its own socket.
#
# The server calls FOUR memory stages -- EXTRACT_INDEX, WRITE, RETRIEVE and
# RERANK -- two of which (retrieve, rerank) the kb never calls, so this is the
# only place they get exercised at all.
# Run AS ROOT in the container.
set -u
CONF=/root                      # the server's AIMEE_HOME in this harness
mkdir -p "$CONF/modules.d/server"

cat > "$CONF/modules.d/server/memory.grant" <<'EOF'
version=1
principal_class=1
principal_ref=7
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-memory
publish=
subscribe=
request=
serve=5889,5890,5891,5892,5893,5894
EOF
echo "server grant installed"

SOCK="$CONF/server-module-bus.sock"
for _ in $(seq 1 30); do
  [ -S "$SOCK" ] && break
  sleep 1
done
[ -S "$SOCK" ] || { echo "server module bus socket never appeared at $SOCK" >&2; exit 1; }

pkill -f "aimee-module-memory $SOCK" 2>/dev/null
sleep 1
cd /root
AIMEE_HOME=/root nohup /usr/local/libexec/aimee-modules/aimee-module-memory "$SOCK" \
  >/root/memory-module-server.log 2>&1 &
echo $! > /root/memory-module-server.pid
sleep 4
pid="$(cat /root/memory-module-server.pid)"
echo "server-side module pid=$pid"
kill -0 "$pid" 2>/dev/null && echo "state: RUNNING" || echo "state: EXITED"
tail -5 /root/memory-module-server.log
