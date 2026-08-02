#!/bin/bash
# Remove the throwaway database and binary the Postgres retraction test used.
# CT 132 is a shared aimee test container, so this leaves it as it was found.
pct exec 132 -- rm -f /tmp/rt-pg
pct exec 132 -- su - postgres -c "psql -tAc 'DROP DATABASE IF EXISTS aimee_retract_probe'"
rm -f /tmp/rt-pg
echo "--- databases remaining:"
pct exec 132 -- su - postgres -c "psql -tAc 'SELECT datname FROM pg_database'"
