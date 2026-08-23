#!/bin/bash
# db2_parity_env.sh — run the C and Go DB2 implementations against identically
# seeded databases and compare what they answer.
#
# The unit suites drive fakes and the live probes prove a statement runs.
# Neither answers the question this does: given the identical request bytes,
# does the Go module say what the C module says?
#
# It works by recording rather than restating. The C replay
# (src/tests/test_bus_db2_process.c) already issues every catalogued operation
# with hand-written arguments; with AIMEE_DB2_PARITY_TRACE set it appends the
# request it made and the reply it got for each call. The Go side is then handed
# those exact request bytes. A second copy of the arguments written in Go would
# drift from the C ones silently, and two implementations agreeing about calls
# neither makes is not parity.
#
# Both sides get their own database, created and seeded the same way in the same
# order, so a divergence belongs to the operation and not to the fixture.
#
#   scripts/db2_parity_env.sh
#
# The container this needs is built if it is missing. That is not a
# convenience: this host reaps containers -- the replay environment's own
# docstring records it being deleted three times, and both the replay container
# and this one were destroyed mid-session while the work was running. A parity
# run that cannot rebuild its own environment is a parity run nobody can repeat.
#
# Everything runs inside the container; nothing is installed on the hypervisor.
set -uo pipefail

PVE=${DB2_PARITY_PVE:-root@192.168.1.252}
CT=${DB2_PARITY_CT:-9002}
DB_C=${DB2_PARITY_DB_C:-aimee_db2_parity_c}
DB_GO=${DB2_PARITY_DB_GO:-aimee_db2_parity_go}
TEMPLATE=${DB2_PARITY_TEMPLATE:-local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst}
GO_VERSION=${DB2_PARITY_GO:-1.25.0}
EMBED_DIM=384

say() { printf 'db2-parity: %s\n' "$1" >&2; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PVE" "$@"; }
in_ct() { on_host "pct exec $CT -- bash -s"; }

cd "$(dirname "$0")/.." || exit 1
REPO="$(pwd)"

if ! on_host true 2>/dev/null; then
   say "cannot reach $PVE over ssh"
   exit 1
fi

# --- the container ---------------------------------------------------------
if ! on_host "pct status $CT" >/dev/null 2>&1; then
   say "container $CT is missing; creating it"
   on_host "pct create $CT $TEMPLATE --hostname aimee-parity --cores 8 \
      --memory 8192 --swap 2048 --rootfs local-lvm:48 \
      --net0 name=eth0,bridge=vmbr0,ip=dhcp --unprivileged 1 \
      --features nesting=1 --onboot 0" >/dev/null 2>&1 ||
      { say "create failed"; exit 1; }
fi
if [ "$(on_host "pct status $CT" 2>/dev/null)" != "status: running" ]; then
   on_host "pct start $CT" >/dev/null 2>&1
fi
# The container needs an address before anything can be installed into it.
for _ in $(seq 1 30); do
   on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null | grep -q inet && break
   sleep 2
done
DBHOST=$(on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null |
   sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)
if [ -z "$DBHOST" ]; then
   say "container $CT has no address"
   exit 1
fi
say "container $CT is at $DBHOST"

# --- what the build and the databases need ---------------------------------
if ! on_host "pct exec $CT -- test -x /usr/lib/postgresql/17/bin/postgres" 2>/dev/null; then
   say "installing postgres, pgvector and the build toolchain"
   cat <<'PKGS' | in_ct >/dev/null 2>&1 || { say "package install failed"; exit 1; }
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq -y install postgresql postgresql-17-pgvector build-essential \
   ca-certificates libpq-dev libsqlite3-dev libssl-dev libzstd-dev pkg-config \
   python3 zlib1g-dev git rsync curl jq openssl procps
PKGS
   # pgvectorscale, pinned to the release the image uses. The schema requires it
   # -- diskann is the only index method over the embedding columns -- so an
   # environment without it cannot apply the schema, never mind compare
   # anything. Upstream ships a built .deb per (version, pg major, arch); the
   # container's postgres is 17.
   say "installing pgvectorscale 0.9.0"
   cat <<'PGVS' | in_ct >/dev/null 2>&1 || { say "pgvectorscale install failed"; exit 1; }
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
   # throwaway comparison data and is rebuilt whenever it goes missing.
   cat <<'PGCONF' | in_ct >/dev/null 2>&1
set -eu
sed -i "s/^#listen_addresses.*/listen_addresses = '*'/" \
   /etc/postgresql/17/main/postgresql.conf
grep -q '192.168.0.0/23 trust' /etc/postgresql/17/main/pg_hba.conf ||
   echo 'host all all 192.168.0.0/23 trust' >> /etc/postgresql/17/main/pg_hba.conf
systemctl restart postgresql
PGCONF
fi
cat <<'ROLE' | in_ct >/dev/null 2>&1 || { say "could not create the aimee role"; exit 1; }
set -eu
su postgres -c "psql -XAt -c \"SELECT 1 FROM pg_roles WHERE rolname='aimee'\"" | grep -q 1 ||
   su postgres -c "psql -XAt -c \"CREATE ROLE aimee LOGIN SUPERUSER PASSWORD 'aimee'\""
ROLE

if ! on_host "pct exec $CT -- test -x /usr/local/go/bin/go" 2>/dev/null; then
   say "installing go $GO_VERSION"
   cat <<GO | in_ct >/dev/null 2>&1 || { say "go install failed"; exit 1; }
set -eu
curl -fsSL -o /tmp/go.tgz https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz
rm -rf /usr/local/go
tar -C /usr/local -xzf /tmp/go.tgz
GO
fi

# --- the source ------------------------------------------------------------
# Copied rather than cloned: what is compared has to be the working tree, not
# whatever a remote happens to hold.
say "copying the working tree into the container"
BUNDLE=$(mktemp -t db2-parity-src-XXXXXX.tgz)
trap 'rm -f "$BUNDLE"' EXIT
tar -C "$REPO" -czf "$BUNDLE" --exclude=.git --exclude=src/build \
   --exclude=.db2scratch --exclude=node_modules . 2>/dev/null
scp -q -o BatchMode=yes "$BUNDLE" "$PVE:/tmp/db2-parity-src.tgz" ||
   { say "could not copy the source to the hypervisor"; exit 1; }
on_host "pct push $CT /tmp/db2-parity-src.tgz /tmp/db2-parity-src.tgz" ||
   { say "could not push the source into the container"; exit 1; }
cat <<'UNPACK' | in_ct >/dev/null || { say "could not unpack the source"; exit 1; }
set -eu
rm -rf /opt/aimee-src
mkdir -p /opt/aimee-src
tar -C /opt/aimee-src -xzf /tmp/db2-parity-src.tgz
UNPACK

# --- the databases ---------------------------------------------------------
# Both are dropped and recreated every run. The replay is not re-runnable
# against a used database -- memory.health_record appends a row per call, so its
# counter assertion fails on a second run -- and a parity run against two
# databases in different states would compare fixtures rather than
# implementations.
say "recreating $DB_C and $DB_GO"
cat <<SETUP | in_ct >/dev/null || { say "could not recreate the databases"; exit 1; }
set -eu
for db in $DB_C $DB_GO; do
  su postgres -c "psql -XAt -c \"DROP DATABASE IF EXISTS \$db WITH (FORCE)\""
  su postgres -c "psql -XAt -c \"CREATE DATABASE \$db OWNER aimee\""
  su postgres -c "psql -XAt -d \$db -c 'CREATE EXTENSION IF NOT EXISTS vector'"
  su postgres -c "psql -XAt -d \$db -c 'CREATE EXTENSION IF NOT EXISTS vectorscale'"
  su postgres -c "psql -XAt -d \$db -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm'"
done
SETUP

# The schema is applied before either side starts, for the reason the replay
# environment gives: on a fresh schema every read answers zero and every write
# against a missing row is refused, and neither distinguishes a working
# operation from a broken one.
#
# The fixtures go into a file rather than through psql -c. Sending SQL as a
# command string crosses ssh, pct exec, su and psql, which is four levels of
# quoting for one JSON literal -- and the backslashes arrive at Postgres as
# data, which is how the first version of this failed with "Token \ is
# invalid".
say "applying the schema and the fixtures to both"
cat <<APPLY | in_ct >/dev/null || { say "could not stage the schema"; exit 1; }
set -eu
sed "s/__EMBED_DIM__/$EMBED_DIM/g" \
   /opt/aimee-src/src/modules/db2/c/schema.sql > /tmp/parity-schema.sql
cat > /tmp/parity-fixture.sql <<'SQL'
INSERT INTO artifacts (id, kind, state, payload) VALUES
  ('replay-flag-probe', 'probe', 'committed', '{"kept": 1}'::jsonb),
  ('replay-link-target', 'probe', 'proposed', '{}'::jsonb)
ON CONFLICT (id) DO UPDATE
  SET state = EXCLUDED.state, payload = EXCLUDED.payload;
SQL
APPLY
for db in "$DB_C" "$DB_GO"; do
   cat <<APPLY | in_ct >/dev/null || { say "could not apply the schema to $db"; exit 1; }
set -eu
su postgres -c "psql -XAtq -v ON_ERROR_STOP=1 -d $db -f /tmp/parity-schema.sql"
su postgres -c "psql -XAtq -v ON_ERROR_STOP=1 -d $db -f /tmp/parity-fixture.sql"
APPLY
done

# --- the C side, recording every call it makes -----------------------------
say "running the C replay against $DB_C"
cat <<REPLAY | in_ct >/tmp/db2-parity-c.log 2>&1
set -u
export PATH=/usr/local/go/bin:\$PATH
export AIMEE_DB2_URL="postgres://aimee:aimee@127.0.0.1:5432/$DB_C"
export EMBEDDER_DIMS=$EMBED_DIM
export AIMEE_DB2_PARITY_TRACE=/tmp/db2-parity-trace.txt
rm -f /tmp/db2-parity-trace.txt
cd /opt/aimee-src
ulimit -S -s 65536 || true
make -C src -j8 GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 db2-replay
echo "C-REPLAY-EXIT=\$?"
wc -l /tmp/db2-parity-trace.txt
REPLAY
tail -3 /tmp/db2-parity-c.log
grep -q 'C-REPLAY-EXIT=0' /tmp/db2-parity-c.log ||
   { say "the C replay failed; see /tmp/db2-parity-c.log"; exit 1; }

# --- the Go side, handed the C's requests ----------------------------------
say "replaying the trace through the Go module against $DB_GO"
cat <<PARITY | in_ct >/tmp/db2-parity-go.log 2>&1
set -u
export PATH=/usr/local/go/bin:\$PATH
export GOFLAGS=-buildvcs=false
export AIMEE_DB2_URL="postgres://aimee:aimee@127.0.0.1:5432/$DB_GO"
export AIMEE_DB2_PARITY_TRACE=/tmp/db2-parity-trace.txt
export AIMEE_DB2_PARITY_REPORT=/tmp/db2-parity-report.tsv
rm -f \$AIMEE_DB2_PARITY_REPORT
cd /opt/aimee-src/server-go
go test ./modules/db2/ -count=1 -run TestParityWithTheCReplay -v 2>&1 | tail -40
echo "GO-PARITY-EXIT=\${PIPESTATUS[0]}"
cd /opt/aimee-src
python3 scripts/db2_parity_triage.py /tmp/db2-parity-report.tsv 2>&1 | head -80
PARITY
tail -90 /tmp/db2-parity-go.log
grep -q '^--- PASS: TestParityWithTheCReplay' /tmp/db2-parity-go.log || {
   say "PARITY FAILED; see /tmp/db2-parity-go.log"
   exit 1
}
say "PARITY PASSED"
