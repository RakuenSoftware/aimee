#!/bin/sh
# Runs INSIDE the scratch container: install what a real aimee-kb needs.
set -eu
export DEBIAN_FRONTEND=noninteractive

# Wait for DHCP/DNS before apt.
i=0
while [ $i -lt 60 ]; do
  getent hosts deb.debian.org >/dev/null 2>&1 && break
  sleep 2; i=$((i+1))
done

apt-get update -qq
apt-get install -y -qq postgresql-17 postgresql-17-pgvector python3 curl ca-certificates \
  >/dev/null 2>&1 || apt-get install -y postgresql postgresql-17-pgvector python3 curl ca-certificates

# aimee-kb links these; the template has some already.
apt-get install -y -qq libpq5 libssl3 libzstd1 libpam0g libsqlite3-0 libaudit1 >/dev/null 2>&1 || true

pg_ctlcluster 17 main start 2>/dev/null || service postgresql start 2>/dev/null || true
sleep 3

echo "=== versions ==="
python3 -V
/usr/lib/postgresql/17/bin/psql --version
echo "=== postgres up? ==="
su postgres -c "/usr/lib/postgresql/17/bin/psql -tAc 'select version()'" 2>&1 | head -1
echo "=== pgvector available? ==="
su postgres -c "/usr/lib/postgresql/17/bin/psql -tAc \"select name from pg_available_extensions where name='vector'\"" 2>&1 | head -1
