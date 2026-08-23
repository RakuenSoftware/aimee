#!/bin/bash
# Build a database with nothing in it but the schema, so the live suites can be
# run against a world they did not help create.
#
#   bash scripts/db2_fresh_database.sh
#   AIMEE_DB2_URL=postgres://aimee:aimee@<ct-ip>:5432/aimee_db2_fresh_probe \
#     go test -C server-go ./modules/db2/ ./modules/postgres/
#
# Run it before believing a live pass. The suites also run against the replay's
# database, which the replay seeds and which accumulates whatever earlier runs
# left behind; passing there and passing here are different claims.
#
# prospective_set_state passed for months on a row some earlier run had left in
# the shared database, and started failing only when the replay recreated it.
# That is a test passing for a reason it does not state, where the reason lives
# outside the repository. The way to find the rest is to take the reason away.
#
# Deliberately NOT the replay's database: the replay seeds fixtures of its own,
# which is reproducible state rather than ambient state, and mixing the two
# would answer neither question.
set -eu

PVE=${DB2_REPLAY_PVE:-root@192.168.1.252}
CT=${DB2_REPLAY_CT:-9001}
DB=${FRESH_DB:-aimee_db2_fresh_probe}
EMBED_DIM=${EMBED_DIM:-384}

on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PVE" "$@"; }

say() { printf 'fresh-db: %s\n' "$1" >&2; }

say "recreating $DB"
on_host "pct exec $CT -- su postgres -c \"psql -XAt -c 'DROP DATABASE IF EXISTS $DB'\"" >/dev/null
on_host "pct exec $CT -- su postgres -c \"psql -XAt -c \\\"CREATE DATABASE $DB OWNER aimee ENCODING 'UTF8' TEMPLATE template0\\\"\"" >/dev/null

say "applying the schema at dim $EMBED_DIM"
sed "s/__EMBED_DIM__/$EMBED_DIM/g" src/modules/db2/c/schema.sql |
  on_host "pct exec $CT -- bash -c 'cat > /tmp/fresh-schema.sql'"
on_host "pct exec $CT -- su postgres -c 'psql -XAt -v ON_ERROR_STOP=1 -d $DB -f /tmp/fresh-schema.sql'" \
  >/dev/null 2>&1 || { say "schema apply failed"; exit 1; }

say "ready: $DB"
