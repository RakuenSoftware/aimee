#!/bin/bash
# Own the scratch PostgreSQL database and process directory for the paired
# self-learning efficacy study. Raw output is preserved outside the run dir.
set -euo pipefail

BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
    echo "usage: AIMEE_TEST_PG_URL=postgresql:///postgres make -C src self-learning-efficacy" >&2
    exit 2
fi

AIMEE_ROOT="${AIMEE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
DB_NAME="aimee_self_learning_efficacy_$$"
RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/aimee-self-learning-efficacy.XXXXXX")
OUTPUT_DIR="${AIMEE_STUDY_OUTPUT:-$AIMEE_ROOT/artifacts/self-learning-efficacy-$(date -u +%Y%m%dT%H%M%SZ)}"

cleanup() {
    dropdb --if-exists --maintenance-db="$BASE_URL" "$DB_NAME" >/dev/null 2>&1 || true
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$OUTPUT_DIR"
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
STUDY_COMMIT=$(git -C "$AIMEE_ROOT" rev-parse HEAD)

{
    printf 'commit=%s\n' "$STUDY_COMMIT"
    printf 'started_at=%s\n' "$(date -u +%FT%TZ)"
    printf 'host=%s\n' "$(hostname)"
    printf 'kernel=%s\n' "$(uname -srmo)"
    printf 'postgres=%s\n' "$(psql --version)"
    printf 'python=%s\n' "$(python3 --version)"
    printf 'database=%s\n' "$DB_NAME"
} > "$OUTPUT_DIR/environment.txt"

env AIMEE_ROOT="$AIMEE_ROOT" AIMEE_SRC="$AIMEE_SRC" WORKDIR="$RUN_DIR" \
    OUTPUT_DIR="$OUTPUT_DIR" PGDB="$TEST_URL" AIMEE_DB2_URL="$TEST_URL" \
    AIMEE_STORE_URL="$TEST_URL" STUDY_COMMIT="$STUDY_COMMIT" \
    bash "$AIMEE_ROOT/tests/e2e/self-learning-efficacy-pg-e2e.sh" \
    2>&1 | tee "$OUTPUT_DIR/run.log"

printf 'finished_at=%s\n' "$(date -u +%FT%TZ)" >> "$OUTPUT_DIR/environment.txt"
printf 'output=%s\n' "$OUTPUT_DIR"
