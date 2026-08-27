#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_DB_HOSTNAME:=aimee-store-db}"
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

exec /usr/local/bin/docker-entrypoint.sh postgres \
  -c ssl=on \
  -c ssl_cert_file="$secure_dir/server.crt" \
  -c ssl_key_file="$secure_dir/server.key" \
  -c ssl_min_protocol_version=TLSv1.2 \
  -c password_encryption=scram-sha-256 \
  -c hba_file="$secure_dir/pg_hba.conf"
