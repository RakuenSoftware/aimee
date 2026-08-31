#!/bin/bash
# provision-pg-env.sh — stand up the Postgres environment the e2e suite needs.
# Run this INSIDE a throwaway Debian host or container, as root.
#
# The important detail, and the one that cost an hour the first time: do NOT
# hand-apply src/modules/db2/c/schema.sql with `psql -f`. It is a TEMPLATE. The
# service substitutes the __EMBED_DIM__ placeholder from the configured embedder
# width at init, and writes bookkeeping rows psql never will. A raw apply leaves
# kb_meta.schema_embedding_dim holding the literal string "__EMBED_DIM__", so
# aimee-kb refuses to start, and rel_types unseeded, so every typed-fact
# operation fails in a way that looks like a product bug.
#
# So: create an EMPTY database with the vector extension, and let aimee-kb apply
# the schema itself on first start.
set -euo pipefail

PGDB="${PGDB:-aimee_test}"
PGUSER="${PGUSER:-aimee}"
PGPASS="${PGPASS:-aimee}"
PGSTOREUSER="${PGSTOREUSER:-aimee_store_runtime}"
PGSTOREPASS="${PGSTOREPASS:-aimee-store-runtime}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  postgresql postgresql-contrib libpq-dev \
  build-essential pkg-config libssl-dev libcurl4-openssl-dev libpam0g-dev \
  libcjson-dev libyaml-dev uuid-dev zlib1g-dev libzstd-dev \
  python3 curl git rsync sqlite3 libsqlite3-dev

# The schema declares halfvec columns, so pgvector must exist before kb applies it.
PGMAJOR="$(psql --version | sed -E 's/.* ([0-9]+)\..*/\1/')"
apt-get install -y -qq "postgresql-${PGMAJOR}-pgvector"

systemctl start postgresql
sleep 3

su - postgres -c "psql -tAc \"SELECT 1 FROM pg_roles WHERE rolname='$PGUSER'\"" | grep -q 1 || \
  su - postgres -c "psql -c \"CREATE ROLE $PGUSER LOGIN PASSWORD '$PGPASS' SUPERUSER\""
su - postgres -c "psql -tAc \"SELECT 1 FROM pg_roles WHERE rolname='$PGSTOREUSER'\"" | grep -q 1 || \
  su - postgres -c "psql -c \"CREATE ROLE $PGSTOREUSER LOGIN PASSWORD '$PGSTOREPASS' \
    NOSUPERUSER NOCREATEDB NOCREATEROLE NOBYPASSRLS\""

su - postgres -c "dropdb --if-exists $PGDB"
su - postgres -c "createdb -O $PGUSER $PGDB"
su - postgres -c "psql -d $PGDB -c 'CREATE EXTENSION IF NOT EXISTS vector'"

# The Go daemon store enforces separate migration and runtime identities.  Its
# migrations are owned by PGUSER; default privileges make every newly-created
# object usable by the non-owner runtime without giving that role DDL rights.
PGPASSWORD="$PGPASS" psql -v ON_ERROR_STOP=1 -h 127.0.0.1 -U "$PGUSER" -d "$PGDB" <<SQL
GRANT CONNECT ON DATABASE $PGDB TO $PGSTOREUSER;
GRANT USAGE ON SCHEMA public TO $PGSTOREUSER;
ALTER DEFAULT PRIVILEGES FOR ROLE $PGUSER IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO $PGSTOREUSER;
ALTER DEFAULT PRIVILEGES FOR ROLE $PGUSER IN SCHEMA public
  GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO $PGSTOREUSER;
ALTER DEFAULT PRIVILEGES FOR ROLE $PGUSER IN SCHEMA public
  GRANT EXECUTE ON FUNCTIONS TO $PGSTOREUSER;
SQL

echo
echo "provisioned: postgresql://$PGUSER:***@127.0.0.1:5432/$PGDB"
echo "store runtime: postgresql://$PGSTOREUSER:***@127.0.0.1:5432/$PGDB"
PGPASSWORD="$PGPASS" psql -h 127.0.0.1 -U "$PGUSER" -d "$PGDB" -tAc 'SELECT version()'
echo
echo "next:"
echo "  1. build the tree:   cd src && make -j\$(nproc) server all"
echo "  2. start kb ONCE so it applies the schema (it needs an explicit port,"
echo "     and exits immediately without one):"
echo "         ./aimee-kb --http-port=8911"
echo "  3. run the suite:    tests/e2e/typed-facts-pg-e2e.sh"
echo
echo "for module-liveness-pg-e2e.sh or learning-loops-pg-e2e.sh, export:"
echo "  AIMEE_STORE_URL=postgresql://$PGSTOREUSER:$PGSTOREPASS@127.0.0.1:5432/$PGDB"
echo "  AIMEE_STORE_MIGRATION_URL=postgresql://$PGUSER:$PGPASS@127.0.0.1:5432/$PGDB"
