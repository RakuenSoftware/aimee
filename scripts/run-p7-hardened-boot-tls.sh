#!/usr/bin/env bash
# P7/db2: full HARDENED-tier boot validation. Proves a real aimee-kb daemon connecting
# as a NON-owner runtime login role over verify-full TLS against an owner-migrated
# schema boots via db2_verify_pre_provisioned (presence-check, NOT apply) and runs the
# witness cadence as the restricted role. This is the end-to-end proof of the
# hardened bootstrap fix (approach C).
#
# On-demand / host-specific: needs PG with ssl=on, a server cert whose CN this host
# resolves to (default: the snakeoil cert CN aimee-test.bailes.us on CT103), the
# pg_hba path, and root to append a temporary hostssl line (reverted on exit). Run on
# the target host as root inside the container. Tune via env: PGHBA, DBHOST, CACERT,
# KB (path to aimee-kb), SRC (schema dir), CONN_CIDR.
set -u
HBA="${PGHBA:-/etc/postgresql/17/main/pg_hba.conf}"
DBHOST="${DBHOST:-aimee-test.bailes.us}"
CACERT="${CACERT:-/etc/ssl/certs/ssl-cert-snakeoil.pem}"
KB="${KB:-/tmp/gate/aimee-kb}"
SRC="${SRC:-/tmp/gate/src/modules/db2/c}"
CONN_CIDR="${CONN_CIDR:-192.168.1.103/32}"
DB=aimee_hardened_boot
RTLOGIN=rt_hardened_login
MARK="# HARDENED-BOOT-TEST-TEMP"
cleanup() {
  sed -i "/$MARK/d" "$HBA" 2>/dev/null
  su - postgres -c "psql -qtA -c \"SELECT pg_reload_conf()\"" >/dev/null 2>&1
  su - postgres -c "dropdb --if-exists $DB 2>/dev/null; psql -qtA -c \"DROP ROLE IF EXISTS $RTLOGIN\" 2>/dev/null" >/dev/null 2>&1
}
trap cleanup EXIT

echo "=== provision (owner/migrate): roles + schema (records dim+version) + grants ==="
su - postgres -c "dropdb --if-exists $DB 2>/dev/null; createdb $DB"
su - postgres -c "psql -qtA -d $DB -c \"CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm\"" >/dev/null
su - postgres -c "psql -q -d $DB -f $SRC/schema_roles.sql" >/dev/null 2>&1
su - postgres -c "sed 's/__EMBED_DIM__/1024/g' $SRC/schema.sql | psql -q -v ON_ERROR_STOP=1 -d $DB -f -" >/dev/null 2>&1
su - postgres -c "psql -q -d $DB -f $SRC/schema_grants.sql" >/dev/null 2>&1
# seed some evidence as owner
su - postgres -c "psql -qtA -d $DB -c \"SELECT org_vault_witness_append(1::smallint,'hb1','!kb','!reseal','','p','','g','2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),false,decode(repeat('00',32),'hex'))\"" >/dev/null

echo "=== create a non-owner runtime LOGIN role (member of aimee_kb_runtime) ==="
su - postgres -c "psql -qtA -d $DB -c \"CREATE ROLE $RTLOGIN LOGIN INHERIT IN ROLE aimee_kb_runtime\"" 2>&1 | head -1

echo "=== pg_hba: allow this role over TLS only, then reload ==="
echo "hostssl $DB $RTLOGIN $CONN_CIDR trust $MARK" >> "$HBA"
su - postgres -c "psql -qtA -c \"SELECT pg_reload_conf()\"" >/dev/null

echo "=== boot aimee-kb HARDENED as the runtime role over verify-full TLS ==="
H=$(mktemp -d)
DSN="postgresql://$RTLOGIN@$DBHOST:5432/$DB?sslmode=verify-full&sslrootcert=$CACERT"
AIMEE_KB_HARDENED=1 AIMEE_DB2_URL="$DSN" AIMEE_HOME=$H AIMEE_WITNESS_CADENCE_TEST_S=2 \
  $KB --http-port=18795 --log-level=info >/tmp/hf.out 2>/tmp/hf.err &
KBPID=$!
sleep 8
echo "--- alive? ---"; kill -0 $KBPID 2>/dev/null && echo kb-ALIVE || echo kb-EXITED
echo "--- db2_init / hardening / verify outcome ---"
grep -iE "hardened|schema apply|permission denied|verify|db2_init|runtime-role" /tmp/hf.err | head -8
echo "--- witness cadence (as runtime, hardened) ---"
grep -iE "kb.witness|checkpoint" /tmp/hf.err | head -4
echo "--- checkpoints produced ---"
su - postgres -c "psql -qtA -d $DB -c \"SELECT count(*) FROM kb_vault_witness_checkpoint\"" 2>/dev/null
kill -TERM $KBPID 2>/dev/null; sleep 1; kill -9 $KBPID 2>/dev/null

echo "=== NEGATIVE: un-migrated schema -> hardened boot must FAIL CLOSED ==="
NDB=aimee_hardened_unmigrated
su - postgres -c "dropdb --if-exists $NDB 2>/dev/null; createdb $NDB"
su - postgres -c "psql -qtA -d $NDB -c \"CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm\"" >/dev/null
# roles only (so the runtime role exists) — but NOT schema.sql (no kb_meta/dim/version/objects).
su - postgres -c "psql -q -d $NDB -f $SRC/schema_roles.sql" >/dev/null 2>&1
su - postgres -c "psql -qtA -d $NDB -c \"GRANT CONNECT ON DATABASE $NDB TO $RTLOGIN\"" >/dev/null 2>&1
echo "hostssl $NDB $RTLOGIN $CONN_CIDR trust $MARK" >> "$HBA"
su - postgres -c "psql -qtA -c \"SELECT pg_reload_conf()\"" >/dev/null
NH=$(mktemp -d)
NDSN="postgresql://$RTLOGIN@$DBHOST:5432/$NDB?sslmode=verify-full&sslrootcert=$CACERT"
AIMEE_KB_HARDENED=1 AIMEE_DB2_URL="$NDSN" AIMEE_HOME=$NH /tmp/gate/aimee-kb --http-port=18796 --log-level=info >/tmp/hfn.out 2>/tmp/hfn.err &
NPID=$!
sleep 6
# kb_main RETRIES db2_init on failure (process stays alive), so refusal is proven by
# db2 never connecting + the fail-closed message — NOT by the process exiting.
if grep -aqE "hardened schema verification failed" /tmp/hfn.err; then
  echo "NEG OK: hardened kb refused (fail-closed) against an un-migrated schema. Reason:"
  grep -aoE "hardened schema verification failed: [^\"]*" /tmp/hfn.err | head -1
  # and db2 must NOT be serving
  if curl -s http://localhost:18796/v1/health 2>/dev/null | grep -q '"db2_ok":true'; then
    echo "NEG FAIL: db2 reported connected despite the verification failure"
  fi
else
  echo "NEG FAIL: hardened kb did NOT emit the fail-closed verification refusal"; tail -5 /tmp/hfn.err
fi
kill -9 $NPID 2>/dev/null
su - postgres -c "dropdb --if-exists $NDB 2>/dev/null" >/dev/null 2>&1
rm -rf $NH

rm -rf $H
