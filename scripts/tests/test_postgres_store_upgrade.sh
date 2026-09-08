#!/usr/bin/env bash
# Reproduce the published pre-role-split store and prove an in-place upgrade is
# data-preserving, fail-closed, and idempotent.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
if [[ -z "${LEGACY_STORE_DB:-}" ]]; then
  for legacy_db in aimee_store aimee_shared; do
    LEGACY_STORE_DB="$legacy_db" bash "$0"
  done
  exit 0
fi
suffix="$$"
legacy_container="aimee-store-upgrade-legacy-${suffix}"
repaired_container="aimee-store-upgrade-repaired-${suffix}"
data_volume="aimee-store-upgrade-data-${suffix}"
tls_volume="aimee-store-upgrade-tls-${suffix}"

cleanup() {
  docker rm -f "$legacy_container" "$repaired_container" >/dev/null 2>&1 || true
  docker volume rm "$data_volume" "$tls_volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_for_sql() {
  local container="$1" user="$2" password="$3"
  for _ in $(seq 1 90); do
    if docker exec -e PGPASSWORD="$password" -e PGSSLMODE=require "$container" \
         psql -h 127.0.0.1 -U "$user" -d aimee_store -Atqc 'SELECT 1' \
         2>/dev/null | grep -qx 1; then
      return 0
    fi
    if ! docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null | grep -qx true; then
      docker logs "$container" >&2 || true
      return 1
    fi
    sleep 1
  done
  docker logs "$container" >&2 || true
  return 1
}

docker volume create "$data_volume" >/dev/null
docker volume create "$tls_volume" >/dev/null

# The last fully published pre-split manifest used this exact owner/password.
docker run -d --name "$legacy_container" \
  -e POSTGRES_USER=aimee -e POSTGRES_PASSWORD=aimee -e POSTGRES_DB="$LEGACY_STORE_DB" \
  -e PGDATA=/var/lib/postgresql/data/pgdata \
  -v "$data_volume":/var/lib/postgresql/data postgres:18 >/dev/null
legacy_ready=0
for _ in $(seq 1 90); do
  if docker exec "$legacy_container" psql -U aimee -d "$LEGACY_STORE_DB" -Atqc 'SELECT 1' \
       2>/dev/null | grep -qx 1; then
    legacy_ready=1
    break
  fi
  sleep 1
done
if [[ "$legacy_ready" != 1 ]]; then
  docker logs "$legacy_container" >&2 || true
  exit 1
fi
docker exec -i "$legacy_container" psql -U aimee -d "$LEGACY_STORE_DB" -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE release_upgrade_probe(
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  payload text NOT NULL
);
INSERT INTO release_upgrade_probe(payload) VALUES ('preserved-before-upgrade');
CREATE FUNCTION pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT 'legacy'::text $$;
CREATE FUNCTION pg_now_text(value integer) RETURNS text LANGUAGE sql AS $$ SELECT value::text $$;
CREATE PROCEDURE upgrade_noop() LANGUAGE sql AS $$ SELECT 1 $$;
CREATE FUNCTION upgrade_definer() RETURNS text LANGUAGE sql SECURITY DEFINER
  SET search_path = pg_catalog AS $$ SELECT current_user::text $$;
REVOKE ALL ON FUNCTION upgrade_definer() FROM PUBLIC;
CREATE SCHEMA aimee_kb_vault_orchestrator_api;
REVOKE ALL ON SCHEMA aimee_kb_vault_orchestrator_api FROM PUBLIC;
CREATE FUNCTION aimee_kb_vault_orchestrator_api.upgrade_private() RETURNS integer LANGUAGE sql AS $$ SELECT 1 $$;
REVOKE ALL ON FUNCTION aimee_kb_vault_orchestrator_api.upgrade_private() FROM PUBLIC;
CREATE SCHEMA aimee_kb_worm_api;
REVOKE ALL ON SCHEMA aimee_kb_worm_api FROM PUBLIC;
CREATE SCHEMA unrelated;
CREATE FUNCTION unrelated.untouched() RETURNS integer LANGUAGE sql AS $$ SELECT 1 $$;
CREATE EXTENSION pg_trgm;
ALTER SYSTEM SET listen_addresses = 'localhost';
SQL
docker stop "$legacy_container" >/dev/null
docker rm "$legacy_container" >/dev/null

start_repaired() {
  docker run -d --name "$repaired_container" \
    -e POSTGRES_USER=postgres -e POSTGRES_PASSWORD=repair-admin-secret \
    -e AIMEE_STORE_MIGRATOR_PASSWORD=repair-migrator-secret \
    -e AIMEE_STORE_RUNTIME_PASSWORD=repair-runtime-secret \
    -e POSTGRES_DB=aimee_store -e PGDATA=/var/lib/postgresql/data/pgdata \
    -v "$data_volume":/var/lib/postgresql/data \
    -v "$tls_volume":/var/lib/postgresql/secure \
    -v "$REPO_ROOT/scripts/postgres-secure-entrypoint.sh":/opt/aimee/postgres-secure-entrypoint.sh:ro \
    -v "$REPO_ROOT/scripts/postgres-store-init.sh":/docker-entrypoint-initdb.d/10-aimee-store-roles.sh:ro \
    --entrypoint /opt/aimee/postgres-secure-entrypoint.sh postgres:18 >/dev/null
  wait_for_sql "$repaired_container" aimee_store_runtime repair-runtime-secret
}

start_repaired

test "$(docker exec -e PGPASSWORD=repair-runtime-secret -e PGSSLMODE=require \
  "$repaired_container" psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store \
  -Atqc 'SELECT payload FROM release_upgrade_probe')" = preserved-before-upgrade
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT tableowner FROM pg_tables WHERE schemaname='public' AND tablename='release_upgrade_probe'")" \
  = aimee_store_migrator
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT sequenceowner FROM pg_sequences WHERE schemaname='public' AND sequencename='release_upgrade_probe_id_seq'")" \
  = aimee_store_migrator
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT rolsuper || ':' || rolcanlogin || ':' || (rolpassword IS NULL) FROM pg_authid WHERE rolname='aimee'")" \
  = true:false:true

# Replay overloaded functions and a procedure as the restricted migration role.
docker exec -i -e PGPASSWORD=repair-migrator-secret -e PGSSLMODE=require \
  "$repaired_container" psql -h 127.0.0.1 -U aimee_store_migrator -d aimee_store -v ON_ERROR_STOP=1 <<'SQL'
CREATE OR REPLACE FUNCTION pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT 'replayed'::text $$;
CREATE OR REPLACE FUNCTION pg_now_text(value integer) RETURNS text LANGUAGE sql AS $$ SELECT value::text $$;
CREATE OR REPLACE PROCEDURE upgrade_noop() LANGUAGE sql AS $$ SELECT 2 $$;
CREATE OR REPLACE FUNCTION aimee_kb_vault_orchestrator_api.upgrade_private() RETURNS integer LANGUAGE sql AS $$ SELECT 2 $$;
SQL
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT count(*) FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid AND d.deptype='e' WHERE p.proowner='aimee_store_migrator'::regrole")" = 0
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT prosecdef AND proconfig=ARRAY['search_path=pg_catalog'] AND NOT EXISTS (SELECT 1 FROM aclexplode(proacl) WHERE grantee=0) FROM pg_proc WHERE proname='upgrade_definer'")" = t
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT has_schema_privilege('aimee_store_runtime','aimee_kb_vault_orchestrator_api','USAGE') OR has_schema_privilege('aimee_store_runtime','aimee_kb_worm_api','USAGE')")" = f
test "$(docker exec "$repaired_container" psql -U postgres -d aimee_store -Atqc \
  "SELECT pg_get_userbyid(proowner) FROM pg_proc WHERE oid='unrelated.untouched()'::regprocedure")" = aimee
container_ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$repaired_container")
test "$(docker exec -e PGPASSWORD=repair-runtime-secret -e PGSSLMODE=require \
  "$repaired_container" psql -h "$container_ip" -U aimee_store_runtime -d aimee_store \
  -Atqc 'SELECT pg_now_text()')" = replayed
if docker exec -e PGPASSWORD=repair-runtime-secret -e PGSSLMODE=require \
  "$repaired_container" psql -h "$container_ip" -U aimee_store_runtime -d aimee_store \
  -c "ALTER FUNCTION pg_now_text() OWNER TO aimee_store_runtime" >/dev/null 2>&1; then
  echo "runtime role acquired migration ownership" >&2
  exit 1
fi

# A listening postmaster is insufficient: wrong credentials and plaintext TCP
# must both fail even though pg_isready would return success.
if docker exec -e PGPASSWORD=wrong -e PGSSLMODE=require "$repaired_container" \
     psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store -Atqc 'SELECT 1' \
     >/dev/null 2>&1; then
  echo "store upgrade accepted the wrong runtime password" >&2
  exit 1
fi
if docker exec "$repaired_container" psql -h 127.0.0.1 -U aimee_store_runtime \
     -d aimee_store -Atqc 'SELECT 1' >/dev/null 2>&1; then
  echo "store upgrade accepted plaintext/passwordless TCP" >&2
  exit 1
fi

# Recreate the container over the same volumes. Every reconciliation step must
# be safe after it has already committed, including revocation of the bootstrap
# identity.
docker stop "$repaired_container" >/dev/null
docker rm "$repaired_container" >/dev/null
start_repaired
test "$(docker exec -e PGPASSWORD=repair-runtime-secret -e PGSSLMODE=require \
  "$repaired_container" psql -h 127.0.0.1 -U aimee_store_runtime -d aimee_store \
  -Atqc 'SELECT payload FROM release_upgrade_probe')" = preserved-before-upgrade

# Two named stores are ambiguous; never open TCP or silently discard one.
docker exec "$repaired_container" psql -U postgres -d postgres -v ON_ERROR_STOP=1 \
  -c 'CREATE DATABASE aimee_shared' >/dev/null
docker restart "$repaired_container" >/dev/null
for _ in $(seq 1 30); do
  [ "$(docker inspect -f '{{.State.Running}}' "$repaired_container")" = false ] && break
  sleep 1
done
test "$(docker inspect -f '{{.State.Running}}' "$repaired_container")" = false
# Drain the log stream: grep -q can SIGPIPE Docker under pipefail.
docker logs "$repaired_container" 2>&1 | grep 'both aimee_shared and aimee_store exist' >/dev/null

echo "postgres-store-upgrade: ok ($LEGACY_STORE_DB)"
