#!/bin/bash
# Install and start the db1 module on aimee-server's bus.
#
# mTLS will not start without it. server_tls_init_default() calls
# pki_mtls_ramp_init(), which is db1_pki_ramp_init() -- served by db1 stage 19
# (db1-pki, event 11795). With no module answering, the server logs:
#
#     WARN db1.pki: DB1 pki is unreachable (module call result 1)
#     WARN pki.ramp: mTLS ramp startup self-test failed; refusing mTLS startup
#     ERROR server.http: tls_port=8743 set but TLS cert/key not loadable; TLS DISABLED
#
# The last line is what an operator sees, and it blames the certificate -- which
# may be perfectly fine. The actual cause is a missing module two layers down.
#
# process-contracts.json: id=db1, principal_ref=30, placements=[server],
# nineteen stages on 11777..11795. The grant serves all of them: the ramp needs
# 11795, but the server calls the others (sessions, agent work, conversation)
# throughout, and a partial grant produces the same "unreachable" line for
# whichever one is missing.
# Run AS ROOT in the container.
set -u
CONF=/root
SOCK=/root/server-module-bus.sock
BIN=/usr/local/libexec/aimee-modules/aimee-module-db1
mkdir -p "$CONF/modules.d/server"

cat > "$CONF/modules.d/server/db1.grant" <<EOF
version=1
principal_class=1
principal_ref=30
uid=self
executable=$BIN
publish=
subscribe=
request=
serve=11777,11778,11779,11780,11781,11782,11783,11784,11785,11786,11787,11788,11789,11790,11791,11792,11793,11794,11795
EOF
echo "db1 grant installed (19 stages)"

[ "${1:-both}" = "grants" ] && exit 0

[ -x "$BIN" ] || { echo "db1: $BIN missing or not executable" >&2; exit 1; }

for _ in $(seq 1 30); do [ -S "$SOCK" ] && break; sleep 1; done
[ -S "$SOCK" ] || { echo "db1: server module bus socket missing" >&2; exit 1; }

# Scoped to this socket, like every other module here: the container is shared.
pkill -f "aimee-module-db1 $SOCK" 2>/dev/null
sleep 1
cd /root
# Its own process, so its own environment -- the same trap the postgres module
# hit. db1 refuses outright rather than guessing:
#
#   db1: AIMEE_DB1_PATH is unset; refusing to serve. This process cannot read
#        the daemon's configuration, so it must be told which database to open.
#
# which is the right behaviour (opening the wrong sqlite file would be worse
# than not starting) but means the path has to be supplied here. It must match
# what the daemon opens; deploy/container/server-entrypoint.sh uses
# $AIMEE_HOME/aimee.db and says the same thing.
AIMEE_HOME=/root \
AIMEE_DB1_PATH="${AIMEE_DB1_PATH:-/root/aimee.db}" \
  nohup "$BIN" "$SOCK" >/root/db1-module.log 2>&1 &
sleep 3
if pgrep -f "aimee-module-db1 $SOCK" >/dev/null; then
  echo "db1: RUNNING"
else
  echo "db1: EXITED"
  tail -5 /root/db1-module.log
fi
