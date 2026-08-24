#!/bin/sh
# Create the two databases the e2e run needs, on the container's PostgreSQL.
#
#   aimee_e2e_store  the server's store, reached by the aimee module through
#                    the postgres module
#   aimee_e2e_kb     DB2, with pgvector, for aimee-kb
#
# BOTH UTF8 EXPLICITLY. The container initdb's SQL_ASCII, where char_length and
# octet_length are the same function, so a byte-versus-character constraint
# passes there whether or not it is right. Production runs initdb
# --encoding=UTF8; matching it is what makes the run mean anything.
set -eu

psql() { su postgres -c "psql $*"; }

psql '-q -c "DROP DATABASE IF EXISTS aimee_e2e_store"'
psql '-q -c "DROP DATABASE IF EXISTS aimee_e2e_kb"'

# TEMPLATE template0 because template1 is SQL_ASCII and a database inherits its
# template's encoding. The locale is left at the template's rather than forced:
# the cluster has no C.UTF-8 generated, and encoding is the property that matters
# here.
psql '-q -c "CREATE DATABASE aimee_e2e_store ENCODING '"'"'UTF8'"'"' TEMPLATE template0"'
psql '-q -c "CREATE DATABASE aimee_e2e_kb ENCODING '"'"'UTF8'"'"' TEMPLATE template0"'
psql '-q -d aimee_e2e_kb -c "CREATE EXTENSION IF NOT EXISTS vector"'

echo "== databases =="
su postgres -c "psql -tAc \"SELECT datname || ' ' || pg_encoding_to_char(encoding) FROM pg_database WHERE datname LIKE 'aimee_e2e%'\""
echo "== pgvector in the kb database =="
su postgres -c "psql -tAc \"SELECT extname || ' ' || extversion FROM pg_extension WHERE extname = 'vector'\" -d aimee_e2e_kb"
