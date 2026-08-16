#!/usr/bin/env bash
# run-identity-facade-pg-test.sh: exercise the identity-token db2 facade against a
# REAL Postgres, connected as the REAL aimee_kb_token_authority_runtime role.
#
# The SQLite shim cannot stand in here: db2_management_token_authority_open runs
# role_assert, which demands a LOGIN NOINHERIT NOBYPASSRLS non-superuser role with
# a pinned search_path and row_security on. That check either passes against a
# properly provisioned database or it does not, and only real Postgres can say.
#
#   scripts/run-identity-facade-pg-test.sh postgres://user@host:5432/postgres
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-identity-facade-pg-test: no Postgres URL (arg1 or AIMEE_TEST_PG_URL)." >&2
  exit 1
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
TESTDB="aimee_identity_facade_gate"
PW="$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
SRC="$ROOT/src"
BIN="$(mktemp -d)/t_identity_facade"
trap 'rm -rf -- "$(dirname "$BIN")"' EXIT HUP INT TERM

echo "== identity facade gate: provisioning $TESTDB =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB;"
DB_URL="${BASE_URL%/*}/$TESTDB"
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;"
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$SRC/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$SRC/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$SRC/modules/db2/c/schema_grants.sql" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "ALTER ROLE aimee_kb_token_authority_runtime PASSWORD '$PW';"

echo "== identity facade gate: building =="
${CC:-cc} -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"$SRC" -I"$SRC/db2" -I"$SRC/headers" -I"$SRC/kb" -I"$SRC/modules/vault" \
  -I"$SRC/modules/audit/include" -I"$SRC/vendor" -I"$SRC/vendor/headers" \
  -I/usr/include/postgresql \
  "$SRC/modules/db2/c/management_token_authority.c" \
  "$SRC/modules/db2/c/db_postgres.c" \
  "$SRC/kb/kb_mgmt_token_authority.c" "$SRC/kb/kb_mgmt_token.c" \
  "$SRC/kb/kb_mgmt_token_public.c" "$SRC/kb/kb_identity_token.c" \
  "$SRC/modules/vault/vault_crypto.c" "$SRC/vendor/cJSON.c" "$SRC/server/oauth_pkce.c" \
  "$SRC/tests/test_identity_authority_facade_pg.c" \
  -lpq -lcrypto -o "$BIN"

# The provisioning above may run over a unix socket as the OS postgres user, but
# the facade must connect AS aimee_kb_token_authority_runtime, and peer auth can
# never authenticate a role that is not the OS user. So the client connects over
# TCP with the password set above. Override the host if Postgres is elsewhere.
CLIENT_HOST="${AIMEE_TEST_PG_CLIENT_HOST:-127.0.0.1}"
CLIENT_PORT="${AIMEE_TEST_PG_CLIENT_PORT:-5432}"
echo "== identity facade gate: running =="
PGPASSWORD="$PW" "$BIN" \
  "host=$CLIENT_HOST port=$CLIENT_PORT dbname=$TESTDB user=aimee_kb_token_authority_runtime"

echo "== identity facade gate: cleanup =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $TESTDB;"
echo "== identity facade gate: PASSED =="
