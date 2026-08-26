#!/bin/bash
# Does a database carrying an OLDER schema survive applying the current one?
#
# Nothing else in the tree asks this. The sqlite shim does not execute DDL the
# way PostgreSQL does, and every other suite builds its database from scratch --
# so a teardown statement that is wrong, or that silently does nothing, looks
# identical to one that worked. The two defects this was written to catch were
# both of that shape:
#
#   * a trigger sweep matching LIKE '...' ESCAPE '\\', which PostgreSQL refuses
#     ("escape string must be empty or one character"). The error aborts the
#     whole schema apply, so every installed deployment fails to START -- worse
#     than whatever the statement was removing.
#   * DROP FUNCTION with a guessed argument list. DROP FUNCTION IF EXISTS with a
#     WRONG signature SUCCEEDS, reporting that nothing needed dropping, so the
#     upgrade reads as clean and leaves the function behind.
#
# Run this whenever a schema change removes or rewrites database objects.
#
# Usage, inside a container with PostgreSQL and the two trees deployed:
#   NEW_TREE=/work/aimee OLD_TREE=/work/aimee-old \
#   URL=postgresql://user:pass@127.0.0.1:5432/aimee_upgrade \
#   ./upgrade-over-installed-schema.sh
#
# OLD_TREE is any checkout predating the change; the point is that it builds a
# database in the shape a deployment already has.
set -uo pipefail
export LC_ALL=C LANG=C

NEW_TREE="${NEW_TREE:-/work/aimee}"
OLD_TREE="${OLD_TREE:-/work/aimee-old}"
URL="${URL:-postgresql://aimeetest:aimeetest@127.0.0.1:5432/aimee_upgrade}"
DB="${URL##*/}"
PREFIX="${PREFIX:-db3}"   # the object-name prefix this upgrade is removing

q() { su postgres -c "psql -tAX -d $DB -c \"$1\"" 2>/dev/null | tr -d ' '; }
say() { echo "  $*"; }
fail=0
check() { if [ "$2" = "$3" ]; then say "PASS  $1 ($3)"; else say "FAIL  $1: want $2, got $3"; fail=1; fi; }

count_tables()    { q "select count(*) from pg_tables where schemaname='public' and tablename like '${PREFIX}%'"; }
count_functions() { q "select count(*) from pg_proc p join pg_namespace n on n.oid=p.pronamespace where n.nspname='public' and p.proname like '${PREFIX}%'"; }
count_triggers()  { q "select count(*) from pg_trigger t join pg_class c on c.oid=t.tgrelid join pg_namespace n on n.oid=c.relnamespace where not t.tgisinternal and n.nspname='public' and t.tgname like '${PREFIX}%'"; }

echo "=== 1. build a database in the shape a deployment already has ==="
cd "$OLD_TREE/src" || exit 1
make -j"$(nproc)" AIMEE_TEST_PG=1 build/obj/tests/db2-test-template >/tmp/old-build.log 2>&1 ||
  { echo "old template build failed"; tail -20 /tmp/old-build.log; exit 1; }
./build/obj/tests/db2-test-template "$URL" tests/db2_test_reset.sql >/tmp/old-apply.log 2>&1 ||
  { echo "old schema apply failed"; tail -20 /tmp/old-apply.log; exit 1; }

before_t=$(count_tables); before_f=$(count_functions); before_g=$(count_triggers)
say "before: ${before_t} tables, ${before_f} functions, ${before_g} triggers matching '${PREFIX}%'"
if [ "${before_t:-0}" -eq 0 ] && [ "${before_f:-0}" -eq 0 ] && [ "${before_g:-0}" -eq 0 ]; then
   say "FAIL  the old tree left nothing to remove; this run would prove nothing"
   exit 1
fi

echo "=== 2. a vector write works on the old schema ==="
DIM=$(q "select atttypmod from pg_attribute where attrelid='memory_embeddings'::regclass and attname='embedding'")
VEC="[$(python3 -c "print(','.join(['0.01']*${DIM}))")]"
ins() { su postgres -c "psql -tAX -v ON_ERROR_STOP=1 -d $DB -c \"insert into memory_embeddings(point_id, embedding) values ($1, '$VEC'::vector) on conflict (point_id) do nothing\"" 2>&1; }
say "insert: $(ins 900001)"
check "row landed before the upgrade" 1 "$(q "select count(*) from memory_embeddings where point_id=900001")"

echo "=== 3. apply the CURRENT schema over that same database ==="
cd "$NEW_TREE/src" || exit 1
make -j"$(nproc)" AIMEE_TEST_PG=1 build/obj/tests/db2-test-template >/tmp/new-build.log 2>&1 ||
  { echo "current template build failed"; tail -20 /tmp/new-build.log; exit 1; }
# The template builder drops and recreates by design. An upgrade is the other
# case, so it is asked to apply in place.
AIMEE_TEMPLATE_KEEP=1 ./build/obj/tests/db2-test-template "$URL" tests/db2_test_reset.sql >/tmp/new-apply.log 2>&1
rc=$?
if [ $rc -ne 0 ]; then
   say "FAIL  the upgrade itself aborted (rc=$rc) -- an installed deployment would not start"
   tail -20 /tmp/new-apply.log
   exit 1
fi
say "PASS  the upgrade applied"

echo "=== 4. the removed objects are actually gone ==="
check "tables"    0 "$(count_tables)"
check "functions" 0 "$(count_functions)"
check "triggers"  0 "$(count_triggers)"

echo "=== 5. the database still works, and kept what it had ==="
say "insert: $(ins 900002)"
check "a write after the upgrade lands"  1 "$(q "select count(*) from memory_embeddings where point_id=900002")"
check "the pre-upgrade row survived"     1 "$(q "select count(*) from memory_embeddings where point_id=900001")"

echo
[ $fail -eq 0 ] && echo "upgrade-over-installed-schema: PASS" || echo "upgrade-over-installed-schema: FAIL"
exit $fail
