#!/usr/bin/env bash
set -euo pipefail

db=aimee_p5b_status_gate
runuser -u postgres -- dropdb --if-exists "$db"
runuser -u postgres -- createdb "$db"
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" \
  -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;'
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" -f /tmp/p5b-schema-roles.sql
sed 's/__EMBED_DIM__/1024/g' /tmp/p5b-schema.sql | \
  runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db"
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" -f /tmp/p5b-schema-grants.sql
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" -f /tmp/p5b-status-pg17-test.sql
