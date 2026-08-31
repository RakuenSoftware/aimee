#!/bin/bash
# Seed the typed-fact layer with the two rows the authority tests act on:
# a user-stated Class-A fact and a model-inferred Class-B one, both on
# `works_for`, which is a FUNCTIONAL (single-valued) relation.
#
# Seeded by SQL on purpose. An upgrade is exactly this situation: Class-A rows
# already in the store, written by an earlier build, that the new code must
# protect.
#
# THREE THINGS THIS HAS TO GET RIGHT, each of which failed silently once:
#
# 1. entity_edges carries MORE THAN ONE write guard, and each refuses a raw
#    INSERT or DELETE of an edge_class='semantic' row outside an open
#    fact_mutation commit: entity_edges_semantic_guard ("semantic facts must be
#    changed through fact_mutation") and semantic_evidence_event_guard
#    ("semantic assertion mutation committed without its evidence event").
#    Suspending one leaves the other, so this suspends every user trigger on the
#    table and restores them immediately -- appropriate here, because the whole
#    premise is rows that predate the current code. The seed's writes were going
#    to /dev/null, so the refusals were invisible and the script reported success
#    while changing nothing.
#
# 2. authority_rank, not confidence_class, is what db2_fact_mutation_invalidate
#    actually gates on (`if (actor->rank < rows[i].authority_rank) continue`).
#    Seeding 'A' without the matching rank leaves a row that LOOKS Class A and
#    is retractable by anyone -- a test that passes while protecting nothing.
#    FACT_ACTOR_MODEL=10, FACT_ACTOR_USER=30 (fact_mutation.h).
#
# 3. A retired fact is lifecycle_state='invalidated' + invalidated_at, NOT
#    superseded_at/suppressed. Reporting state by the old predicate calls an
#    already-invalidated row "current", so a re-run looks freshly seeded when it
#    is untouched.
#
# Run AS ROOT in the container.
set -u
P=/root/psql.sh

# relation_id must resolve against the seeded ontology or recall skips the row.
RID="$($P "select id from rel_types where rel_type='works_for' limit 1" | tail -1)"
echo "works_for relation_id=${RID}"

$P "ALTER TABLE entity_edges DISABLE TRIGGER USER" >/dev/null

$P "delete from entity_edges where source in ('alice','bob')" >/dev/null

$P "insert into entity_edges
      (source, relation, target, weight, relation_id, subject_kind, object_kind,
       edge_class, confidence_class, confidence, authority_rank, lifecycle_state,
       asserted_at, invalidated_at, superseded_at, suppressed)
    values
      ('alice','works_for','acme', 1, ${RID}, 1, 5, 'semantic', 'A', 1.0, 30, 'persistent',
       to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0),
      ('bob','works_for','acme', 1, ${RID}, 1, 5, 'semantic', 'B', 0.6, 10, 'persistent',
       to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', '', 0)" >/dev/null

$P "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null

echo "--- seeded ---"
$P "select source, relation, target, confidence_class, confidence, authority_rank,
           lifecycle_state,
           case when superseded_at='' and invalidated_at='' and suppressed=0
                then 'current' else 'gone' end as state
      from entity_edges where source in ('alice','bob') order by source"

# A seed that did not land must not read as success -- that is how the silent
# guard refusals survived a full run.
n="$($P "select count(*) from entity_edges where source in ('alice','bob')
           and invalidated_at='' and superseded_at='' and suppressed=0" | tail -1)"
[ "${n:-0}" = "2" ] || { echo "FAIL: expected 2 live seeded rows, got ${n:-0}" >&2; exit 1; }
