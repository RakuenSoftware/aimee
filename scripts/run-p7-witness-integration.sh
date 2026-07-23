#!/usr/bin/env bash
# P7-witness integration gate: the C-side witness tests that need a REAL Postgres.
#
#   producer  — signs a checkpoint over verified leaves and verifies it against the
#               vault-derived public key
#   emit      — proves the bytes leaving kb on the log path verify offline from the
#               trust anchor alone, with no database access
#   tamper    — E3 §2 live-store scenarios: locally inconsistent tampering caught
#               locally; coherent rewrite NOT locally detectable but exposed by
#               comparison against a retained copy
#
# Each runs against its OWN freshly created database. The tamper test is
# destructive by construction (it disables the WORM triggers to reach the state an
# attacker who has defeated them would be in), so it must never share a database
# with anything else. Kept out of run-p1-rls-gate.sh for that reason.
#
#   scripts/run-p7-witness-integration.sh postgres://aimee:aimee@localhost:5432/postgres
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "usage: $0 <superuser libpq url>   (or set AIMEE_TEST_PG_URL)" >&2
  exit 2
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
BIN="${AIMEE_WITNESS_BIN_DIR:-$ROOT/src/build/obj/tests}"

run_case() {
  local name="$1" db="$2" bin="$3"
  if [ ! -x "$bin" ]; then
    echo "== P7-witness integration: MISSING BINARY $bin ==" >&2
    echo "   build it first: make -C src ${bin##*/}" >&2
    exit 1
  fi
  echo "== P7-witness integration: $name =="
  psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $db;" >/dev/null
  psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $db;" >/dev/null
  # The test's own db2_init applies the schema; it only needs the extensions.
  psql -v ON_ERROR_STOP=1 "${BASE_URL%/*}/$db" \
    -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null
  # The test must not skip: an unset URL here would silently pass the gate.
  AIMEE_TEST_PG_URL="${BASE_URL%/*}/$db" "$bin"
  psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $db;" >/dev/null
}

run_case "checkpoint producer" aimee_p7_witness_produce_gate \
         "$BIN/unit-test-witness-checkpoint-produce-pg"
run_case "evidence emission"   aimee_p7_witness_emit_gate \
         "$BIN/unit-test-witness-emit-pg"
run_case "tamper detection"    aimee_p7_witness_tamper_gate \
         "$BIN/unit-test-witness-tamper-pg"

echo "== P7-witness integration gate: PASSED =="
