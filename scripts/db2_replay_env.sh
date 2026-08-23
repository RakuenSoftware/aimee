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
   say "installing postgres, pgvector and pgvectorscale"
   on_host "pct exec $CT -- bash -lc 'export DEBIAN_FRONTEND=noninteractive; \
      apt-get -qq update && apt-get -qq -y install postgresql postgresql-17-pgvector'" \
      >/dev/null 2>&1 || { say "install failed"; exit 1; }
   # pgvectorscale, pinned to the release the image uses. The schema requires it
   # -- diskann is the only index method over the embedding columns -- so an
   # environment without it cannot apply the schema, never mind compare
   # anything. Upstream ships a built .deb per (version, pg major, arch); the
   # container's postgres is 17.
   say "installing pgvectorscale 0.9.0"
   cat <<'PGVS' | on_host "pct exec $CT -- bash -s" >/dev/null 2>&1 || { say "pgvectorscale install failed"; exit 1; }
set -eu
if ls /usr/lib/postgresql/17/lib/vectorscale*.so >/dev/null 2>&1; then exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get -qq -y install unzip >/dev/null
cd /tmp
curl -fsSL -o pgvs.zip \
  https://github.com/timescale/pgvectorscale/releases/download/0.9.0/pgvectorscale-0.9.0-pg17-amd64.zip
unzip -p pgvs.zip 'pgvectorscale-postgresql-17_0.9.0-Linux_amd64.deb' > pgvs.deb
dpkg -i pgvs.deb
ls /usr/lib/postgresql/17/lib/vectorscale*.so
PGVS
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
run_sql "$DB" "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS vectorscale; CREATE EXTENSION IF NOT EXISTS pg_trgm" \
   >/dev/null || { say "could not create the extensions $DB needs"; exit 1; }

cd "$(dirname "$0")/.." || exit 1

# Both the pre-applied schema and the replay itself must agree on this.
EMBED_DIM=384

# Copy a local SQL file into the container and run it there, for anything too
# large to pass through run_sql's heredoc.
run_file() {
   local database=$1 path=$2
   # schema.sql carries the __EMBED_DIM__ placeholder that db_schema.c fills in
   # at runtime, so it is not valid SQL until the dimension is substituted. The
   # value has to match the EMBEDDER_DIMS the replay runs with, or the module
   # would find a vector of the wrong width already there.
   sed "s/__EMBED_DIM__/$EMBED_DIM/g" "$path" |
      on_host "pct exec $CT -- bash -c 'cat > /tmp/db2-replay-file.sql'" || return 1
   on_host "pct exec $CT -- su postgres -c 'psql -XAt -v ON_ERROR_STOP=1 -d $database \
      -f /tmp/db2-replay-file.sql'"
}

# The replay's module applies the schema itself on connect, so the database
# only has to exist. It is applied here as well, first, because some replay
# cases need a row to exist before the module starts: on a fresh schema every
# read answers zero and every write against a missing row is refused, and
# neither distinguishes a working operation from a broken one.
say "applying the schema so the replay can be seeded"
run_file "$DB" src/modules/db2/c/schema.sql >/dev/null ||
   { say "could not apply the schema to $DB"; exit 1; }

# Two artifacts. The first is committed and carries a payload of its own, for
# the flag-review replay to merge into and for the assertion afterwards to read
# back. Both ends of a link are foreign keys and a citation's citing end is
# one, so the cite and link replays need real rows or they can only ever show a
# refusal.
run_sql "$DB" "INSERT INTO artifacts (id, kind, state, payload)
   VALUES ('replay-flag-probe', 'probe', 'committed', '{\"kept\": 1}'::jsonb),
          ('replay-link-target', 'probe', 'proposed', '{}'::jsonb)
   ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, payload = EXCLUDED.payload" \
   >/dev/null || { say "could not seed the artifact fixtures"; exit 1; }

AIMEE_DB2_URL="postgres://aimee:aimee@$DBHOST:5432/$DB" \
   EMBEDDER_DIMS=$EMBED_DIM \
   make -C src -j8 GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 db2-replay || exit 1

# What the replay cannot assert for itself: the flagged artifact came back with
# its own payload intact beside the flag, and its state reset. A merge that
# dropped the existing keys, and a write that never happened, both satisfy the
# replay's assertion on the reply and fail here.
say "checking what the flag-review replay wrote"
flagged=$(run_sql "$DB" "SELECT state
   || ' kept=' || COALESCE((payload->>'kept'), 'missing')
   || ' flagged=' || COALESCE((payload->>'flagged_for_review'), 'missing')
   || ' reason=' || COALESCE((payload->>'flagged_reason'), 'missing')
   FROM artifacts WHERE id = 'replay-flag-probe'")
if [ "$flagged" != "proposed kept=1 flagged=true reason=replayed" ]; then
   say "flag-review left the artifact as: ${flagged:-<no row>}"
   exit 1
fi
say "flag-review merged the flag into the artifact's own payload"
