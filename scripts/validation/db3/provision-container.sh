#!/bin/bash
# Build a container that can run the DB3 verification, on a Proxmox host.
#
# Run this ON the Proxmox host. It makes an unprivileged Debian 13 CT with a
# local PostgreSQL 17 + pgvector, the C toolchain, and Go -- everything the
# suites below need, and nothing that talks to a production database.
#
# The point of a container rather than the developer's machine is that the
# unit suite against real Postgres needs a server it may create and drop
# databases on. Pointing it at a shared instance is how a verification run
# becomes an outage.
#
#   ./provision-container.sh [VMID] [HOSTNAME]
set -euo pipefail

CT="${1:-9100}"
NAME="${2:-aimee-db3-verify}"
TEMPLATE="${TEMPLATE:-local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst}"
STORAGE="${STORAGE:-local-lvm}"
BRIDGE="${BRIDGE:-vmbr0}"

if pct status "$CT" >/dev/null 2>&1; then
   echo "CT $CT exists; stopping and destroying it"
   pct stop "$CT" >/dev/null 2>&1 || true
   pct destroy "$CT" --force 1 >/dev/null 2>&1 || true
fi

# 32G because a full build tree plus 700-odd linked test binaries is several GB,
# and 12G because the suite links with -flto at -j$(nproc).
pct create "$CT" "$TEMPLATE" \
   --hostname "$NAME" --cores 6 --memory 12288 --swap 2048 \
   --rootfs "$STORAGE:32" --net0 "name=eth0,bridge=$BRIDGE,ip=dhcp" \
   --features nesting=1 --unprivileged 1 --onboot 0 >/dev/null
pct start "$CT"

address=""
for _ in $(seq 1 30); do
   address=$(pct exec "$CT" -- ip -4 -br a show eth0 2>/dev/null | awk '{print $3}')
   [ -n "$address" ] && break
   sleep 2
done
echo "CT $CT ($NAME) at ${address:-<no address>}"

# libsqlite3-dev is not optional even though this run targets Postgres: the DB2
# sources compile against the sqlite shim's header regardless of which backend
# the binaries link, so its absence fails the build rather than the tests.
pct exec "$CT" -- bash -lc '
   export DEBIAN_FRONTEND=noninteractive
   apt-get update -q >/dev/null
   apt-get install -y -q \
      gcc make pkg-config git rsync ca-certificates curl python3 \
      libssl-dev libpq-dev libpam0g-dev libsqlite3-dev \
      zlib1g-dev libzstd-dev \
      postgresql-17 postgresql-17-pgvector \
      clang-format-19 golang-go >/dev/null
' >/dev/null
pct exec "$CT" -- mkdir -p /work/aimee /work/verify
echo "provisioned. Next: deploy the tree to /work/aimee, then run pg-setup.sh."
