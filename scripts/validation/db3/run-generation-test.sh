#!/bin/bash
# Run the kb/kb_pdf generation write-path test against real PostgreSQL.
#
# This one is separate from run-pg-tests.sh because it is the fixture that
# `make unit-tests` deliberately does not run: it needs a live Postgres with
# pgvector, so it sits in the check_tests_are_run allowlist beside the other
# *-pg fixtures and is driven from here instead.
#
# It gets its OWN database rather than the suite's template. The template is the
# clone source for every other test; writing into it would leak this test's rows
# into all of them.
#
# Run AS ROOT inside the verification container, after pg-setup.sh.
set -euo pipefail
export LC_ALL=C LANG=C
TREE="${TREE:-/work/aimee}"
DB="${DB:-aimee_generation_check}"
URL="postgresql://aimeetest:aimeetest@127.0.0.1:5432/$DB"

su postgres -c "psql -X -tAc \"DROP DATABASE IF EXISTS $DB\"" >/dev/null
su postgres -c "createdb -O aimeetest $DB"
su postgres -c "psql -X -v ON_ERROR_STOP=1 -d $DB -c 'CREATE EXTENSION IF NOT EXISTS vector'" >/dev/null
su postgres -c "psql -X -v ON_ERROR_STOP=1 -d $DB -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm'" >/dev/null

cd "$TREE/src"

# db2-test-template applies the real schema, which is what creates kb_documents,
# kb_embeddings, and the generation column and backfill under test.
make -j"$(nproc)" --no-print-directory AIMEE_TEST_PG=1 \
   AIMEE_TEST_DB2_TEMPLATE_URL="$URL" db2-test-template >/dev/null
make -j"$(nproc)" --no-print-directory GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 \
   build/obj/tests/unit-test-pgvec-generation-pg >/dev/null

echo "--- generation write path, real PostgreSQL ---"
AIMEE_TEST_PG_URL="$URL" ./build/obj/tests/unit-test-pgvec-generation-pg
rc=$?

su postgres -c "psql -X -tAc \"DROP DATABASE IF EXISTS $DB\"" >/dev/null
exit $rc
