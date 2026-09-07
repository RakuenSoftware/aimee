#!/bin/bash
# Provision a throwaway PostgreSQL database and run the full recursive-learning
# stack against it. The inner harness starts and stops both services and their
# modules; this wrapper owns the database and scratch-directory lifecycle.
set -euo pipefail

BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
    echo "usage: AIMEE_TEST_PG_URL=postgresql:///postgres make -C src learning-loop-evidence" >&2
    exit 2
fi

: "${AIMEE_STORE_URL:?Set AIMEE_STORE_URL to the non-owner store role URL}"
: "${AIMEE_STORE_MIGRATION_URL:?Set AIMEE_STORE_MIGRATION_URL to the separate store schema owner URL}"

AIMEE_ROOT="${AIMEE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
DB_NAME="aimee_learning_evidence_$$"
RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/aimee-learning-evidence.XXXXXX")

cleanup() {
    dropdb --if-exists --maintenance-db="$BASE_URL" "$DB_NAME" >/dev/null 2>&1 || true
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

createdb --maintenance-db="$BASE_URL" "$DB_NAME"

# Retarget each identity to the disposable database without borrowing the
# administrator identity for runtime operations or changing shared role passwords.
retarget_url() {
    python3 - "$1" "$DB_NAME" <<'PYURL'
import sys
from urllib.parse import urlsplit, urlunsplit
u = urlsplit(sys.argv[1])
if u.scheme not in ('postgres', 'postgresql'):
    raise SystemExit('PostgreSQL URLs required')
print(urlunsplit(u._replace(path='/' + sys.argv[2])))
PYURL
}
TEST_URL=$(retarget_url "$BASE_URL")
STORE_URL=$(retarget_url "$AIMEE_STORE_URL")
MIGRATION_URL=$(retarget_url "$AIMEE_STORE_MIGRATION_URL")
runtime_role=$(psql -XAt "$AIMEE_STORE_URL" -v ON_ERROR_STOP=1 -c 'SELECT current_user')
migrator_role=$(psql -XAt "$AIMEE_STORE_MIGRATION_URL" -v ON_ERROR_STOP=1 -c 'SELECT current_user')
[ "$runtime_role" != "$migrator_role" ] || {
    echo "store runtime and migrator must use different roles" >&2; exit 2;
}
psql -X "$TEST_URL" -v ON_ERROR_STOP=1 -v runtime="$runtime_role" \
    -v migrator="$migrator_role" -v db="$DB_NAME" <<'SQL'
CREATE EXTENSION IF NOT EXISTS vector;
GRANT CONNECT ON DATABASE :"db" TO :"runtime", :"migrator";
GRANT USAGE ON SCHEMA public TO :"runtime";
GRANT USAGE, CREATE ON SCHEMA public TO :"migrator";
ALTER DEFAULT PRIVILEGES FOR ROLE :"migrator" IN SCHEMA public
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO :"runtime";
ALTER DEFAULT PRIVILEGES FOR ROLE :"migrator" IN SCHEMA public
    GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO :"runtime";
SQL

env AIMEE_ROOT="$AIMEE_ROOT" AIMEE_SRC="$AIMEE_SRC" WORKDIR="$RUN_DIR" \
    PGDB="$TEST_URL" AIMEE_DB2_URL="$TEST_URL" AIMEE_STORE_URL="$STORE_URL" \
    AIMEE_STORE_MIGRATION_URL="$MIGRATION_URL" \
    bash "$AIMEE_ROOT/tests/e2e/learning-loops-pg-e2e.sh"
