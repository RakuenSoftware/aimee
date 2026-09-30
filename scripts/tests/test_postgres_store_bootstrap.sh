#!/usr/bin/env bash
# Current-layout provisioning, credential refresh, and restart ACL preservation.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
container="aimee-store-bootstrap-$$"
data_volume="${container}-data"
tls_volume="${container}-tls"
runtime_password=runtime-first
cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
  docker volume rm "$data_volume" "$tls_volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT
admin_sql() {
  docker exec "$container" psql -X -U postgres -d aimee_store -v ON_ERROR_STOP=1 -Atqc "$1"
}
start_store() {
  docker run -d --name "$container" \
    -e POSTGRES_USER=postgres -e POSTGRES_PASSWORD=admin-fixture \
    -e AIMEE_STORE_MIGRATOR_PASSWORD=migrator-fixture \
    -e AIMEE_STORE_RUNTIME_PASSWORD="$runtime_password" \
    -e POSTGRES_DB=aimee_store -e PGDATA=/var/lib/postgresql/data/pgdata \
    -v "$data_volume":/var/lib/postgresql/data \
    -v "$tls_volume":/var/lib/postgresql/secure \
    -v "$REPO_ROOT/scripts/postgres-secure-entrypoint.sh":/opt/aimee/postgres-secure-entrypoint.sh:ro \
    -v "$REPO_ROOT/scripts/postgres-store-init.sh":/docker-entrypoint-initdb.d/10-aimee-store-roles.sh:ro \
    -v "$REPO_ROOT/scripts/postgres-hygiene-role.sql":/usr/local/share/aimee/postgres-hygiene-role.sql:ro \
    --entrypoint /opt/aimee/postgres-secure-entrypoint.sh postgres:18 >/dev/null
}
wait_for_runtime() {
  for _ in $(seq 1 90); do
    if docker exec -e PGPASSWORD="$runtime_password" -e PGSSLMODE=require "$container" \
       psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store -Atqc 'SELECT 1' 2>/dev/null | grep -qx 1; then
      return
    fi
    if [[ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]]; then break; fi
    sleep 1
  done
  docker logs "$container" >&2 || true
  return 1
}
# The removed administrator identity must be rejected before any provisioning.
for script in postgres-store-init.sh postgres-secure-entrypoint.sh; do
  if POSTGRES_USER=aimee POSTGRES_PASSWORD=fixture \
     AIMEE_STORE_MIGRATOR_PASSWORD=fixture AIMEE_STORE_RUNTIME_PASSWORD=fixture \
     bash "$REPO_ROOT/scripts/$script" >/dev/null 2>&1; then
    echo 'store accepted the retired administrator identity' >&2; exit 1
  fi
done
docker volume create "$data_volume" >/dev/null
docker volume create "$tls_volume" >/dev/null
start_store
wait_for_runtime

test "$(admin_sql "SELECT NOT rolsuper AND NOT rolcreatedb AND NOT rolcreaterole FROM pg_roles WHERE rolname='aimee_store_migrator'")" = t
test "$(admin_sql "SELECT pg_has_role('aimee_store_runtime','aimee_memory_hygiene','MEMBER') AND NOT pg_has_role('aimee_store_runtime','aimee_memory_hygiene','USAGE')")" = t
# Current domain migrations own new objects and can narrow default grants.
docker exec -i -e PGPASSWORD=migrator-fixture -e PGSSLMODE=require "$container" \
  psql -h 127.0.0.1 -U aimee_store_migrator -d aimee_store -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE bootstrap_probe(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, payload text);
INSERT INTO bootstrap_probe(payload) VALUES('preserved');
CREATE TABLE protected_history(id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY);
REVOKE ALL ON protected_history FROM aimee_store_runtime, PUBLIC;
GRANT SELECT ON protected_history TO aimee_store_runtime;
REVOKE ALL ON SEQUENCE protected_history_id_seq FROM aimee_store_runtime, PUBLIC;
CREATE FUNCTION protected_writer() RETURNS integer LANGUAGE sql SECURITY DEFINER
 SET search_path=pg_catalog AS $$ SELECT 1 $$;
REVOKE ALL ON FUNCTION protected_writer() FROM aimee_store_runtime, PUBLIC;
CREATE TABLE memory_rejection_tombstones(id bigint);
REVOKE DELETE, TRUNCATE ON memory_rejection_tombstones FROM aimee_store_runtime, PUBLIC;
SQL
# Administrator-owned objects are not adopted or granted to the application.
admin_sql 'CREATE TABLE administrator_probe(id integer)' >/dev/null
docker stop "$container" >/dev/null
docker rm "$container" >/dev/null
runtime_password=runtime-second
start_store
wait_for_runtime
test "$(admin_sql "SELECT payload FROM bootstrap_probe")" = preserved
test "$(admin_sql "SELECT tableowner FROM pg_tables WHERE tablename='administrator_probe' AND schemaname='public'")" = postgres
test "$(admin_sql "SELECT has_table_privilege('aimee_store_runtime','administrator_probe','SELECT,INSERT,UPDATE,DELETE')")" = f
test "$(admin_sql "SELECT has_table_privilege('aimee_store_runtime','protected_history','SELECT') AND NOT has_table_privilege('aimee_store_runtime','protected_history','INSERT,UPDATE,DELETE,TRUNCATE') AND NOT has_sequence_privilege('aimee_store_runtime','protected_history_id_seq','USAGE,SELECT,UPDATE') AND NOT has_function_privilege('aimee_store_runtime','protected_writer()','EXECUTE')")" = t
test "$(admin_sql "SELECT has_table_privilege('aimee_store_runtime','memory_rejection_tombstones','DELETE,TRUNCATE')")" = f
docker exec -e PGPASSWORD="$runtime_password" -e PGSSLMODE=require "$container" \
  psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store -v ON_ERROR_STOP=1 \
  -c "INSERT INTO bootstrap_probe(payload) VALUES('after restart')" >/dev/null
for password in runtime-first wrong; do
  if docker exec -e PGPASSWORD="$password" -e PGSSLMODE=require "$container" \
     psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store -Atqc 'SELECT 1' >/dev/null 2>&1; then
    echo 'store accepted an obsolete or incorrect password' >&2; exit 1
  fi
done
if docker exec -e PGPASSWORD="$runtime_password" -e PGSSLMODE=disable "$container" \
   psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store -Atqc 'SELECT 1' >/dev/null 2>&1; then
  echo 'store accepted plaintext TCP' >&2; exit 1
fi
echo 'postgres-store-bootstrap: ok'
