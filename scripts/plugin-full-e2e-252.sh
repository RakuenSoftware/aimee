#!/bin/sh
# Full end-to-end for plugin modules on 192.168.1.252, in two legs.
#
#   server leg  -- runs on the .252 HOST. Needs a real aimee-server and nothing
#                  else, so it always runs.
#   kb leg      -- runs INSIDE an LXC container with PostgreSQL, because a real
#                  aimee-kb needs a real DB2. Pass CT=<vmid> to run it.
#
# To make a container for the kb leg (about two minutes):
#
#   pct create <vmid> /var/lib/vz/template/cache/debian-13-standard_13.6-1_amd64.tar.zst \
#       --hostname aimee-plugin-e2e --cores 8 --memory 8192 --swap 2048 \
#       --rootfs local-lvm:24 --net0 name=eth0,bridge=vmbr0,ip=dhcp \
#       --unprivileged 1 --features nesting=1 --onboot 0 --start 1
#   pct push <vmid> scripts/plugin-e2e-ctprep.sh /tmp/ctprep.sh --perms 755
#   pct exec <vmid> -- sh /tmp/ctprep.sh        # postgres 17 + pgvector + python3
#   aimee-keepalive ct:<vmid>                   # or it is reaped 4h from creation
#
# THE LAST LINE IS NOT OPTIONAL FOR A LONG RUN. The host reaps a guest 4h after
# creation or its last renewal, and aimee-keepalive slides the lease a full TTL
# from now with no cap on renewals. Nothing in this repo called it until now, so
# environments kept dying at the four hour mark and it was read as the host
# being unstable. Renew every couple of hours on a long session; the reaper's
# own log names the container and the overrun when it takes one.
#
# It also applies a SECOND clock that renewing does not answer: a guest with no
# measurable activity for a full TTL is reaped however recently it was leased.
# So a container held "in case it is wanted later" is meant to die.
#
# and `pct stop <vmid> && pct destroy <vmid>` when done. Pick a VMID clear of
# whatever else manages containers on the host.
#
# The kb leg is the one that matters for event-kind allocation: it is the only
# configuration where the real `postgres` module is attached, and that is what
# exposed the plugin range colliding with postgres's kind block. The server leg
# alone cannot see that class of bug -- a scratch host runs no other modules.
#
# Scope discipline: the server leg lives under /tmp/al2 and removes it; the kb
# leg lives under /opt/plive and the scratch database aimee_plive. .252 hosts
# live deployments in OTHER containers; nothing here touches them, and no
# blanket pkill is used.
#
# Usage:
#   sh scripts/plugin-full-e2e-252.sh              # server leg only
#   CT=101 sh scripts/plugin-full-e2e-252.sh       # both legs
set -eu

HOST="${HOST:-root@192.168.1.252}"
CT="${CT:-}"
PKG=src/build/plugin-e2e-pkg.tgz

SRV=aimee-server
KB=aimee-kb
MOD=src/build/obj/aimee-module

[ -x "$SRV" ] || { echo "missing ./$SRV -- run: make -C src server" >&2; exit 2; }
[ -x "$MOD" ] || { echo "missing $MOD -- run: (cd server-go && go build -o ../$MOD ./cmd/aimee-module)" >&2; exit 2; }
if [ -n "$CT" ] && [ ! -x "$KB" ]; then
  echo "missing ./$KB (needed for the kb leg) -- run: make -C src kb" >&2; exit 2
fi

echo "== packaging =="
PLUGGY=""
if [ -d src/build/pluggylib ]; then
  PLUGGY="src/build/pluggylib src/tests/fixtures/pluggy"
else
  echo "note: src/build/pluggylib absent -- the pluggy leg will be skipped" >&2
fi
KBBIN=""
[ -n "$CT" ] && KBBIN="$KB aimee-kb-resolver"
# shellcheck disable=SC2086
tar czf "$PKG" $SRV $KBBIN "$MOD" \
  scripts/provision-plugin-module.py scripts/aimee-pluggy-host.py \
  --transform='s|^src/build/obj/||' \
  --transform='s|^scripts/||' \
  --transform='s|^src/build/pluggylib|pluggylib|' \
  --transform='s|^src/tests/fixtures/pluggy|fixtures|' \
  $PLUGGY

echo "== shipping =="
scp -q "$PKG" "$HOST:/tmp/pkg.tgz"
scp -q scripts/plugin-full-e2e-server-remote.sh "$HOST:/tmp/server-remote.sh"

rc=0

echo
echo "########## SERVER LEG (on the .252 host) ##########"
ssh "$HOST" "sh /tmp/server-remote.sh" || rc=$?

if [ -n "$CT" ]; then
  echo
  echo "########## KB LEG (in container $CT) ##########"
  scp -q scripts/plugin-full-e2e-kb-remote.sh "$HOST:/tmp/kb-remote.sh"
  scp -q scripts/plugin-e2e-pgsetup.sh "$HOST:/tmp/pgsetup.sh"
  ssh "$HOST" "
    pct push $CT /tmp/pkg.tgz /tmp/pkg.tgz &&
    pct push $CT /tmp/pgsetup.sh /tmp/pgsetup.sh --perms 755 &&
    pct push $CT /tmp/kb-remote.sh /tmp/kb-remote.sh --perms 755 &&
    pct exec $CT -- sh -c 'rm -rf /opt/plive && mkdir -p /opt/plive &&
                           tar xzf /tmp/pkg.tgz -C /opt/plive &&
                           chmod +x /opt/plive/aimee-server /opt/plive/aimee-kb /opt/plive/aimee-module' &&
    pct exec $CT -- sh /tmp/pgsetup.sh &&
    pct exec $CT -- sh /tmp/kb-remote.sh
  " || rc=$?
else
  echo
  echo "NOTE: kb leg skipped. A real aimee-kb needs a real PostgreSQL, so it runs"
  echo "      in an LXC container. Re-run with CT=<vmid> once one exists."
fi

rm -f "$PKG"
exit "$rc"
