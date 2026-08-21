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
  $P "delete from entity_edges where source='user' and relation='email'" >/dev/null
  $P "insert into entity_edges
        (source, relation, target, weight, relation_id, subject_kind, object_kind,
         edge_class, confidence_class, confidence, asserted_at, superseded_at, suppressed)
      values ('user','email','theo@example.com', 1, ${RID}, 1, 11, 'semantic', '$1', $2,
         to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', 0)" >/dev/null
}

state() { $P "select confidence_class || ' ' || case when superseded_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='user' and relation='email'"; }

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
