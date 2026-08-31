#!/bin/bash
# Did the agent's query really retract a user-stated Class A fact?
#
# test-context-block.sh printed "A current" -> "A gone" and still reported PASS,
# because its only assertion is that the retraction scan answered -- it never
# asserts the user's fact SURVIVED. So the headline claim of that probe is
# unverified, and the observation needs settling directly.
#
# kb_handle_memory_context_block() passes FACT_AUTHORITY_MODEL structurally, so
# a Class A row at authority_rank 30 should be untouchable through this path.
# This measures the row itself, by id, before and after, rather than trusting a
# formatted state string that could be reporting a different row.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh
rc=0

RID="$($P "select id from rel_types where rel_type='email' limit 1")"

rows() {
  $P "select id || ' ' || confidence_class || ' rank=' || authority_rank
        || ' life=' || lifecycle_state
        || ' inval=' || case when invalidated_at='' then 'no' else 'YES' end
        || ' sup=' || case when superseded_at='' then 'no' else 'YES' end
        || ' suppressed=' || suppressed
      from entity_edges where source='user' and relation='email' order by id"
}

echo "=== seed one Class A row at authority_rank 30 (a real user fact) ==="
$P "ALTER TABLE entity_edges DISABLE TRIGGER USER" >/dev/null
$P "delete from entity_edges where source='user' and relation='email'" >/dev/null
$P "insert into entity_edges
      (source, relation, target, weight, relation_id, subject_kind, object_kind,
       edge_class, confidence_class, confidence, authority_rank, lifecycle_state,
       asserted_at, invalidated_at, superseded_at, suppressed)
    values ('user','email','theo@example.com', 1, ${RID}, 1, 11, 'semantic', 'A', 1.0,
       30, 'persistent',
       to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0)" >/dev/null
$P "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null
echo "  before:"
rows | sed 's/^/    /'

echo
echo "=== the agent's model-composed query asks to forget it ==="
curl -s -m 25 -H "Authorization: Bearer $B" -H 'content-type: application/json' \
     -X POST --data '{"query":"please forget my email","block_type":"general","limit":3}' \
     http://127.0.0.1:8741/v1/actions/memory.context_block >/dev/null
sleep 2
echo "  after:"
rows | sed 's/^/    /'

live="$($P "select count(*) from entity_edges where source='user' and relation='email'
              and invalidated_at='' and superseded_at='' and suppressed=0")"
echo
echo "  live user 'email' facts after: ${live:-0}"

if [ "${live:-0}" -ge 1 ]; then
  echo "PASS: the user's Class-A fact survived the agent's query, so"
  echo "      get_context_block did not retract at user authority"
else
  echo "FAIL: the agent's own query retracted a user-stated Class-A fact."
  echo "      kb_handle_memory_context_block passes FACT_AUTHORITY_MODEL, so if"
  echo "      this is real the retraction is not honouring the authority it is given."
  rc=1
fi
exit $rc
