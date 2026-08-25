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

base_no_query="$BASE_URL"
query=""
case "$BASE_URL" in
    *\?*)
        base_no_query="${BASE_URL%%\?*}"
        query="?${BASE_URL#*\?}"
        ;;
esac
TEST_URL="${base_no_query%/*}/$DB_NAME$query"

env AIMEE_ROOT="$AIMEE_ROOT" AIMEE_SRC="$AIMEE_SRC" WORKDIR="$RUN_DIR" \
    PGDB="$TEST_URL" AIMEE_DB2_URL="$TEST_URL" AIMEE_STORE_URL="$TEST_URL" \
    bash "$AIMEE_ROOT/tests/e2e/learning-loops-pg-e2e.sh"
