#!/bin/bash
# One-time setup inside the validation container. Run AS ROOT in the container.
set -u
chmod +x /root/start-kb.sh
# Generate ONCE. Regenerating under a running kb leaves the server presenting a
# bearer the kb no longer accepts, which surfaces as a 401 the caller reports as
# "the knowledge service refused" — a confusing way to fail a test harness.
[ -s /root/kb-bearer.txt ] || head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > /root/kb-bearer.txt
echo "bearer bytes: $(wc -c < /root/kb-bearer.txt)"

# The kb runs as root, so peer auth on the unix socket cannot map to `aimee`.
# Give the role a password and reach it over loopback TCP with md5/scram.
su - postgres -c "psql -c \"ALTER ROLE aimee PASSWORD 'aimee-e2e';\"" >/dev/null
if ! su - postgres -c "psql -Atc \"SELECT 1 FROM pg_roles WHERE rolname='aimee_migrator'\"" | grep -qx 1; then
  su - postgres -c "psql -c \"CREATE ROLE aimee_migrator LOGIN SUPERUSER PASSWORD 'aimee-migrate-e2e';\"" >/dev/null
else
  su - postgres -c "psql -c \"ALTER ROLE aimee_migrator PASSWORD 'aimee-migrate-e2e';\"" >/dev/null
fi
echo "role password set"

[ -s /root/server-bearer.txt ] || head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > /root/server-bearer.txt
echo "server bearer bytes: $(wc -c < /root/server-bearer.txt)"
chmod +x /root/start-server.sh 2>/dev/null || true
