#!/bin/bash
# Does the P6 epistemic-kind migration survive a database that already holds
# semantic facts?
#
# THE BUG. schema.sql ran an unscoped
#
#     UPDATE entity_edges SET epistemic_kind='world_fact';
#
# inside the one-shot P6 block. entity_edges carries entity_edges_semantic_guard,
# which raises 'semantic facts must be changed through fact_mutation' for any
# INSERT or UPDATE of an edge_class='semantic' row outside an open commit. The
# ALTER immediately above already gives every row that value, so the statement
# rewrote what each row already had -- and every semantic row tripped the guard.
# The apply is ONE TRANSACTION, so the whole schema rolled back and aimee-kb
# never became ready:
#
#     aimee: db2_init: schema apply failed: ERROR: semantic facts must be
#     changed through fact_mutation
#     aimee-kb: DB2 not ready (...); retry 13/24 in 5s
#
# and the server, three layers away, answered "failed to store memory".
#
# WHY IT WAS INVISIBLE. On an EMPTY database the UPDATE touches no rows, so CI
# against a fresh template passes. It fires on any database that already holds
# semantic facts -- that is, every real deployment, on every upgrade.
#
# THE TEST. Reproduce the exact condition: semantic edges present, migration
# marker cleared so the one-shot block actually runs. Then start the kb.
#
# The marker check at the end is what makes this falsifiable. Without it, a kb
# that started because the block was SKIPPED would look identical to a kb that
# started because the block SUCCEEDED -- and skipping is precisely what the
# workaround did.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
P() { PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared -Atc "$1" 2>&1 | grep -v '^perl'; }
rc=0

edges="$(P "select count(*) from entity_edges where edge_class='semantic'")"
echo "semantic edges in the store: ${edges:-0}"
if [ "${edges:-0}" -lt 1 ]; then
  echo "FAIL: no semantic facts, so the guard cannot fire and this test is vacuous" >&2
  echo "      (that is exactly why CI against an empty template never caught it)" >&2
  exit 1
fi

echo "clearing the one-shot marker so the P6 block runs again"
P "delete from kb_meta where key='epistemic_kind_v1_migrated'" >/dev/null
before="$(P "select count(*) from kb_meta where key='epistemic_kind_v1_migrated'")"
echo "  marker rows before: ${before:-0}"
[ "${before:-0}" = "0" ] || { echo "FAIL: could not clear the marker" >&2; exit 1; }

echo
echo "restarting aimee-kb (the schema applies at startup)"
bash /root/start-kb.sh >/dev/null 2>&1

ready=0
for _ in $(seq 1 30); do
  if curl -s -m 5 http://127.0.0.1:8741/v1/health >/dev/null 2>&1; then ready=1; break; fi
  sleep 2
done

echo
if [ "$ready" = "1" ]; then
  echo "PASS: aimee-kb became ready with semantic facts present"
else
  echo "FAIL: aimee-kb never became ready"
  grep -a "schema apply failed\|DB2 not ready" /root/kb.log | tail -3
  rc=1
fi

after="$(P "select count(*) from kb_meta where key='epistemic_kind_v1_migrated'")"
echo "  marker rows after: ${after:-0}"
if [ "${after:-0}" = "1" ]; then
  echo "PASS: the one-shot block RAN to completion (it set the marker itself)"
else
  echo "FAIL: the marker is absent -- the block did not complete, so a kb that"
  echo "      started did so by skipping the migration, not by surviving it"
  rc=1
fi

echo
echo "epistemic_kind on semantic edges:"
P "select epistemic_kind, count(*) from entity_edges where edge_class='semantic' group by 1"
exit $rc
