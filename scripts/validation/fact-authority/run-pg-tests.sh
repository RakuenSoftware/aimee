#!/bin/bash
# Run the typed-fact unit tests against REAL Postgres rather than the sqlite
# shim they normally use.
#
# This is the only way to exercise gap 2's guard on the engine that runs in
# production: the guard is SQL (a CASE-rank comparison inside the UPDATE and its
# probe), and the shim translates DB2's SQL rather than executing it as Postgres
# would. A guard that is correct under the shim and wrong under libpq would look
# green all the way to deployment.
#
# The template is built by the repo's own db2-test-template tool against its own
# database, NOT cloned from the running kb's — a clone needs the source idle, and
# the kb holds pooled connections. Run AS ROOT in the container.
set -u
export LC_ALL=C LANG=C
export PGPASSWORD=aimee-e2e
TPL="postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_test_tpl"
export AIMEE_TEST_DB2_TEMPLATE_URL="$TPL"

psql -h 127.0.0.1 -U aimee -d postgres -q -tAc \
  "select 1 from pg_database where datname='aimee_test_tpl'" | grep -q 1 || \
  psql -h 127.0.0.1 -U aimee -d postgres -q -c "create database aimee_test_tpl" 2>&1 | tail -1

echo "== building the template schema =="
/root/pgtests/db2-test-template "$TPL" /root/pgtests/db2_test_reset.sql 2>&1 | tail -3

echo
echo "== typed-fact tests, real postgres =="
rc=0
for t in unit-test-fact-lifecycle unit-test-fact-ingest; do
  printf '%-28s ' "$t"
  if out="$(/root/pgtests/$t 2>&1)"; then
    echo "PASS"
  else
    echo "FAIL"
    rc=1
    echo "$out" | tail -15 | sed 's/^/    /'
  fi
done
exit $rc
