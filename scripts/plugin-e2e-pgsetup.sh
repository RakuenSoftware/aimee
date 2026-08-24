#!/bin/sh
# Scoped: creates ONLY the plive role/database inside container 101.
# Nothing else in this postgres is touched.
set -u
PSQL=/usr/lib/postgresql/17/bin/psql
run() { su postgres -c "$PSQL -tAc \"$1\"" 2>&1; }

echo "=== available extensions of interest ==="
run "select name from pg_available_extensions where name in ('vector','pg_trgm','pgcrypto') order by 1"

echo "=== create scratch role/db (idempotent) ==="
run "drop database if exists aimee_plive"
run "drop role if exists plive"
run "create role plive login password 'plive_e2e_pw'"
run "create database aimee_plive owner plive"

echo "=== extensions into the scratch db ==="
su postgres -c "$PSQL -d aimee_plive -tAc \"create extension if not exists vector\"" 2>&1
su postgres -c "$PSQL -d aimee_plive -tAc \"create extension if not exists pg_trgm\"" 2>&1
su postgres -c "$PSQL -d aimee_plive -tAc \"grant all on schema public to plive\"" 2>&1

echo "=== verify as plive over TCP ==="
PGPASSWORD=plive_e2e_pw $PSQL -h 127.0.0.1 -U plive -d aimee_plive -tAc \
  "select 'connected as '||current_user||' to '||current_database()" 2>&1
