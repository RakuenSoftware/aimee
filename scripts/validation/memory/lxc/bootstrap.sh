#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl git gnupg build-essential pkg-config python3 python3-venv python3-dev libsqlite3-dev libssl-dev libzstd-dev zlib1g-dev libp11-kit-dev libpq-dev jq procps openssl
install -d /usr/share/postgresql-common/pgdg
curl -fsSL --retry 5 https://www.postgresql.org/media/keys/ACCC4CF8.asc -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
printf 'deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt trixie-pgdg main\n' > /etc/apt/sources.list.d/pgdg.list
apt-get update
apt-get install -y --no-install-recommends postgresql-18 postgresql-18-pgvector libpq-dev
mkdir -p /opt/aimee /opt/pg18
ln -s /usr/lib/postgresql/18/bin /opt/pg18/bin
pg_lsclusters
