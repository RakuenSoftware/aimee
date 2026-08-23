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
#
# THIS RUNNER MUST REPRODUCE THE HARNESS ENVIRONMENT, NOT JUST THE DATABASE.
#
# `make unit-tests` exports AIMEE_CONFIG_TEST_DEFAULTS and AIMEE_CONFIG_TEST_MODULE
# so the binaries can resolve config through the real config module. This script
# ran them with only the template URL set, so every config accessor fell back to
# the zero-initialised DB2_RUNTIME_CONFIG struct and read 0.
#
# That produced a FALSE FAILURE I then misdiagnosed as a sqlite-vs-Postgres
# divergence in the retraction path. There was no divergence: the sqlite binary
# fails identically when run this way. I had run the Postgres binary outside the
# harness and the sqlite binary inside it, so the control differed in two things
# at once and the difference I measured was my own invocation.
#
# The specific flag behind that (typed_facts_enabled) is now retired, so these
# two tests would pass either way. The environment gap is fixed anyway, because
# the next config-gated assertion would fail the same silent way and there would
# be nothing in the output to say why.
set -u
export LC_ALL=C LANG=C
export PGPASSWORD=aimee-e2e
TPL="postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_test_tpl"
export AIMEE_TEST_DB2_TEMPLATE_URL="$TPL"

# Config module + its shipped defaults, the same two the Makefile exports.
CFG_MODULE="${AIMEE_CONFIG_TEST_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-config}"
CFG_DEFAULTS="${AIMEE_CONFIG_TEST_DEFAULTS:-/root/config-defaults.json}"
if [ -x "$CFG_MODULE" ] && [ -s "$CFG_DEFAULTS" ]; then
  export AIMEE_CONFIG_TEST_MODULE="$CFG_MODULE"
  export AIMEE_CONFIG_TEST_DEFAULTS="$CFG_DEFAULTS"
  export AIMEE_CONFIG_TEST_HOST_HOME="${HOME:-/root}"
  echo "config module: $CFG_MODULE"
  echo "config defaults: $CFG_DEFAULTS"
else
  # Say so rather than running anyway and reporting whatever falls out. A result
  # produced with config accessors reading 0 is not a result about the product.
  echo "WARNING: config module or defaults missing, so every config accessor will"
  echo "         read 0 and any config-gated assertion will fail for that reason."
  echo "         module=$CFG_MODULE defaults=$CFG_DEFAULTS"
fi

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
