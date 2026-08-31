#!/bin/bash
# Create and provision the validation container from nothing. Run ON THE .252
# PROXMOX HOST.
#
# The reaper (/root/AGENTS.md) applies two 4h clocks to every guest and destroys
# on EITHER: a lease clock reset by `aimee-keepalive`, and a liveness clock reset
# only by the guest doing measurable work. The first build of this environment
# was reaped mid-run because it was never leased -- so the lease is taken here,
# immediately after creation, before anything else can go wrong. Renewing it for
# the duration is the caller's job (see keepalive-loop.sh).
#
# Usage: provision-host.sh [CTID]
set -u
CTID="${1:-9078}"
TEMPLATE=local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst

pct status "$CTID" >/dev/null 2>&1 && { echo "CT $CTID already exists" >&2; exit 1; }

pct create "$CTID" "$TEMPLATE" \
  --hostname aimee-fact-authority --cores 4 --memory 6144 --swap 512 \
  --rootfs local-lvm:24 --net0 name=eth0,bridge=vmbr0,ip=dhcp \
  --unprivileged 1 --features nesting=1 --onboot 0 >/dev/null

# Lease FIRST. Everything below takes minutes, and an unleased guest is already
# on a countdown that no amount of later work resets.
aimee-keepalive "ct:${CTID}" || echo "WARNING: could not take a lease" >&2

pct start "$CTID"
sleep 10
IP="$(pct exec "$CTID" -- bash -lc "ip -4 addr show eth0 | sed -n 's/.*inet \([0-9.]*\).*/\1/p'" | head -1)"
echo "CT ${CTID} up at ${IP}"

pct exec "$CTID" -- bash -lc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y -qq postgresql postgresql-17-pgvector libpq5 libssl3 zlib1g \
                         libzstd1 libpam0g curl jq sqlite3 python3 >/dev/null 2>&1
  systemctl is-active postgresql
' 2>&1 | tail -1

pct exec "$CTID" -- su - postgres -c \
  "psql -qc \"CREATE ROLE aimee LOGIN SUPERUSER PASSWORD 'aimee-e2e';\" \
        -qc 'CREATE DATABASE aimee_shared OWNER aimee;'" >/dev/null 2>&1
pct exec "$CTID" -- su - postgres -c \
  "psql -q -d aimee_shared -c 'CREATE EXTENSION IF NOT EXISTS vector;'" >/dev/null 2>&1
echo "postgres ready"
echo "${IP}"
