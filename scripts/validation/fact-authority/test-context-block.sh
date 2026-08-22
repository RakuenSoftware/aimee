#!/bin/bash
# The get_context_block path, which is the sharpest form of gap 1.
#
# This action runs a §4 retraction on the turn text before answering. Its `query`
# is a string the MODEL composed — get_context_block is an MCP tool the agent
# calls, choosing its own argument — yet the code retracted at a hardcoded
# FACT_AUTHORITY_USER. So an agent could delete a user-stated fact by writing
# "forget my email" into a query nobody asked it to make.
#
# Both classes are exercised against the SAME query, because "the fact survived"
# only means something if the retraction path is demonstrably live: the Class-B
# row must still be withdrawn by it.
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh

RID="$($P "select id from rel_types where rel_type='email' limit 1")"
[ -n "$RID" ] || RID="$($P "insert into rel_types (rel_type, status, sensitivity) values ('email','active','pii') returning id")"

seed() { # $1 = confidence_class  $2 = confidence
  # entity_edges carries MORE THAN ONE write guard -- entity_edges_semantic_guard
# and semantic_evidence_event_guard ("semantic assertion mutation committed
# without its evidence event"). Naming one leaves the other, so this suspends
# every user trigger on the table for the seed. They refuse raw INSERT/DELETE of a semantic edge
  # outside an open fact_mutation commit, and these writes are silenced -- so
  # without suspending it the seed changes nothing and the test still reports a
  # state, just the previous run's. Suspended only around the seed, which is the
  # legitimate case: rows standing in for what an earlier build wrote.
  # authority_rank is what retraction actually gates on (FACT_ACTOR_MODEL=10,
  # FACT_ACTOR_USER=30); a Class-A row without rank 30 protects nothing.
  $P "ALTER TABLE entity_edges DISABLE TRIGGER USER" >/dev/null
  local rank=10
  [ "$1" = "A" ] && rank=30
  $P "delete from entity_edges where source='user' and relation='email'" >/dev/null
  $P "insert into entity_edges
        (source, relation, target, weight, relation_id, subject_kind, object_kind,
         edge_class, confidence_class, confidence, authority_rank, lifecycle_state,
         asserted_at, invalidated_at, superseded_at, suppressed)
      values ('user','email','theo@example.com', 1, ${RID}, 1, 11, 'semantic', '$1', $2,
         ${rank}, 'persistent',
         to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0)" >/dev/null
  $P "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null
}

# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", which makes a successful retraction
# read as a blocked one.
state() { $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='user' and relation='email'"; }

# The retraction pre-scan runs through the memory module's EXTRACT_INDEX stage
# (5889). When that stage does not answer, db2_typed_fact_ingress logs
# "retraction scan gave no answer; not retracting this turn" and returns without
# ever reaching the authority decision -- so both facts survive and this test
# passes while proving nothing. That is the same false-positive shape as a probe
# refused at an auth wall, so it is checked rather than assumed.
scans="$(grep -ac 'retraction scan gave no answer' /root/kb.log 2>/dev/null || echo 0)"

ask_forget() {
  curl -s -m 20 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data '{"query":"please forget my email","block_type":"general","limit":3}' \
       http://127.0.0.1:8741/v1/actions/memory.context_block >/dev/null
}

echo "=== the agent's own query says \"please forget my email\" ==="
echo
seed A 1.0
echo "  user-stated (Class A) before: $(state)"
ask_forget
echo "  after the agent's query:      $(state)"
echo
seed B 0.6
echo "  model-authored (Class B) before: $(state)"
ask_forget
echo "  after the same query:            $(state)"

echo
after="$(grep -ac 'retraction scan gave no answer' /root/kb.log 2>/dev/null || echo 0)"
if [ "${after:-0}" -gt "${scans:-0}" ]; then
  echo "FAIL: the retraction scan gave no answer during this run ($scans -> $after)."
  echo "      Both facts survived because the scan never fired, NOT because"
  echo "      authority was withheld. This run proves nothing about gap 1."
  exit 1
fi
echo "PASS: the scan answered, so the surviving facts reflect an authority decision"
