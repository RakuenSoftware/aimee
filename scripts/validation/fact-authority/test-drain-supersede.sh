#!/bin/bash
# Gap 2 end to end, through the REAL commit path.
#
# With the memory module placed in kb it serves EXTRACT_INDEX and WRITE, so a
# stored note finally reaches db2_fact_commit for real: memory.store enqueues a
# memory_facts job, the drain runs the pattern extractor over the note, and each
# triple goes through the typed-fact write gate. That is the only production
# route to the functional-relation correction this fix guards.
#
# The relation has to be chosen with care, and a first pass at this got it
# wrong. It must be BOTH:
#   - functional (rel_type_is_functional), or the correction never applies and
#     two values legitimately accumulate; and
#   - what the extractor actually emits -- it turns "my X is Y" into rel_type X
#     verbatim, so "my hostname is ..." yields `hostname`, not `has_hostname`.
#   - a SEED relation, or the gate returns NOVEL and forces Class C whatever the
#     authority, which would mask exactly what is under test.
# `age` satisfies all three.
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh

RID="$($P "select id from rel_types where rel_type='age' limit 1")"

reset_age() {
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
  $P "delete from entity_edges where source='user' and relation='age'" >/dev/null
  $P "insert into entity_edges
        (source, relation, target, weight, relation_id, subject_kind, object_kind,
         edge_class, confidence_class, confidence, authority_rank, lifecycle_state,
         asserted_at, invalidated_at, superseded_at, suppressed)
      values ('user','age','30', 1, ${RID}, 1, 10, 'semantic', 'A', 1.0, 30, 'persistent',
         to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0)" >/dev/null
  $P "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null
}

# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", so a fact that was correctly
# retracted still reads as standing.
show() {
  $P "select '    ' || target || '  class=' || confidence_class ||
             case when superseded_at='' and invalidated_at='' and suppressed=0 then '  [current]' else '  [archived]' end
        from entity_edges where source='user' and relation='age' order by id"
}

store_http() {  # a note the MODEL wrote: bearer over loopback -> agent_message
  curl -s -m 20 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data "{\"key\":\"$1\",\"content\":\"$2\",\"tier\":\"L2\",\"kind\":\"fact\"}" \
       http://127.0.0.1:8741/v1/actions/memory.store >/dev/null
}

store_uds() {   # a note the USER wrote: kernel-attested peer -> user_stated
  curl -s -m 20 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
       -X POST --data "{\"key\":\"$1\",\"content\":\"$2\",\"tier\":\"L2\",\"kind\":\"fact\"}" \
       http://localhost/v1/memory/store >/dev/null
}

wait_for() {    # $1 = relation, $2 = expected row count
  for _ in $(seq 1 25); do
    n="$($P "select count(*) from entity_edges where source='user' and relation='$1'")"
    [ "${n:-0}" -ge "$2" ] && return 0
    sleep 3
  done
  return 1
}

$P "delete from memories where key like 'e2e-drain-%'" >/dev/null

echo "=== the user stated: user/age = 30 (Class A) ==="
reset_age
show

echo
echo "a note the MODEL wrote is stored, saying the age is 41:"
store_http e2e-drain-age "my age is 41"
echo "    provenance = $($P "select provenance_category from memories where key='e2e-drain-age' order by id desc limit 1")"
wait_for age 2 || true
sleep 6
echo
echo "current values of user/age after the drain:"
show
echo "  (expected: 30 [current] and ALONE -- the Class-B write must neither"
echo "   supersede the user's value nor sit beside it on a functional relation)"

echo
echo "=== positive control: the same path with no prior fact to outrank ==="
$P "delete from entity_edges where source='user' and relation='email'" >/dev/null
store_http e2e-drain-ctl "my email is ctl@example.com"
wait_for email 1 || true
echo "  committed by the drain through the real gate:"
$P "select '    ' || target || '  class=' || confidence_class
      from entity_edges where source='user' and relation='email' order by id"
echo "  (empty here means the drain never ran, and the result above proves nothing)"

echo
echo "=== the other direction: a USER-provenance note may mint Class A ==="
reset_age
$P "delete from entity_edges where source='user' and relation='age'" >/dev/null
store_uds e2e-drain-user "my age is 52"
echo "  stored provenance = $($P "select provenance_category from memories where key='e2e-drain-user' order by id desc limit 1")"
wait_for age 1 || true
show
echo "  (Class A here is provenance-to-authority working in the READ direction:"
echo "   the drain took USER authority from the row, not from a hardcoded constant)"

echo
echo "=== job outcomes ==="
$P "select '    ' || kind || ' ' || status || ' attempts=' || attempts
      from kb_async_jobs where kind='memory_facts' order by id desc limit 4"
