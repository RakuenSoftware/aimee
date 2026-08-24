#!/bin/sh
# Provision a plain PostgreSQL 17 inside an LXC container, for the the store family
# suites.
#
# Deliberately NOT the pgvector/pgvectorscale environment: none of the postgres module's
# families store a vector, so pulling an extension build into this loop would
# cost minutes per run to test nothing these suites touch. Debian 13 ships
# PostgreSQL 17, so this is an apt install and a listen address.
set -eu

export DEBIAN_FRONTEND=noninteractive

echo "== waiting for network =="
i=0
while [ $i -lt 60 ]; do
  if getent hosts deb.debian.org >/dev/null 2>&1; then break; fi
  i=$((i + 1))
  sleep 1
done

echo "== installing postgresql =="
apt-get update -qq
apt-get install -y -qq postgresql-17 >/dev/null

echo "== starting =="
pg_ctlcluster 17 main start 2>/dev/null || true
i=0
while [ $i -lt 30 ]; do
  if su postgres -c '/usr/lib/postgresql/17/bin/psql -tAc "SELECT 1"' >/dev/null 2>&1; then
    break
  fi
  i=$((i + 1))
  sleep 1
done

su postgres -c '/usr/lib/postgresql/17/bin/psql -tAc "SELECT version()"' | head -1
echo "== ready =="
