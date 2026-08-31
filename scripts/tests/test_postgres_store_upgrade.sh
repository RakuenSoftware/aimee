#!/usr/bin/env bash
# Reproduce the published pre-role-split store and prove an in-place upgrade is
# data-preserving, fail-closed, and idempotent.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
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
  -e POSTGRES_USER=aimee -e POSTGRES_PASSWORD=aimee -e POSTGRES_DB=aimee_store \
  -e PGDATA=/var/lib/postgresql/data/pgdata \
  -v "$data_volume":/var/lib/postgresql/data postgres:18 >/dev/null
legacy_ready=0
for _ in $(seq 1 90); do
  if docker exec "$legacy_container" psql -U aimee -d aimee_store -Atqc 'SELECT 1' \
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
docker exec -i "$legacy_container" psql -U aimee -d aimee_store -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE release_upgrade_probe(
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  payload text NOT NULL
);
INSERT INTO release_upgrade_probe(payload) VALUES ('preserved-before-upgrade');
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

echo "postgres-store-upgrade: ok"
