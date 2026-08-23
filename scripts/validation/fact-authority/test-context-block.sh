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
  # RETURNING id, not max(id) afterwards.
  #
  # max(id) was a second race on top of the one state() already fixes: the drain
  # runs continuously now and can insert its own ('user','email') row between
  # this INSERT and the follow-up query, so the "id of the row we just wrote"
  # came back as the drain's row instead. The probe then watched a Class-A row it
  # did not create and reported "B current -> A current" -- a failure message
  # describing two different rows. Taking the id from the insert itself cannot
  # pick up anyone else's write.
  local id
  id="$($P "insert into entity_edges
        (source, relation, target, weight, relation_id, subject_kind, object_kind,
         edge_class, confidence_class, confidence, authority_rank, lifecycle_state,
         asserted_at, invalidated_at, superseded_at, suppressed)
      values ('user','email','theo@example.com', 1, ${RID}, 1, 11, 'semantic', '$1', $2,
         ${rank}, 'persistent',
         to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0)
      returning id")"
  $P "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null
  printf '%s' "$id"
}

# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", which makes a successful retraction
# read as a blocked one.
# BY ID, because this store is no longer quiet.
#
# This used to select every ('user','email') row, which was fine while the
# typed-fact layer was gated off and nothing else wrote. Retiring that gate means
# kb_memory_facts_drain() actually runs, and it legitimately re-asserts facts
# from stored memories WHILE this probe is mid-assertion -- so the seeded Class-B
# row was withdrawn correctly and a freshly drained Class-A row appeared beside
# it, and the multi-row match reported "B current -> A current" as a failure.
#
# The drain doing its job is the fix working. The probe just has to measure the
# row it owns instead of everything that shares the selector.
state() { $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where id=${1:-0}"; }

# The retraction pre-scan runs through the memory module's EXTRACT_INDEX stage
# (5889). When that stage does not answer, db2_typed_fact_ingress logs
# "retraction scan gave no answer; not retracting this turn" and returns without
# ever reaching the authority decision -- so both facts survive and this test
# passes while proving nothing. That is the same false-positive shape as a probe
# refused at an auth wall, so it is checked rather than assumed.
# `grep -c` PRINTS 0 and EXITS 1 when there are no matches, so the old
# `|| echo 0` appended a SECOND line and this captured "0\n0". The comparison
# below then died with "integer expression expected" and the `if` fell through
# to PASS -- meaning this guard, whose whole job is to catch a false positive,
# was itself broken in exactly the healthy case where the log has no such lines.
# head -1 keeps one number.
scans="$(grep -ac 'retraction scan gave no answer' /root/kb.log 2>/dev/null | head -1)"; scans="${scans:-0}"

ask_forget() {
  curl -s -m 20 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data '{"query":"please forget my email","block_type":"general","limit":3}' \
       http://127.0.0.1:8741/v1/actions/memory.context_block >/dev/null
}

echo "=== the agent's own query says \"please forget my email\" ==="
echo
a_id="$(seed A 1.0)"
a_before="$(state "$a_id")"
echo "  user-stated (Class A) id=$a_id before: $a_before"
ask_forget
a_after="$(state "$a_id")"
echo "  after the agent's query:              $a_after"
echo
b_id="$(seed B 0.6)"
b_before="$(state "$b_id")"
echo "  model-authored (Class B) id=$b_id before: $b_before"
ask_forget
b_after="$(state "$b_id")"
echo "  after the same query:                  $b_after"

echo
rc=0
after="$(grep -ac 'retraction scan gave no answer' /root/kb.log 2>/dev/null | head -1)"; after="${after:-0}"
if [ "${after:-0}" -gt "${scans:-0}" ]; then
  echo "FAIL: the retraction scan gave no answer during this run ($scans -> $after)."
  echo "      Both facts survived because the scan never fired, NOT because"
  echo "      authority was withheld. This run proves nothing about gap 1."
  exit 1
fi
echo "  (the scan answered, so what follows reflects an authority decision)"

# ASSERT THE OUTCOME, not just that the machinery ran.
#
# This script used to stop at the line above: its ONLY assertion was that the
# retraction scan had fired, and it never checked what happened to the facts. So
# it printed PASS while the Class-A row went "current" -> "gone" -- reporting
# success for the very defect it was written to catch. The header calls this
# path the sharpest form of gap 1, and the probe was blind to it.
#
# The real defect it was hiding: db2_typed_fact_ingress() tried
# db2_fact_actor_from_request() FIRST and used the declared authority only as a
# fallback. That function returns FACT_ACTOR_USER for any authenticated
# principal, so the FACT_AUTHORITY_MODEL that kb_handle_memory_context_block()
# passes on purpose was discarded whenever a request context existed -- which is
# always, for an MCP tool call inside a human's authenticated turn.
case "$a_after" in
  *current*)
    echo "PASS: the user's Class-A fact SURVIVED the agent's query" ;;
  *)
    echo "FAIL: the agent's own model-composed query retracted a user-stated"
    echo "      Class-A fact ($a_before -> $a_after). That is gap 1."
    rc=1 ;;
esac
# The Class-B row MUST still be withdrawn, or "the Class-A fact survived" is
# equally explained by the retraction path being dead.
case "$b_after" in
  *gone*)
    echo "PASS: the model-authored Class-B fact was withdrawn, so the retraction"
    echo "      path is live and the Class-A survival above means something" ;;
  *)
    echo "FAIL: the Class-B fact was NOT withdrawn ($b_before -> $b_after), so the"
    echo "      retraction path is not doing anything and this run proves nothing."
    rc=1 ;;
esac
exit $rc
