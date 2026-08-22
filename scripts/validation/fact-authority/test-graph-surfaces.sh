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

echo "=== relations.schema_list (expected empty, and why) ==="
# NOT a bug to be fixed by serving the typed-fact seed here, which was tried and
# was wrong. memory_relation_schema is keyed by memory_relation_kind_t -- the
# CODE-GRAPH ontology (depends_on, implements, fixes, calls, tests) -- which is a
# different vocabulary from the typed-fact seed (works_for, has_email, lives_in).
# Feeding seed relations through this struct mapped every one of them to
# REL_OTHER (99), so the surface reported seventeen rows all saying "other":
# worse than empty, because it looks like an answer.
#
# The table is genuinely unpopulated for the code-graph ontology. Whether it
# should be populated, or the surface retired, is a data-model decision and is
# recorded rather than guessed at.
rows="$(kb relations.schema_list '{}')"
n="$(printf '%s' "$rows" | python3 -c '
import json,sys
try: print(len(json.load(sys.stdin).get("rows") or []))
except Exception: print(0)')"
echo "  rows returned: $n  (empty is the CURRENT truth, not a passing assertion)"
bad="$(printf '%s' "$rows" | grep -c '"relation_id":99' || true)"
if [ "${bad:-0}" -gt 0 ]; then
  echo "  FAIL: rows are being reported as REL_OTHER(99) -- a wrong ontology is"
  echo "        being served through this surface"
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
