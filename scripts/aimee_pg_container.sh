#!/bin/bash
# aimee_pg_container.sh — ensure a PostgreSQL container exists, and print its address.
#
# This host reaps containers. The replay environment's docstring records its
# container being deleted three times; the parity container has now gone three
# times in one session, twice mid-run. So "the container is there" is not an
# assumption anything may make, and every script that needs PostgreSQL has to
# be able to rebuild it.
#
# Written once rather than three times. It was already duplicated between
# db2_parity_env.sh and db2_replay_env.sh, and I was about to write a third
# copy for a live test, which is the point at which a shared script stops being
# premature.
#
#   eval "$(scripts/aimee_pg_container.sh)"    # exports AIMEE_PG_HOST
#   scripts/aimee_pg_container.sh --host       # prints the address alone
#
# Everything runs inside the container; nothing is installed on the hypervisor.
# That is a standing constraint here, not a preference: a package installed on
# the hypervisor is outside any container's lifecycle and cannot be torn down
# with one.
set -uo pipefail

PVE=${AIMEE_PG_PVE:-root@192.168.1.252}
CT=${AIMEE_PG_CT:-9002}
TEMPLATE=${AIMEE_PG_TEMPLATE:-local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst}
PG_MAJOR=17
# Pinned to the release the image uses, read from the Dockerfile so the two
# cannot drift apart silently.
VECTORSCALE=$(sed -n 's/^ARG PGVECTORSCALE_VERSION=\([0-9.]*\)/\1/p' \
    "$(dirname "$0")/../Dockerfile" | head -1)
VECTORSCALE=${VECTORSCALE:-0.9.0}

say() { printf 'aimee-pg: %s\n' "$1" >&2; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PVE" "$@"; }
in_ct() { on_host "pct exec $CT -- bash -s"; }

if ! on_host true 2>/dev/null; then
   say "cannot reach $PVE over ssh"
   exit 1
fi

if ! on_host "pct status $CT" >/dev/null 2>&1; then
   say "container $CT is missing; creating it"
   on_host "pct create $CT $TEMPLATE --hostname aimee-pg --cores 8 \
      --memory 8192 --swap 2048 --rootfs local-lvm:48 \
      --net0 name=eth0,bridge=vmbr0,ip=dhcp --unprivileged 1 \
      --features nesting=1 --onboot 0" >/dev/null 2>&1 ||
      { say "create failed"; exit 1; }
fi
if [ "$(on_host "pct status $CT" 2>/dev/null)" != "status: running" ]; then
   on_host "pct start $CT" >/dev/null 2>&1
fi

# An address has to exist before anything can be installed.
for _ in $(seq 1 30); do
   on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null | grep -q inet && break
   sleep 2
done
HOST=$(on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null |
   sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)
if [ -z "$HOST" ]; then
   say "container $CT has no address"
   exit 1
fi

if ! on_host "pct exec $CT -- test -x /usr/lib/postgresql/$PG_MAJOR/bin/postgres" 2>/dev/null; then
   say "installing postgres and pgvector"
   cat <<PKGS | in_ct >/dev/null 2>&1 || { say "package install failed"; exit 1; }
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq -y install postgresql postgresql-$PG_MAJOR-pgvector build-essential \\
   ca-certificates libpq-dev libsqlite3-dev libssl-dev libzstd-dev pkg-config \\
   python3 zlib1g-dev git rsync curl jq openssl procps unzip
PKGS
   # Listen on the bridge and trust the local network: this container holds
   # throwaway test data and is rebuilt whenever it goes missing.
   cat <<PGCONF | in_ct >/dev/null 2>&1
set -eu
sed -i "s/^#listen_addresses.*/listen_addresses = '*'/" \\
   /etc/postgresql/$PG_MAJOR/main/postgresql.conf
grep -q '192.168.0.0/23 trust' /etc/postgresql/$PG_MAJOR/main/pg_hba.conf ||
   echo 'host all all 192.168.0.0/23 trust' >> /etc/postgresql/$PG_MAJOR/main/pg_hba.conf
systemctl restart postgresql
PGCONF
fi

# pgvectorscale. The schema requires it -- diskann is the only index method over
# the embedding columns -- so a container without it cannot apply the schema.
cat <<PGVS | in_ct >/dev/null 2>&1 || { say "pgvectorscale install failed"; exit 1; }
set -eu
if ls /usr/lib/postgresql/$PG_MAJOR/lib/vectorscale*.so >/dev/null 2>&1; then exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get -qq -y install curl unzip >/dev/null
cd /tmp
curl -fsSL -o pgvs.zip \\
  https://github.com/timescale/pgvectorscale/releases/download/$VECTORSCALE/pgvectorscale-$VECTORSCALE-pg$PG_MAJOR-amd64.zip
unzip -p pgvs.zip 'pgvectorscale-postgresql-$PG_MAJOR'_'$VECTORSCALE-Linux_amd64.deb' > pgvs.deb
dpkg -i pgvs.deb
PGVS

cat <<ROLE | in_ct >/dev/null 2>&1 || { say "could not create the aimee role"; exit 1; }
set -eu
su postgres -c "psql -XAt -c \\"SELECT 1 FROM pg_roles WHERE rolname='aimee'\\"" | grep -q 1 ||
   su postgres -c "psql -XAt -c \\"CREATE ROLE aimee LOGIN SUPERUSER PASSWORD 'aimee'\\""
ROLE

# UTF8, not the cluster default. A database on SQL_ASCII cannot detect a whole
# class of defect: char_length and octet_length are identical there, so a
# character-counted limit standing in for a byte one passes every test and
# fails in production, which is initdb --encoding=UTF8.
DB=${AIMEE_PG_DB:-aimee_live}
cat <<SETUP | in_ct >/dev/null 2>&1 || { say "could not create $DB"; exit 1; }
set -eu
su postgres -c "psql -XAt -c \\"SELECT 1 FROM pg_database WHERE datname='$DB'\\"" | grep -q 1 ||
   su postgres -c "psql -XAt -c \\"CREATE DATABASE $DB OWNER aimee ENCODING UTF8 TEMPLATE template0\\""
su postgres -c "psql -XAt -d $DB -c 'CREATE EXTENSION IF NOT EXISTS vector'"
su postgres -c "psql -XAt -d $DB -c 'CREATE EXTENSION IF NOT EXISTS vectorscale'"
su postgres -c "psql -XAt -d $DB -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm'"
SETUP

# Renew the lease. The reaper reclaims a guest 4h after creation or its last
# renewal, and nothing here ever renewed -- which is why this container has been
# destroyed three times in one session, twice mid-run. Renewing on every ensure
# buys a fresh 4h, and the host's own guidance is to renew on a comfortable
# margin rather than at the deadline.
#
# This does NOT make the container immortal, and should not: the reaper also
# requires measurable activity, so an idle guest is reclaimed however recently
# it was leased. A container kept only in case it is wanted later is meant to
# die.
on_host "aimee-keepalive ct:$CT" >/dev/null 2>&1 ||
   say "could not renew the lease; this container will be reaped 4h after it was created"

say "container $CT ready at $HOST, database $DB"
if [ "${1:-}" = "--host" ]; then
   printf '%s\n' "$HOST"
else
   printf 'export AIMEE_PG_HOST=%s\n' "$HOST"
   printf 'export AIMEE_DB2_URL=postgres://aimee:aimee@%s:5432/%s\n' "$HOST" "$DB"
fi
