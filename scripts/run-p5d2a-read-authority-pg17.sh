#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-p5d2a-read-authority-pg17: Postgres URL required" >&2
  exit 1
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
DB=aimee_p5d2a_authority_gate
URL="${BASE_URL%/*}/$DB"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB"
psql -v ON_ERROR_STOP=1 "$URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm"
psql -v ON_ERROR_STOP=1 "$URL" -f "$ROOT/src/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$URL" -f - >/dev/null
psql -v ON_ERROR_STOP=1 "$URL" -f "$ROOT/src/modules/db2/c/schema_grants.sql" >/dev/null
{
  sed '/INSERT INTO public.kb_management_token_intent_namespace(correlation_id,jti,kind)/,$d' \
    "$ROOT/scripts/p5c2d-token-authority-pg17-test.sql"
  sed -n '1,$p' "$ROOT/scripts/p5d2a-read-authority-pg17-test.sql"
} | psql -v ON_ERROR_STOP=1 "$URL" -f -
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $DB"
