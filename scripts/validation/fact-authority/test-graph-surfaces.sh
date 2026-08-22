#!/bin/bash
# The two graph surfaces that could not see the typed-fact layer.
#
# 1. relations.schema_list answered {"rows":[]} against a store holding a full
#    ontology, because db2_relation_schema_list read memory_relation_schema and
#    NOTHING in the tree writes that table. A reader would conclude the graph has
#    no relation schema at all.
#
# 2. memory.entity_profile answered "entity profile not found" for `user` and
#    `Dana`, both live sources in entity_edges, while returning a card with
#    relation_count 0 for an entity that had one mention and no typed facts. It
#    counted memory_relations only; typed facts live in entity_edges.
#
# Both are read surfaces, so both are checked against facts this script can see
# in the database directly -- otherwise "it returned something" is not evidence
# that it returned the RIGHT something.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
B="$(cat /root/kb-bearer.txt)"
KB=http://127.0.0.1:8741
P=/root/psql.sh
rc=0

kb() { curl -s -m 25 -H "Authorization: Bearer $B" -H 'content-type: application/json' \
            -X POST --data "$2" "$KB/v1/actions/$1"; }

echo "=== relations.schema_list ==="
# This used to be recorded rather than asserted, because the surface returned
# {"rows":[]} on every deployment and the reason was a data-model question I had
# not settled. It is settled now, and the answer was not "populate the table".
#
# Two wrong turns are worth keeping, because both look reasonable:
#
#   a) Serve the TYPED-FACT seed through it. Wrong: memory_relation_schema is
#      keyed by memory_relation_kind_t -- the CODE-GRAPH ontology (depends_on,
#      implements, fixes, calls, tests) -- a different vocabulary from the seed
#      (works_for, has_email, lives_in). Every seed relation mapped to
#      REL_OTHER(99), so the surface reported seventeen rows all saying "other":
#      worse than empty, because it looks like an answer.
#
#   b) Seed the TABLE from the code-graph ontology. Also wrong, just slower to
#      hurt: memory_ontology_validate() does not read that table. It enforces a
#      static C table. Seeding the database would create a second source of
#      truth for the same question, free to drift from the one that decides.
#
# The fix serves the surface from the enforcing table itself
# (memory_ontology_rules), and deletes the DB reader so there is only one
# answer. So this is now a real assertion: the surface must be non-empty, and
# must carry the code-graph vocabulary rather than a wall of REL_OTHER.
rows="$(kb relations.schema_list '{}')"
n="$(printf '%s' "$rows" | python3 -c '
import json,sys
try: print(len(json.load(sys.stdin).get("rows") or []))
except Exception: print(0)')"
echo "  rows returned: $n"
if [ "${n:-0}" -gt 0 ]; then
  echo "  PASS: the surface publishes the relation schema"
else
  echo "  FAIL: the surface is empty -- it is not reading the enforcing table"
  rc=1
fi
# Every published rule must name a real relation. A row whose relation_id is
# REL_OTHER(99) means a wrong ontology is being served; that was symptom (a).
bad="$(printf '%s' "$rows" | grep -c '"relation_id":99' || true)"
if [ "${bad:-0}" -gt 0 ]; then
  echo "  FAIL: rows are being reported as REL_OTHER(99) -- a wrong ontology is"
  echo "        being served through this surface"
  rc=1
else
  echo "  PASS: no row is REL_OTHER(99), so the vocabulary is the code graph's"
fi
# The published set must agree with what the validator enforces. The surface
# labels each code, so a row naming a relation the enum does not know would show
# up here as the literal "other" against a non-99 id.
mismatch="$(printf '%s' "$rows" | python3 -c '
import json,sys
try: rows = json.load(sys.stdin).get("rows") or []
except Exception: rows = []
print(sum(1 for r in rows
          if r.get("relation") == "other" and r.get("relation_id") != 99))')"
if [ "${mismatch:-0}" -eq 0 ]; then
  echo "  PASS: every row resolves to a named relation"
else
  echo "  FAIL: $mismatch row(s) carry a relation code the ontology cannot name"
  rc=1
fi

echo
echo "=== memory.entity_profile against the typed-fact graph ==="
# Pick an entity that exists ONLY as a live typed-fact endpoint. That is the
# case that used to 404; an entity with memory_entities mentions would have been
# found either way and would prove nothing.
ent="$($P "select e.source from entity_edges e
             where e.edge_class='semantic' and e.lifecycle_state in ('persistent','promoted')
               and e.superseded_at='' and e.invalidated_at='' and e.suppressed=0
               and not exists (select 1 from memory_entities m
                                 where lower(m.entity)=lower(e.source))
             limit 1" | tail -1)"
if [ -z "${ent:-}" ]; then
  echo "  SKIP: no entity exists only as a typed-fact endpoint here, so the"
  echo "        case that used to 404 cannot be staged. Nothing is claimed."
else
  edges="$($P "select count(*) from entity_edges
                 where (lower(source)=lower('$ent') or lower(target)=lower('$ent'))
                   and edge_class='semantic' and lifecycle_state in ('persistent','promoted')
                   and superseded_at='' and invalidated_at='' and suppressed=0" | tail -1)"
  echo "  entity: $ent   (live typed-fact edges: ${edges:-0}, memory_entities mentions: 0)"
  prof="$(kb memory.entity_profile "{\"entity\":\"$ent\"}")"
  echo "  $(printf '%s' "$prof" | head -c 200)"
  case "$prof" in
    *'"status":"ok"'*)
      got="$(printf '%s' "$prof" | python3 -c '
import json,sys
try: print((json.load(sys.stdin).get("profile") or {}).get("relation_count", -1))
except Exception: print(-1)')"
      if [ "${got:-0}" -ge "${edges:-1}" ]; then
        echo "  PASS: the profile exists and counts its typed-fact relations ($got)"
      else
        echo "  FAIL: profile found but relation_count $got < live edges ${edges:-0}"
        rc=1
      fi ;;
    *)
      echo "  FAIL: still 'not found' for an entity with live typed-fact edges"
      rc=1 ;;
  esac
fi
exit $rc
