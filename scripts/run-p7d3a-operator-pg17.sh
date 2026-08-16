#!/usr/bin/env bash
set -euo pipefail

base_url=${1:-}
if [[ -z "$base_url" || "$base_url" != */postgres ]]; then
  echo "usage: $0 postgres://.../postgres" >&2
  exit 2
fi

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
psql_bin=${PSQL:-psql}
url_prefix=${base_url%/postgres}
run_tag="${$}_${RANDOM}"
legacy_schema=$(mktemp)
databases=()

cleanup() {
  for db in "${databases[@]}"; do
    "$psql_bin" "$base_url" -X -v ON_ERROR_STOP=1 \
      -c "DROP DATABASE IF EXISTS \"$db\" WITH (FORCE)" >/dev/null 2>&1 || true
  done
  rm -f "$legacy_schema"
}
trap cleanup EXIT INT TERM

# Build the exact immediately-pre-D3a schema from this checkout.  Keeping this
# mechanical makes the gate fail if the migration delimiters drift.
awk '
  /last_opened_rewrap_fence BIGINT NOT NULL DEFAULT 0/ { next }
  /-- P7-reseal-d3a: reserve a durable acknowledgement marker/ { skip=1 }
  skip && /aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status\(\) FROM PUBLIC;/ {
    skip=0; next
  }
  !skip { print }
' "$repo_dir/src/modules/db2/c/schema.sql" >"$legacy_schema"
sed -i 's/fencing_token    BIGINT NOT NULL DEFAULT 1 CHECK (fencing_token > 0)/fencing_token    BIGINT NOT NULL DEFAULT 0 CHECK (fencing_token >= 0)/' "$legacy_schema"
sed -i 's/__EMBED_DIM__/1024/g' "$legacy_schema"
if grep -q 'org_vault_rewrap_operator_status\|last_opened_rewrap_fence' "$legacy_schema"; then
  echo "failed to derive pre-D3a schema" >&2
  exit 1
fi

create_db() {
  local db=$1
  databases+=("$db")
  "$psql_bin" "$base_url" -X -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$db\"" >/dev/null
  "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 \
    -f "$repo_dir/src/modules/db2/c/schema_roles.sql" >/dev/null
  "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 -f "$legacy_schema" >/dev/null
}

schema_fingerprint() {
  local db=$1
  "$psql_bin" "$url_prefix/$db" -X -Atq -v ON_ERROR_STOP=1 <<'SQL'
SELECT md5(string_agg(item,E'\n' ORDER BY item)) FROM (
  SELECT 'column:'||table_name||':'||ordinal_position||':'||column_name||':'||data_type||':'||
         is_nullable||':'||COALESCE(column_default,'') AS item
    FROM information_schema.columns WHERE table_schema='public'
  UNION ALL
  SELECT 'constraint:'||c.relname||':'||con.conname||':'||pg_get_constraintdef(con.oid)
    FROM pg_constraint con JOIN pg_class c ON c.oid=con.conrelid
    JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public'
  UNION ALL
  SELECT 'index:'||tablename||':'||indexname||':'||indexdef
    FROM pg_indexes WHERE schemaname='public'
) definitions;
SQL
}

seed_case() {
  local db=$1 case_name=$2
  case "$case_name" in
    open_completed)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,receipt,receipt_digest,inventory_digest,stage_digest)
VALUES(repeat('1',32),'r1','uid:0','completed',2,1,1,2,'\x01',decode(repeat('01',32),'hex'),
 decode(repeat('02',32),'hex'),decode(repeat('03',32),'hex'));
UPDATE kb_vault_control SET sealed=false,seal_epoch=2,fencing_token=1;
SQL
      ;;
    open_recovery)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class,failure_from_state)
VALUES(repeat('1',32),'r1','uid:0','recovery_required',2,1,1,2,'backend','preparing');
UPDATE kb_vault_control SET sealed=false,seal_epoch=2,fencing_token=1;
SQL
      ;;
    terminal_fence_gap)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,receipt,receipt_digest,inventory_digest,stage_digest)
VALUES(repeat('1',32),'r1','uid:0','completed',2,1,1,2,'\x01',decode(repeat('01',32),'hex'),
 decode(repeat('02',32),'hex'),decode(repeat('03',32),'hex'));
UPDATE kb_vault_control SET sealed=true,seal_epoch=2,fencing_token=3;
SQL
      ;;
    terminal_epoch_mismatch)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class,failure_from_state)
VALUES(repeat('1',32),'r1','uid:0','recovery_required',2,1,1,2,'backend','preparing');
UPDATE kb_vault_control SET sealed=true,seal_epoch=3,fencing_token=2;
SQL
      ;;
    completed_then_aborted)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,receipt,receipt_digest,inventory_digest,stage_digest)
VALUES(repeat('1',32),'r1','uid:0','completed',2,1,1,2,'\x01',decode(repeat('01',32),'hex'),
 decode(repeat('02',32),'hex'),decode(repeat('03',32),'hex'));
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class)
VALUES(repeat('2',32),'r2','uid:0','aborted',3,2,2,3,'cancelled');
UPDATE kb_vault_control SET sealed=true,seal_epoch=3,fencing_token=2;
SQL
      ;;
    recovery_then_later)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class,failure_from_state)
VALUES(repeat('1',32),'r1','uid:0','recovery_required',2,1,1,2,'backend','preparing');
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class)
VALUES(repeat('2',32),'r2','uid:0','aborted',3,2,2,3,'cancelled');
UPDATE kb_vault_control SET sealed=true,seal_epoch=3,fencing_token=2;
SQL
      ;;
    duplicate_fences)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation,failure_class) VALUES
(repeat('1',32),'r1','uid:0','aborted',2,1,1,2,'cancelled'),
(repeat('2',32),'r2','uid:0','aborted',2,1,2,3,'cancelled');
UPDATE kb_vault_control SET sealed=true,seal_epoch=2,fencing_token=1;
SQL
      ;;
    malformed_control)
      "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
INSERT INTO kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,fencing_token,
 old_generation,new_generation)
VALUES(repeat('1',32),'r1','uid:0','preparing',2,1,1,2);
UPDATE kb_vault_control SET sealed=false,seal_epoch=2,fencing_token=1;
SQL
      ;;
  esac
}

for case_name in open_completed open_recovery terminal_fence_gap terminal_epoch_mismatch \
                 completed_then_aborted recovery_then_later duplicate_fences malformed_control; do
  db="p7d3a_${case_name}_${run_tag}"
  create_db "$db"
  seed_case "$db" "$case_name"
  before=$(schema_fingerprint "$db")
  error_file=$(mktemp)
  if sed 's/__EMBED_DIM__/1024/g' "$repo_dir/src/modules/db2/c/schema.sql" | \
       "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 -f - \
       >/dev/null 2>"$error_file"; then
    echo "$case_name: migration unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
  fi
  grep -q 'P7_D3A_MARKER_MIGRATION_REQUIRED' "$error_file"
  rm -f "$error_file"
  after=$(schema_fingerprint "$db")
  [[ "$before" == "$after" ]]
  marker_count=$("$psql_bin" "$url_prefix/$db" -X -Atq -v ON_ERROR_STOP=1 \
    -c "SELECT count(*) FROM information_schema.columns WHERE table_schema='public' AND table_name='kb_vault_control' AND column_name='last_opened_rewrap_fence'")
  [[ "$marker_count" == 0 ]]
done

expect_upgrade_failure() {
  local db=$1 expected=$2
  local error_file
  error_file=$(mktemp)
  if sed 's/__EMBED_DIM__/1024/g' "$repo_dir/src/modules/db2/c/schema.sql" | \
       "$psql_bin" "$url_prefix/$db" -X -v ON_ERROR_STOP=1 -f - \
       >/dev/null 2>"$error_file"; then
    echo "$db: upgrade unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
  fi
  grep -q "$expected" "$error_file"
  rm -f "$error_file"
}

# Partial-install upgrade behavior is explicit: unsafe marker/index shapes fail
# atomically, while a type-correct empty marker and canonical fence index resume.
wrong_marker_db="p7d3a_wrong_marker_${run_tag}"
create_db "$wrong_marker_db"
"$psql_bin" "$url_prefix/$wrong_marker_db" -X -v ON_ERROR_STOP=1 \
  -c 'ALTER TABLE public.kb_vault_control ADD COLUMN last_opened_rewrap_fence INTEGER' \
  >/dev/null
expect_upgrade_failure "$wrong_marker_db" P7_D3A_MARKER_MIGRATION_REQUIRED

null_marker_db="p7d3a_null_marker_${run_tag}"
create_db "$null_marker_db"
"$psql_bin" "$url_prefix/$null_marker_db" -X -v ON_ERROR_STOP=1 \
  -c 'ALTER TABLE public.kb_vault_control ADD COLUMN last_opened_rewrap_fence BIGINT' \
  >/dev/null
expect_upgrade_failure "$null_marker_db" P7_D3A_MARKER_MIGRATION_REQUIRED

wrong_index_db="p7d3a_wrong_index_${run_tag}"
create_db "$wrong_index_db"
"$psql_bin" "$url_prefix/$wrong_index_db" -X -v ON_ERROR_STOP=1 \
  -c 'CREATE INDEX idx_kb_vault_rewrap_fencing_token ON public.kb_vault_rewrap_operation(request_id)' \
  >/dev/null
expect_upgrade_failure "$wrong_index_db" P7_D3A_FENCE_INDEX_MIGRATION_REQUIRED

resume_db="p7d3a_resume_${run_tag}"
create_db "$resume_db"
"$psql_bin" "$url_prefix/$resume_db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
ALTER TABLE public.kb_vault_control
  ADD COLUMN last_opened_rewrap_fence BIGINT DEFAULT 7;
UPDATE public.kb_vault_control SET last_opened_rewrap_fence=0;
CREATE UNIQUE INDEX idx_kb_vault_rewrap_fencing_token
  ON public.kb_vault_rewrap_operation(fencing_token);
SQL
sed 's/__EMBED_DIM__/1024/g' "$repo_dir/src/modules/db2/c/schema.sql" | \
  "$psql_bin" "$url_prefix/$resume_db" -X -v ON_ERROR_STOP=1 -f - >/dev/null
resume_shape=$("$psql_bin" "$url_prefix/$resume_db" -X -Atq -v ON_ERROR_STOP=1 <<'SQL'
SELECT format('%s:%s:%s:%s',a.atttypid='bigint'::regtype,a.attnotnull,
  pg_get_expr(d.adbin,d.adrelid)='0'::text,
  (SELECT pg_get_expr(fd.adbin,fd.adrelid)='1'::text
     FROM pg_attribute fa JOIN pg_attrdef fd
       ON fd.adrelid=fa.attrelid AND fd.adnum=fa.attnum
    WHERE fa.attrelid='public.kb_vault_control'::regclass
      AND fa.attname='fencing_token'))
  FROM pg_attribute a JOIN pg_attrdef d
    ON d.adrelid=a.attrelid AND d.adnum=a.attnum
 WHERE a.attrelid='public.kb_vault_control'::regclass
   AND a.attname='last_opened_rewrap_fence';
SQL
)
[[ "$resume_shape" == "t:t:t:t" ]]

success_db="p7d3a_success_${run_tag}"
databases+=("$success_db")
"$psql_bin" "$base_url" -X -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$success_db\"" >/dev/null
"$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 \
  -f "$repo_dir/src/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$repo_dir/src/modules/db2/c/schema.sql" | \
  "$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 -f - >/dev/null
"$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 \
  -f "$repo_dir/src/modules/db2/c/schema_grants.sql" >/dev/null
# Plant both classes of grant drift caught in branch review, then prove that a
# grants re-apply repairs them before the runtime assertions execute.
"$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 <<'SQL' >/dev/null
CREATE SCHEMA p7d3a_hostile;
CREATE TABLE p7d3a_hostile.direct_table(id BIGINT);
CREATE SEQUENCE p7d3a_hostile.direct_sequence;
CREATE FUNCTION p7d3a_hostile.direct_function() RETURNS BIGINT LANGUAGE sql AS 'SELECT 1';
GRANT USAGE ON SCHEMA p7d3a_hostile TO aimee_kb_vault_orchestrator_login;
GRANT SELECT ON p7d3a_hostile.direct_table TO aimee_kb_vault_orchestrator_login;
GRANT USAGE ON SEQUENCE p7d3a_hostile.direct_sequence TO aimee_kb_vault_orchestrator_login;
GRANT EXECUTE ON FUNCTION p7d3a_hostile.direct_function()
  TO aimee_kb_vault_orchestrator_login;
GRANT USAGE ON SCHEMA p7d3a_hostile TO aimee_kb_vault_orchestrator;
GRANT SELECT ON p7d3a_hostile.direct_table TO aimee_kb_vault_orchestrator;
GRANT USAGE ON SEQUENCE p7d3a_hostile.direct_sequence
  TO aimee_kb_vault_orchestrator;
GRANT EXECUTE ON FUNCTION p7d3a_hostile.direct_function()
  TO aimee_kb_vault_orchestrator;
GRANT SELECT ON pg_catalog.pg_authid
  TO aimee_kb_vault_orchestrator,aimee_kb_vault_orchestrator_login;
ALTER TABLE p7d3a_hostile.direct_table OWNER TO aimee_kb_vault_orchestrator;
ALTER SEQUENCE p7d3a_hostile.direct_sequence OWNER TO aimee_kb_vault_orchestrator;
ALTER FUNCTION p7d3a_hostile.direct_function() OWNER TO aimee_kb_vault_orchestrator;
ALTER SCHEMA p7d3a_hostile OWNER TO aimee_kb_vault_orchestrator;
GRANT CREATE,TEMPORARY ON DATABASE :DBNAME
  TO aimee_kb_vault_orchestrator,aimee_kb_vault_orchestrator_login;
ALTER DATABASE :DBNAME OWNER TO aimee_kb_vault_orchestrator;
GRANT aimee_kb_vault_orchestrator TO aimee_kb_runtime;
SQL
"$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 \
  -f "$repo_dir/src/modules/db2/c/schema_grants.sql" >/dev/null
"$psql_bin" "$url_prefix/$success_db" -X -v ON_ERROR_STOP=1 \
  -f "$repo_dir/src/tests/test_p7_d3a_schema.sql" >/dev/null

echo "P7 D3a PostgreSQL 17 migration and authority gate passed"
