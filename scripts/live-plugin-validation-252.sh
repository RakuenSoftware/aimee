#!/bin/sh
# Live validation: a REAL aimee-server, a PROVISIONED plugin instance, exercised
# over the /v1 HTTP surface. This is the evidence the retirement question needs,
# and the plugin e2e suites do not provide it: they stand up a bare bus host, not
# the daemon.
#
# Runs entirely on the test host under a SHORT path (/tmp/al). The path matters:
# a UNIX socket path is capped at 108 bytes, and a longer one is silently
# truncated -- the server comes up and its socket is unreachable.
#
# Scoped: its own HOME, its own module bus, its own policy dir, removed at the
# end. .252 hosts a live aimee deployment of its own; nothing here touches it,
# and no blanket pkill is used.
set -eu

HOST="${HOST:-root@192.168.1.252}"
REMOTE=/tmp/al
PKG=src/build/live-pkg.tgz

SRV=aimee-server
MOD=src/build/obj/aimee-module

[ -x "$SRV" ] || { echo "missing ./$SRV -- run: make -C src server" >&2; exit 2; }
[ -x "$MOD" ] || { echo "missing $MOD -- run: (cd server-go && go build -o ../$MOD ./cmd/aimee-module)" >&2; exit 2; }

echo "== packaging =="
PLUGGY=""
if [ -d src/build/pluggylib ]; then
  PLUGGY="src/build/pluggylib src/tests/fixtures/pluggy"
else
  echo "note: src/build/pluggylib absent -- the pluggy leg will be skipped" >&2
fi
tar czf "$PKG" "$SRV" "$MOD" scripts/provision-plugin-module.py scripts/aimee-pluggy-host.py \
  --transform='s|^src/build/pluggylib|pluggylib|' \
  --transform='s|^src/tests/fixtures/pluggy|fixtures|' \
  $PLUGGY

echo "== shipping =="
scp -q "$PKG" "$HOST:/tmp/live-pkg.tgz"
scp -q scripts/live-plugin-validation-remote.sh "$HOST:/tmp/live-remote.sh"
ssh "$HOST" "sh /tmp/live-remote.sh"
rm -f "$PKG"
