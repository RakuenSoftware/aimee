#!/bin/bash
# Run the DB2 process replay against a real Postgres, creating the container it
# needs if it is not there.
#
# The replay is the only test that proves a DB2 statement parses and runs: the
# unit suites drive stub backends, so a column that does not exist or a function
# that was never created passes them and fails here. It therefore has to be easy
# to run, and it has not been -- the container it uses has been deleted three
# times, and each time the replay silently stopped being run until someone
# noticed. This script rebuilds it rather than failing.
#
# Postgres runs natively inside an unprivileged LXC container. Nothing is
# installed on the hypervisor.
#
#   scripts/db2_replay_env.sh
#
# The replay is NOT re-runnable against a used database: memory.health_record is
# declared unsafe precisely because it appends a row per call, so the
# health-counters assertion fails on a second run against the same database.
# The database is therefore dropped and recreated every time.
set -uo pipefail

PVE=${DB2_REPLAY_PVE:-root@192.168.1.252}
CT=${DB2_REPLAY_CT:-9001}
DB=${DB2_REPLAY_DB:-aimee_db2_process_ci}
TEMPLATE=${DB2_REPLAY_TEMPLATE:-local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst}

say() { printf 'db2-replay: %s\n' "$1" >&2; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PVE" "$@"; }

if ! on_host true 2>/dev/null; then
   say "cannot reach $PVE over ssh"
   exit 1
fi

if ! on_host "pct status $CT" >/dev/null 2>&1; then
   say "container $CT is missing; creating it"
   on_host "pct create $CT $TEMPLATE --hostname db2-replay --cores 4 --memory 4096 \
      --rootfs local-lvm:16 --net0 name=eth0,bridge=vmbr0,ip=dhcp --unprivileged 1 \
      --features nesting=1 --onboot 0" >/dev/null 2>&1 || { say "create failed"; exit 1; }
fi

if [ "$(on_host "pct status $CT" 2>/dev/null)" != "status: running" ]; then
   on_host "pct start $CT" >/dev/null 2>&1
   # The container needs an address before anything can be installed into it.
   for _ in $(seq 1 30); do
      on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null | grep -q inet && break
      sleep 2
   done
fi

if ! on_host "pct exec $CT -- test -x /usr/lib/postgresql/17/bin/postgres" 2>/dev/null; then
   say "installing postgres and pgvector"
   on_host "pct exec $CT -- bash -lc 'export DEBIAN_FRONTEND=noninteractive; \
      apt-get -qq update && apt-get -qq -y install postgresql postgresql-17-pgvector'" \
      >/dev/null 2>&1 || { say "install failed"; exit 1; }
   # Listen on the bridge and trust the local network: this container holds
   # throwaway test data and is recreated whenever it goes missing.
   on_host "pct exec $CT -- bash -lc \"sed -i \\\"s/^#listen_addresses.*/listen_addresses = '*'/\\\" \
      /etc/postgresql/17/main/postgresql.conf; \
      grep -q '192.168.0.0/23 trust' /etc/postgresql/17/main/pg_hba.conf || \
      echo 'host all all 192.168.0.0/23 trust' >> /etc/postgresql/17/main/pg_hba.conf; \
      systemctl restart postgresql\"" >/dev/null 2>&1
fi

# Everything below runs SQL from a file inside the container. Sending it as
# psql -c through ssh, pct exec and su means four levels of quoting, which is
# how the role creation was silently lost the first time this script ran.
run_sql() {
   local database=$1 sql=$2
   on_host "pct exec $CT -- bash -c 'cat > /tmp/db2-replay.sql'" <<<"$sql" || return 1
   on_host "pct exec $CT -- su postgres -c 'psql -XAt -v ON_ERROR_STOP=1 -d $database \
      -f /tmp/db2-replay.sql'"
}

if ! run_sql postgres "SELECT 1 FROM pg_roles WHERE rolname = 'aimee'" | grep -q 1; then
   say "creating the aimee role"
   run_sql postgres "CREATE ROLE aimee LOGIN SUPERUSER PASSWORD 'aimee'" >/dev/null ||
      { say "could not create the aimee role"; exit 1; }
fi

DBHOST=$(on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null |
   sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)
if [ -z "$DBHOST" ]; then
   say "container $CT has no address"
   exit 1
fi
say "using postgres at $DBHOST"

# Anything still holding a connection blocks the drop; the replay leaves its
# module running if it aborted.
pkill -f aimee-module-db2-replay 2>/dev/null
pkill -f unit-test-bus-db2-process 2>/dev/null

run_sql postgres "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null ||
   { say "could not drop $DB"; exit 1; }
run_sql postgres "CREATE DATABASE $DB OWNER aimee" >/dev/null ||
   { say "could not create $DB"; exit 1; }
run_sql "$DB" "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" \
   >/dev/null || { say "could not create the extensions $DB needs"; exit 1; }

cd "$(dirname "$0")/.." || exit 1
AIMEE_DB2_URL="postgres://aimee:aimee@$DBHOST:5432/$DB" \
   EMBEDDER_DIMS=384 \
   make -C src -j8 GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 db2-replay
