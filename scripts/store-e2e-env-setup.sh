#!/bin/sh
# Build the store-migration test environment INSIDE an LXC container on .252.
#
# PostgreSQL + pgvector + pgvectorscale, which is what
# docs/proposals/pending/one-store-postgres-and-pgvectorscale-everywhere.md needs
# to test against: pgvectorscale is the whole point of S7, and an environment
# without it can only ever exercise the HNSW fallback.
#
# pgvectorscale is fetched as a prebuilt .deb per (version, pg major, arch), the
# same way Dockerfile does it, rather than compiled from the Rust crate -- that
# build costs ~7 minutes and produces a ~1 MB artifact with no dependency on this
# repo's source.
set -eu

PG_MAJOR="${PG_MAJOR:-17}"
PGVS_VERSION="${PGVS_VERSION:-0.9.0}"
# Observed digest for pg17/amd64 0.9.0, recorded so a re-run gets the same bytes.
# Dockerfile pins pg18; this env targets the PostgreSQL Debian 13 ships.
PGVS_SHA256_pg17_amd64="4cc8e0f233bf2d34d460efcd01bf91e11c36969ddc7c6ef06af933d817294429"
ARCH="${ARCH:-amd64}"
export DEBIAN_FRONTEND=noninteractive

echo "== waiting for network =="
i=0
while [ $i -lt 60 ]; do
  getent hosts deb.debian.org >/dev/null 2>&1 && break
  sleep 2; i=$((i+1))
done

echo "== base packages =="
apt-get update -qq
apt-get install -y -qq "postgresql-$PG_MAJOR" "postgresql-$PG_MAJOR-pgvector" \
  python3 curl ca-certificates unzip sqlite3 >/dev/null

echo "== pgvectorscale $PGVS_VERSION (pg$PG_MAJOR/$ARCH) =="
zip="pgvectorscale-${PGVS_VERSION}-pg${PG_MAJOR}-${ARCH}.zip"
url="https://github.com/timescale/pgvectorscale/releases/download/${PGVS_VERSION}/${zip}"
ok=0
for a in 1 2 3; do
  if curl -fsSL --connect-timeout 10 --max-time 300 -o "/tmp/$zip" "$url"; then ok=1; break; fi
  echo "  fetch attempt $a failed; backing off"; rm -f "/tmp/$zip"; sleep $((a * 5))
done
if [ "$ok" = "1" ]; then
  # Record the digest so a later run can be pinned to the same bytes; a release
  # asset is mutable in a way a tag build is not.
  got=$(sha256sum "/tmp/$zip" | cut -d' ' -f1)
  echo "  sha256: $got"
  if [ "$PG_MAJOR/$ARCH" = "17/amd64" ] && [ "$got" != "$PGVS_SHA256_pg17_amd64" ]; then
    echo "  ERROR: digest mismatch for a pinned asset (want $PGVS_SHA256_pg17_amd64)" >&2
    exit 1
  fi
  rm -rf /tmp/pgvs && unzip -j -q "/tmp/$zip" -d /tmp/pgvs
  deb=$(find /tmp/pgvs -name '*.deb' ! -name '*dbgsym*' -print -quit)
  if [ -n "$deb" ]; then
    dpkg -i "$deb" >/dev/null 2>&1 || dpkg-deb -x "$deb" /
    echo "  installed from ${deb##*/}"
  else
    echo "  WARNING: no .deb inside the asset"
  fi
else
  echo "  WARNING: pgvectorscale unavailable; only the HNSW fallback can be tested"
fi

pg_ctlcluster "$PG_MAJOR" main start 2>/dev/null || service postgresql start 2>/dev/null || true
sleep 3

PSQL="/usr/lib/postgresql/$PG_MAJOR/bin/psql"
echo "== verify =="
echo -n "  postgres : "; su postgres -c "$PSQL -tAc 'select version()'" 2>&1 | head -1 | cut -c1-40
for ext in vector vectorscale pg_trgm; do
  n=$(su postgres -c "$PSQL -tAc \"select count(*) from pg_available_extensions where name='$ext'\"" 2>/dev/null | tr -d ' ')
  echo "  $ext available: ${n:-0}"
done

echo "== scratch database =="
su postgres -c "$PSQL -tAc 'DROP DATABASE IF EXISTS store_e2e'" >/dev/null 2>&1
su postgres -c "$PSQL -tAc 'DROP ROLE IF EXISTS store_e2e'" >/dev/null 2>&1
su postgres -c "$PSQL -tAc \"CREATE ROLE store_e2e LOGIN PASSWORD 'store_e2e_pw'\"" >/dev/null 2>&1
su postgres -c "$PSQL -tAc 'CREATE DATABASE store_e2e OWNER store_e2e'" >/dev/null 2>&1
for ext in vector vectorscale pg_trgm; do
  su postgres -c "$PSQL -d store_e2e -tAc 'CREATE EXTENSION IF NOT EXISTS $ext'" >/dev/null 2>&1 \
    && echo "  extension $ext: created" || echo "  extension $ext: NOT created"
done
su postgres -c "$PSQL -d store_e2e -tAc 'GRANT ALL ON SCHEMA public TO store_e2e'" >/dev/null 2>&1
echo "  dsn: postgresql://store_e2e:store_e2e_pw@127.0.0.1:5432/store_e2e"
echo "ENV READY"
