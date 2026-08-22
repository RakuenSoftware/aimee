#!/bin/bash
# Install the config module on BOTH daemon buses.
#
# Config was extracted into its own process module. Both daemons now refuse to
# start without it:
#
#     aimee-kb: config module unavailable: config module unavailable
#
# so a deployment that ships aimee-kb / aimee-server but not
# aimee-module-config gets two daemons that exit immediately, with a message
# that names the module rather than the missing file -- easy to read as a bus or
# grant problem when it is simply not installed.
#
# process-contracts.json: id=config, principal_ref=2, placements=[server,kb],
# serves config-store (event 4609).
#
# Ordering is the awkward part and is why this is its own script. The daemon
# creates the bus socket, then blocks waiting for config-store; the module
# cannot connect until that socket exists. So the grants must be written BEFORE
# the daemon starts (a daemon reads modules.d once, at startup -- a grant written
# later is not seen and the attach is denied), and the module must be started
# AFTER, as soon as the socket appears. Hence: grants here, then start the
# daemon, then call this script's start half.
#
# Usage: install-config-module.sh grants|start|start-kb|start-server|both
# Run AS ROOT in the container.
set -u
MODE="${1:-both}"
BIN=/usr/local/libexec/aimee-modules/aimee-module-config
KB_CONF=/root/.config/aimee
KB_SOCK="$KB_CONF/kb-module-bus.sock"
SRV_SOCK=/root/server-module-bus.sock

write_grant() {  # $1 = modules.d dir
  mkdir -p "$1"
  cat > "$1/config.grant" <<EOF
version=1
principal_class=1
principal_ref=2
uid=self
executable=$BIN
publish=
subscribe=
request=
serve=4609
EOF
}

start_one() {  # $1 = socket, $2 = AIMEE_HOME, $3 = log
  for _ in $(seq 1 30); do [ -S "$1" ] && break; sleep 1; done
  [ -S "$1" ] || { echo "config: bus socket $1 never appeared"; return 1; }
  # Scoped to this socket. A bare `pkill -f aimee-module-config` would take down
  # the instance on the OTHER daemon's bus -- and another session's, since this
  # container is shared.
  pkill -f "aimee-module-config $1" 2>/dev/null
  sleep 1
  AIMEE_HOME="$2" nohup "$BIN" "$1" >"$3" 2>&1 &
  sleep 3
  if pgrep -f "aimee-module-config $1" >/dev/null; then
    echo "config on $1: RUNNING"
  else
    echo "config on $1: EXITED"
    tail -3 "$3"
  fi
}

case "$MODE" in
  grants|both)
    write_grant "$KB_CONF/modules.d/kb"
    write_grant /root/modules.d/server
    echo "config grants installed (kb + server)"
    ;;
esac

case "$MODE" in
  start|start-kb|start-server|both)
    [ -x "$BIN" ] || { echo "config: $BIN missing or not executable" >&2; exit 1; }
    ;;
esac

# Per-daemon modes exist so a start script can launch ONLY its own instance in
# the background while its daemon is still coming up, instead of blocking on the
# other daemon's socket for 30s.
case "$MODE" in
  start|both|start-kb)     start_one "$KB_SOCK"  "$KB_CONF" /root/config-module-kb.log ;;
esac
case "$MODE" in
  start|both|start-server) start_one "$SRV_SOCK" /root      /root/config-module-server.log ;;
esac
