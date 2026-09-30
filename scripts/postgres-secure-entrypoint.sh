#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_DB_HOSTNAME:=aimee-store-db}"
: "${POSTGRES_USER:?POSTGRES_USER is required}"
if [[ "$POSTGRES_USER" != postgres ]]; then
  echo "aimee store: POSTGRES_USER must be postgres" >&2
  exit 1
fi
: "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD is required}"
: "${POSTGRES_DB:?POSTGRES_DB is required}"
: "${PGDATA:?PGDATA is required}"
: "${AIMEE_STORE_MIGRATOR_PASSWORD:?AIMEE_STORE_MIGRATOR_PASSWORD is required}"
: "${AIMEE_STORE_RUNTIME_PASSWORD:?AIMEE_STORE_RUNTIME_PASSWORD is required}"
secure_dir=${AIMEE_STORE_SECURE_DIR:-/var/lib/postgresql/secure}
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

# Encrypted deployments publish only the public trust certificate outside the
# mounted filesystem. The private key and reconciliation logs remain inside it.
if [[ -n "${AIMEE_STORE_PUBLIC_TLS_DIR:-}" ]]; then
  install -d -m 0755 "$AIMEE_STORE_PUBLIC_TLS_DIR"
  install -m 0644 "$secure_dir/server.crt" "$AIMEE_STORE_PUBLIC_TLS_DIR/server.crt"
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

# Existing current-layout stores refresh role credentials before opening TCP.
# Fresh stores use the upstream initdb hooks. No old database names, owners, or
# administrator identities are adopted during startup.
if [[ -s "$PGDATA/PG_VERSION" ]]; then
  provision_socket="$secure_dir/reconcile-socket"
  provision_hba="$secure_dir/reconcile-pg_hba.conf"
  provision_log="$secure_dir/reconcile.log"
  install -d -o postgres -g postgres -m 0700 "$provision_socket"
  printf '%s\n' 'local all all trust' >"$provision_hba"
  chown postgres:postgres "$provision_hba"
  chmod 0600 "$provision_hba"
  touch "$provision_log"
  chown postgres:postgres "$provision_log"
  chmod 0600 "$provision_log"

  provision_started=0
  stop_provision_cluster() {
    if [[ "$provision_started" == 1 ]]; then
      gosu postgres pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null 2>&1 || true
      provision_started=0
    fi
  }
  trap stop_provision_cluster EXIT INT TERM
  gosu postgres pg_ctl -D "$PGDATA" -w -l "$provision_log" \
    -o "-c listen_addresses='' -c unix_socket_directories='$provision_socket' -c hba_file='$provision_hba' -c ssl=off" start
  provision_started=1

  PGHOST="$provision_socket" /docker-entrypoint-initdb.d/10-aimee-store-roles.sh

  stop_provision_cluster
  trap - EXIT INT TERM
fi

exec /usr/local/bin/docker-entrypoint.sh postgres \
  -c listen_addresses='*' \
  -c ssl=on \
  -c ssl_cert_file="$secure_dir/server.crt" \
  -c ssl_key_file="$secure_dir/server.key" \
  -c ssl_min_protocol_version=TLSv1.2 \
  -c password_encryption=scram-sha-256 \
  -c hba_file="$secure_dir/pg_hba.conf"
