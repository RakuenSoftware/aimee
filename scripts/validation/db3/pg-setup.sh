#!/bin/bash
# Bring up the local PostgreSQL and create the template database the unit suite
# clones per test. Run AS ROOT inside the verification container.
#
# The role is a superuser because db2-test-template creates and drops databases
# and installs extensions. That is safe here and nowhere else, which is the
# reason this whole harness insists on a throwaway container.
#
# The SQL is in files rather than -c arguments on purpose: it passes through
# `su postgres -c`, and a DO block or a quoted literal written inline gets eaten
# by one shell layer or another. A file has no quoting to lose.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LC_ALL=C LANG=C

pg_ctlcluster 17 main start 2>/dev/null || true
for _ in $(seq 1 30); do
   pg_isready -q && break
   sleep 1
done

# The postgres user has to be able to read them, and this directory arrives by
# whatever route the tree did.
chmod a+rX "$HERE"
chmod a+r "$HERE/role.sql" "$HERE/ext.sql"

su postgres -c "psql -X -v ON_ERROR_STOP=1 -f '$HERE/role.sql'"
su postgres -c "createdb -O aimeetest aimee_test_tpl" 2>/dev/null \
   || echo "template database already present"
su postgres -c "psql -X -v ON_ERROR_STOP=1 -d aimee_test_tpl -f '$HERE/ext.sql'"

echo "SETUP-OK"
pg_isready
