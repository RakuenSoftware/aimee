#!/usr/bin/env bash
set -euo pipefail

base_url=${1:-}
if [[ -z "$base_url" || "$base_url" != */postgres ]]; then
  echo "usage: $0 postgres://.../postgres" >&2
  exit 2
fi
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
psql_bin=${PSQL:-psql}
prefix=${base_url%/postgres}
db="p7d3b_${$}_${RANDOM}"
cleanup() { "$psql_bin" "$base_url" -X -v ON_ERROR_STOP=1 -c "DROP DATABASE IF EXISTS \"$db\" WITH (FORCE)" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM
"$psql_bin" "$base_url" -X -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$db\"" >/dev/null
"$psql_bin" "$prefix/$db" -X -v ON_ERROR_STOP=1 -f "$repo_dir/src/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$repo_dir/src/modules/db2/c/schema.sql" | "$psql_bin" "$prefix/$db" -X -v ON_ERROR_STOP=1 -f - >/dev/null
"$psql_bin" "$prefix/$db" -X -v ON_ERROR_STOP=1 -f "$repo_dir/src/modules/db2/c/schema_grants.sql" >/dev/null
"$psql_bin" "$prefix/$db" -X -v ON_ERROR_STOP=1 -f "$repo_dir/src/tests/test_p7_d3b_schema.sql" >/dev/null
echo "P7-D3b PostgreSQL 17 operator gate passed"
