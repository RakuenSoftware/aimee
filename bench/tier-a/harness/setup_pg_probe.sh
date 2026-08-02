#!/bin/bash
# Create the throwaway database the opt-in Postgres integration tests use.
# CT 132 is a shared aimee test container; cleanup_pg_probe.sh removes this again
# so the container is left as it was found.
pct exec 132 -- su - postgres -c "psql -tAc 'DROP DATABASE IF EXISTS aimee_retract_probe'"
pct exec 132 -- su - postgres -c "psql -tAc 'CREATE DATABASE aimee_retract_probe'"
pct exec 132 -- su - postgres -c "psql -d aimee_retract_probe -tAc 'CREATE EXTENSION IF NOT EXISTS vector'"
echo READY
