#!/bin/sh
# Run every native-PostgreSQL suite for the the store families that have moved to Go.
#
#   sh scripts/db1srv-postgres-suites.sh            # against an existing box
#   CT=8150 HOST=root@192.168.1.252 sh ...          # pick the container
#
# These suites test what the Go unit tests structurally cannot. The Go tests
# script the database, so they prove dispatch, validation and the commit/
# rollback decisions; they say nothing about whether ON CONFLICT reports a
# replay as zero rows, whether a CHECK constraint holds, or whether an ORDER BY
# picks the row the module assumes. That is what runs here.
#
# There is no SQLite anywhere in this file. The store is PostgreSQL; the C
# module and its schema are what these families replace.
set -eu

HOST="${HOST:-root@192.168.1.252}"
CT="${CT:-8150}"
PSQL=/usr/lib/postgresql/17/bin/psql
TEMPLATE="${TEMPLATE:-/var/lib/vz/template/cache/debian-13-standard_13.6-1_amd64.tar.zst}"

# The .252 host reaps these containers, and it is not hostile automation -- it is
# a documented reaper doing exactly what it says. /var/log/aimee-reaper.log:
#
#   2026-08-23T16:00:31 REAPED pct/8150 (no renewal for 15572s > 14400s)
#
# A guest is reclaimed 4h after creation or its last renewal, and
# /usr/local/bin/aimee-keepalive slides the lease a full TTL from now, with no
# cap on renewals. Nothing in this repo had ever called it, so every environment
# died at the four hour mark and the reaper got read as the host being unstable.
# This script's own comment used to say "destroyed mid-session by other
# automation there", which was a wrong diagnosis of a correct system.
#
# TWO CLOCKS, and renewing only answers one. The reaper also requires measurable
# activity within the TTL, so an idle guest is reclaimed however recently it was
# leased. That is the right rule rather than a gap: an idle box is
# indistinguishable from an abandoned one. A suite run is busy throughout, so
# renewing covers the run; between runs this container is SUPPOSED to die, and
# ensure_container rebuilds it.
#
# So this creates and provisions one when it is not there rather than failing
# with a shipping error, and touches ONLY the VMID it was given: that host runs a
# live aimee deployment of its own.
renew_lease() {
  ssh -n "$HOST" "aimee-keepalive ct:$CT" >/dev/null 2>&1 ||
    echo "== warning: could not renew the lease for CT $CT; it may be reaped mid-run" >&2
}

ensure_container() {
  if ssh "$HOST" "pct status $CT" 2>/dev/null | grep -q running; then
    renew_lease
    return
  fi
  echo "== CT $CT is not running: creating =="
  ssh "$HOST" "pct destroy $CT --force >/dev/null 2>&1 || true"
  ssh "$HOST" "pct create $CT $TEMPLATE \
    --hostname postgres-family-suites --cores 4 --memory 4096 --swap 1024 \
    --rootfs local-lvm:12 --net0 name=eth0,bridge=vmbr0,ip=dhcp \
    --unprivileged 1 --features nesting=1 --onboot 0 --start 1" >/dev/null
  # Lease it immediately. Provisioning PostgreSQL is the slowest step here, and
  # the 4h clock starts at creation rather than when the container becomes
  # useful, so a fresh guest is already burning its window while apt runs.
  renew_lease
  echo "== provisioning postgresql =="
  scp -q scripts/postgres-suite-setup.sh "$HOST:/tmp/pgsetup.sh"
  ssh "$HOST" "pct push $CT /tmp/pgsetup.sh /tmp/pgsetup.sh --perms 755" >/dev/null
  ssh "$HOST" "pct exec $CT -- sh /tmp/pgsetup.sh" | tail -2
}

ship() {
  base=$(basename "$1")
  scp -q "$1" "$HOST:/tmp/$base"
  ssh "$HOST" "pct push $CT /tmp/$base /tmp/$base --perms 644" >/dev/null
}

ensure_container

# Schemas ship with their migration version as a zero-padded prefix, so the glob
# that applies them below lands in the order the migration engine uses rather
# than in alphabetical order.
#
# That distinction did not exist while the schemas were nineteen independent
# CREATE TABLE files: any order worked, so alphabetical was as good as any. The
# first migration that ALTERs tables other files create breaks that -- applied
# alphabetically it runs before those tables exist. The order is read out of
# migrations.go rather than restated here, so the suite cannot drift from the
# order a deployment actually applies.
echo "== shipping schemas =="
ORDER=$(python3 -c '
import re
src = open("server-go/modules/aimee/families/migrations.go").read()
body = src[src.index("var schemaHistory"):src.index("// Migration is one")]
for version, name in re.findall(r"\{(\d+),\s*\"([^\"]+)\"\}", body):
    print(f"{int(version):03d} {name}")
')
[ -n "$ORDER" ] || { echo "could not read the migration order from migrations.go"; exit 1; }

# Clear schema files left by earlier runs before shipping. They are applied by a
# glob, so a file from a previous naming scheme is not inert -- it gets applied
# too, in whatever position its name sorts to, which silently defeats the
# ordering established below.
ssh -n "$HOST" "pct exec $CT -- sh -c 'rm -f /tmp/family_schema_*.sql /tmp/ordered_*.sql'" >/dev/null
ssh -n "$HOST" "rm -f /tmp/family_schema_*.sql /tmp/ordered_*.sql" >/dev/null

# Each schema ships under TWO names, because the two consumers want different
# things and conflating them broke both:
#
#   family_schema_<name>.sql   one file, applied alone by that family's suite.
#   ordered_<version>_<name>.sql  the whole-schema run, globbed in migration
#                              order -- a separate prefix so the per-family
#                              names cannot land in the ordered glob and be
#                              applied a second time out of order.
#
# ssh -n throughout: without it ssh reads the loop's stdin, swallowing the rest
# of the list, so only the first schema is ever shipped.
echo "$ORDER" | while read -r version base; do
  f="server-go/modules/aimee/families/$base"
  [ -f "$f" ] || { echo "migration $version names $base, which is not on disk"; exit 1; }
  scp -q "$f" "$HOST:/tmp/family_$base"
  ssh -n "$HOST" "pct push $CT /tmp/family_$base /tmp/family_$base --perms 644" >/dev/null
  scp -q "$f" "$HOST:/tmp/ordered_${version}_$base"
  ssh -n "$HOST" "pct push $CT /tmp/ordered_${version}_$base /tmp/ordered_${version}_$base --perms 644" >/dev/null
done

echo "== shipping suites =="
for f in scripts/test-family-economizer.sql \
         scripts/test-family-jti.sql \
         scripts/test-family-mgmt.sql \
         scripts/test-family-identity.sql \
         scripts/test-family-checkpoints-git.sql \
         scripts/test-family-guardrail.sql \
         scripts/test-family-pki.sql \
         scripts/test-family-sessions.sql \
         scripts/test-family-roundtable.sql \
         scripts/test-family-delegation.sql \
         scripts/test-family-lifecycle.sql \
         scripts/test-family-conversation.sql \
         scripts/test-family-ensemble.sql \
         scripts/test-family-telemetry.sql \
         scripts/test-family-workflow.sql \
         scripts/test-family-agent-work.sql \
         scripts/test-family-runtime.sql \
         scripts/test-whole-schema.sql; do
  ship "$f"
done

echo
FAIL=0
run() {
  printf '%-28s ' "$1"
  if out=$(ssh "$HOST" "timeout 300 pct exec $CT -- su postgres -c '$PSQL -q -d postgres -f /tmp/$2'" 2>&1); then
    echo "$out" | grep -E 'SUITE PASSED' || { echo "no PASSED line"; FAIL=$((FAIL+1)); }
  else
    echo "FAILED"
    echo "$out" | grep -iE 'error|assert' | head -3 | sed 's/^/    /'
    FAIL=$((FAIL+1))
  fi
}

run "economizer state"  test-family-economizer.sql
run "jti replay"        test-family-jti.sql
run "management"        test-family-mgmt.sql
run "identity"          test-family-identity.sql
run "checkpoints + git"  test-family-checkpoints-git.sql
run "guardrail state"    test-family-guardrail.sql
run "pki"                test-family-pki.sql
run "sessions"           test-family-sessions.sql
run "roundtable"         test-family-roundtable.sql
run "delegation"         test-family-delegation.sql
run "lifecycle"          test-family-lifecycle.sql
run "conversation"       test-family-conversation.sql
run "ensemble"          test-family-ensemble.sql
run "telemetry"         test-family-telemetry.sql
run "workflow"          test-family-workflow.sql
run "agent work"        test-family-agent-work.sql
run "runtime"           test-family-runtime.sql

# --- the whole schema, in one database ---------------------------------------
#
# Everything above applies ONE family's schema file. That isolation is right for
# testing a family, and it is exactly why none of those suites could see that
# agent_log was declared in two files with different types for `success`: each
# met only its own definition. A deployment applies all nineteen to one
# database, so that is the shape this checks.
#
# CREATED AS UTF8, EXPLICITLY, and this is load-bearing rather than tidiness.
# The container initdb's as SQL_ASCII, where char_length() and octet_length()
# are the same function -- so every byte-versus-character assertion below is a
# no-op there and passes whether or not migrations 20 and 21 are applied. The
# suite would have been unfalsifiable on exactly the property it exists to
# prove. Production runs initdb --encoding=UTF8, so this matches production and
# the assertions mean something. template0 because template1 is SQL_ASCII and a
# database inherits its template's encoding.
printf '%-28s ' "whole schema"
whole_out=$(ssh "$HOST" "pct exec $CT -- su postgres -c '
  psql -q -c \"DROP DATABASE IF EXISTS aimee_whole_schema\" &&
  psql -q -c \"CREATE DATABASE aimee_whole_schema ENCODING '\''UTF8'\'' LC_COLLATE '\''C'\'' LC_CTYPE '\''C'\'' TEMPLATE template0\" &&
  for f in /tmp/ordered_*.sql; do
    $PSQL --set ON_ERROR_STOP=1 -q -d aimee_whole_schema -f \$f >/dev/null || exit 1
  done &&
  $PSQL -q -d aimee_whole_schema -f /tmp/test-whole-schema.sql'" 2>&1) \
  && echo "$whole_out" | grep -E 'WHOLE SCHEMA SUITE PASSED' \
  || { echo "FAILED"; echo "$whole_out" | grep -iE 'error|assert' | head -3 | sed 's/^/    /'; FAIL=$((FAIL+1)); }

echo
if [ "$FAIL" -eq 0 ]; then
  echo "ALL POSTGRES FAMILY SUITES PASSED"
else
  echo "$FAIL SUITE(S) FAILED"
fi
exit "$FAIL"
