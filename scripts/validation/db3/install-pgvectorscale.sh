#!/bin/bash
# Install pgvectorscale, so a verification container can exercise the DEFAULT
# index rather than only the fallback.
#
# Without the extension a database can only build HNSW, so a run there proves
# the fallback works and says nothing about the default. That is the wrong half
# to have covered: DiskANN is what a real deployment indexes with, and it is
# also the only one of the two that can index vectors wider than 2000
# dimensions.
#
# Debian ships no package for it, so this takes the project's own release
# archive. Run AS ROOT inside the verification container.
set -euo pipefail
VERSION="${PGVECTORSCALE_VERSION:-0.9.0}"
PG_MAJOR="${PG_MAJOR:-17}"
ARCH="${ARCH:-amd64}"

if [ -f "/usr/share/postgresql/$PG_MAJOR/extension/vectorscale.control" ]; then
   echo "pgvectorscale is already installed"
   exit 0
fi

command -v unzip >/dev/null || {
   export DEBIAN_FRONTEND=noninteractive
   apt-get install -y -q unzip >/dev/null
}

archive="/tmp/pgvectorscale.zip"
url="https://github.com/timescale/pgvectorscale/releases/download/$VERSION/pgvectorscale-$VERSION-pg$PG_MAJOR-$ARCH.zip"
curl -sSL -o "$archive" "$url"
rm -rf /tmp/pgvectorscale && mkdir -p /tmp/pgvectorscale
unzip -q -o "$archive" -d /tmp/pgvectorscale

# The archive carries a .deb for the matching PostgreSQL major.
deb=$(find /tmp/pgvectorscale -name '*.deb' | head -1)
if [ -n "$deb" ]; then
   dpkg -i "$deb" >/dev/null
else
   # Otherwise place the library and control files where PostgreSQL looks.
   lib=$(find /tmp/pgvectorscale -name 'vectorscale*.so' | head -1)
   [ -n "$lib" ] || { echo "no library in the archive"; exit 1; }
   install -m 0755 "$lib" "$(pg_config --pkglibdir)/"
   find /tmp/pgvectorscale \( -name 'vectorscale*.control' -o -name 'vectorscale*.sql' \) \
      -exec install -m 0644 {} "$(pg_config --sharedir)/extension/" \;
fi

pg_ctlcluster "$PG_MAJOR" main restart 2>/dev/null || true
for _ in $(seq 1 30); do
   pg_isready -q && break
   sleep 1
done
echo "installed pgvectorscale $VERSION for PostgreSQL $PG_MAJOR"
