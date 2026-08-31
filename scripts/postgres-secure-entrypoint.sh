#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_DB_HOSTNAME:=aimee-store-db}"
: "${POSTGRES_USER:?POSTGRES_USER is required}"
: "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD is required}"
: "${POSTGRES_DB:?POSTGRES_DB is required}"
: "${PGDATA:?PGDATA is required}"
: "${AIMEE_STORE_MIGRATOR_PASSWORD:?AIMEE_STORE_MIGRATOR_PASSWORD is required}"
: "${AIMEE_STORE_RUNTIME_PASSWORD:?AIMEE_STORE_RUNTIME_PASSWORD is required}"
secure_dir=/var/lib/postgresql/secure
# The server mounts this volume read-only and must traverse the directory to
# read server.crt as its TLS trust root.  The certificate is public (0644);
# the private key and pg_hba.conf remain postgres-only (0600), so traversal
# does not expose either sensitive file.
install -d -o postgres -g postgres -m 0755 "$secure_dir"

if [[ ! -s "$secure_dir/server.key" || ! -s "$secure_dir/server.crt" ]]; then
  tmp_dir=$(mktemp -d "$secure_dir/.tls.XXXXXX")
  trap 'rm -rf -- "$tmp_dir"' EXIT
  openssl req -x509 -newkey rsa:3072 -sha256 -days 825 -nodes \
    -subj "/CN=${AIMEE_STORE_DB_HOSTNAME}" \
    -addext "subjectAltName=DNS:${AIMEE_STORE_DB_HOSTNAME}" \
    -keyout "$tmp_dir/server.key" -out "$tmp_dir/server.crt"
  chown postgres:postgres "$tmp_dir/server.key" "$tmp_dir/server.crt"
  chmod 0600 "$tmp_dir/server.key"
  chmod 0644 "$tmp_dir/server.crt"
  mv "$tmp_dir/server.key" "$secure_dir/server.key"
  mv "$tmp_dir/server.crt" "$secure_dir/server.crt"
  rmdir "$tmp_dir"
  trap - EXIT
fi

cat >"$secure_dir/pg_hba.conf" <<'EOF'
local   all  all               trust
hostssl all  all  0.0.0.0/0   scram-sha-256
hostssl all  all  ::/0        scram-sha-256
hostnossl all all 0.0.0.0/0   reject
hostnossl all all ::/0        reject
EOF
chown postgres:postgres "$secure_dir/pg_hba.conf"
chmod 0600 "$secure_dir/pg_hba.conf"

# The upstream image runs /docker-entrypoint-initdb.d only for an empty PGDATA.
# Releases before the store role split therefore keep their `aimee` superuser
# and never create the migrator/runtime roles when Compose replaces the image.
# Reconcile an existing cluster through a Unix-socket-only temporary postmaster;
# no TCP listener exists until the role split and password rotation have
# completed successfully.
if [[ -s "$PGDATA/PG_VERSION" ]]; then
  migration_socket="$secure_dir/reconcile-socket"
  migration_hba="$secure_dir/reconcile-pg_hba.conf"
  migration_log="$secure_dir/reconcile.log"
  install -d -o postgres -g postgres -m 0700 "$migration_socket"
  printf '%s\n' 'local all all trust' >"$migration_hba"
  chown postgres:postgres "$migration_hba"
  chmod 0600 "$migration_hba"
  touch "$migration_log"
  chown postgres:postgres "$migration_log"
  chmod 0600 "$migration_log"

  migration_started=0
  stop_migration_cluster() {
    if [[ "$migration_started" == 1 ]]; then
      gosu postgres pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null 2>&1 || true
      migration_started=0
    fi
  }
  trap stop_migration_cluster EXIT INT TERM
  gosu postgres pg_ctl -D "$PGDATA" -w -l "$migration_log" \
    -o "-c listen_addresses='' -c unix_socket_directories='$migration_socket' -c hba_file='$migration_hba' -c ssl=off" start
  migration_started=1

  existing_admin=""
  for candidate in postgres aimee; do
    if gosu postgres psql --host "$migration_socket" --username "$candidate" \
         --dbname "$POSTGRES_DB" --tuples-only --no-align \
         --command "SELECT 1 FROM pg_roles WHERE rolname = current_user AND rolsuper" \
         2>/dev/null | grep -qx 1; then
      existing_admin="$candidate"
      break
    fi
  done
  if [[ -z "$existing_admin" ]]; then
    echo "aimee store: existing cluster has no supported administrative role (postgres or aimee)" >&2
    exit 1
  fi

  AIMEE_STORE_ADMIN_USER="$existing_admin" PGHOST="$migration_socket" \
    /docker-entrypoint-initdb.d/10-aimee-store-roles.sh

  # Historical releases shipped the network-reachable `aimee:aimee`
  # superuser. Once its objects and ownership have moved, remove login and erase
  # its verifier so enabling TLS does not preserve that known credential.
  # Do this on every existing-cluster reconciliation, not only when `aimee`
  # was the role used above. A prior interrupted reconciliation may already
  # have created `postgres` but failed before revoking the legacy credential;
  # the next boot must finish the security transition rather than treating the
  # new role as proof that every later step committed.
  gosu postgres psql --host "$migration_socket" --username postgres \
    --dbname "$POSTGRES_DB" --set=ON_ERROR_STOP=1 <<'SQL'
-- PostgreSQL 18 does not allow the original bootstrap superuser to lose its
-- SUPERUSER attribute. NOLOGIN plus a NULL password is the supported durable
-- revocation: no HBA authentication method can use the historical identity.
SELECT 'ALTER ROLE aimee NOLOGIN PASSWORD NULL'
WHERE EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee') \gexec
SQL

  stop_migration_cluster
  trap - EXIT INT TERM
fi

exec /usr/local/bin/docker-entrypoint.sh postgres \
  -c ssl=on \
  -c ssl_cert_file="$secure_dir/server.crt" \
  -c ssl_key_file="$secure_dir/server.key" \
  -c ssl_min_protocol_version=TLSv1.2 \
  -c password_encryption=scram-sha-256 \
  -c hba_file="$secure_dir/pg_hba.conf"
