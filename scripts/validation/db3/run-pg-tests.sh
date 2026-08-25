#!/bin/bash
# Run the C unit suite against real PostgreSQL, the way CI's unit-test-shards-pg
# job does: the same binaries, linked against libpq instead of the sqlite shim,
# so DB2's SQL is EXECUTED by the engine that runs it in production.
#
# This is what the DB3 routing work needs. The join-to-filter reduction the
# routed searches rest on is a claim ABOUT SQL -- that filtering on a resolved
# generation selects the same rows as joining projects and comparing to
# current_generation. The shim translates DB2's SQL rather than executing it as
# Postgres would, so a reduction that is wrong can look green all the way to
# deployment.
#
# Run AS ROOT inside the verification container, after pg-setup.sh.
#
# DO NOT run anything else against the same build tree while this is going, and
# DO NOT kill it. `make unit-tests` points HOME and TMPDIR at one temporary
# directory and removes it from an EXIT trap; killing the run fires that trap
# while test binaries are still inside it, and they then fail on getcwd() with
# nothing to say why. A killed run also leaves zero-byte binaries from
# interrupted links, which make considers up to date -- so the next run
# "executes" empty files and reports failures that are entirely your own. If a
# run is interrupted, delete them before trusting anything:
#
#     find src/build -type f -size 0 -delete
set -uo pipefail
export LC_ALL=C LANG=C
export AIMEE_TEST_DB2_TEMPLATE_URL="${AIMEE_TEST_DB2_TEMPLATE_URL:-postgresql://aimeetest:aimeetest@127.0.0.1:5432/aimee_test_tpl}"
TREE="${TREE:-/work/aimee}"
LOG="${LOG:-/work/pg-tests.log}"

# The build is tens of thousands of compiler lines. Keep them in the log and let
# the caller see the verdict.
exec 3>&1
exec >"$LOG" 2>&1

# The deterministic root-owned helpers the CI shards install: the trust-walk
# tests assert on ownership and mode, which an image does not guarantee.
install -d -o root -g root -m 0755 /run/aimee-test-helpers
for helper in cat env yes true; do
   install -o root -g root -m 0755 "$(type -P "$helper")" "/run/aimee-test-helpers/$helper"
done
printf '%s\n' 'deterministic root-owned test fixture' \
   | tee /run/aimee-test-helpers/root-file >/dev/null
chown root:root /run/aimee-test-helpers/root-file
chmod 0644 /run/aimee-test-helpers/root-file
export AIMEE_TEST_TRUSTED_CAT=/run/aimee-test-helpers/cat
export AIMEE_TEST_TRUSTED_ENV=/run/aimee-test-helpers/env
export AIMEE_TEST_TRUSTED_YES=/run/aimee-test-helpers/yes
export AIMEE_TEST_TRUSTED_TRUE=/run/aimee-test-helpers/true
export AIMEE_TEST_TRUSTED_ROOT_FILE=/run/aimee-test-helpers/root-file

cd "$TREE/src" || { echo "no tree at $TREE/src" >&3; exit 1; }

# Several test binaries name objects that are not declared as their own
# prerequisites, so a COLD parallel build can reach a link before the object
# exists. Each pass fills more in. Loop until a pass succeeds or stops making
# progress, and say which -- a silent retry would hide a real failure behind
# "it passed eventually".
rc=1
previous=-1
for pass in 1 2 3 4 5 6; do
   echo "########## pass $pass ##########"
   make -j"$(nproc)" GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 \
      UNIT_TEST_SKIP_P1=1 unit-tests-pg
   rc=$?
   [ $rc -eq 0 ] && break
   built=$(grep -c 'build/obj/tests/unit-test' "$LOG")
   echo "########## pass $pass rc=$rc binaries=$built ##########"
   if [ "$built" -eq "$previous" ]; then
      echo "no progress on pass $pass; stopping"
      break
   fi
   previous=$built
done

# Which vector index the schema actually built. pgvectorscale's DiskANN is the
# default and HNSW is the fallback, and until recently the tree said both at
# once -- so this is asserted against the database rather than read from code.
echo "########## vector index default ##########"
su postgres -c "psql -X -d aimee_test_tpl -f $(dirname "$0")/verify-index-default.sql" 2>&1 \
   | grep -E 'NOTICE|ERROR' || true

{
   echo "PG-UNIT-EXIT=$rc"
   echo "binaries-run=$(grep -c 'build/obj/tests/unit-test' "$LOG")"
   echo "getcwd-errors=$(grep -c 'getcwd' "$LOG")   # non-zero means a run was killed"
   echo "failures:"
   grep -E '^FAILED: ' "$LOG" | sed 's/^FAILED: //' | sort -u | sed 's/^/  /'
} >&3
