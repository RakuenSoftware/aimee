#!/bin/bash
# Seed the typed-fact layer with the two rows the authority tests act on:
# a user-stated Class-A fact and a model-inferred Class-B one, both on
# `works_for`, which is a FUNCTIONAL (single-valued) relation.
#
# Seeded by SQL on purpose. An upgrade is exactly this situation: Class-A rows
# already in the store, written by an earlier build, that the new code must
# protect. Run AS ROOT in the container.
set -u
P=/root/psql.sh

# relation_id must resolve against the seeded ontology or recall skips the row.
RID="$($P "select id from rel_types where rel_type='works_for' limit 1" | tail -1)"
echo "works_for relation_id=${RID}"

$P "delete from entity_edges where source in ('alice','bob')" >/dev/null

$P "insert into entity_edges
      (source, relation, target, weight, relation_id, subject_kind, object_kind,
       edge_class, confidence_class, confidence, asserted_at, superseded_at, suppressed)
    values
      ('alice','works_for','acme', 1, ${RID}, 1, 5, 'semantic', 'A', 1.0,
       to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', 0),
      ('bob','works_for','acme', 1, ${RID}, 1, 5, 'semantic', 'B', 0.6,
       to_char(now() at time zone 'UTC','YYYY-MM-DD HH24:MI:SS'), '', 0)" >/dev/null

echo "--- seeded ---"
$P "select source, relation, target, confidence_class, confidence,
           case when superseded_at='' then 'current' else 'superseded' end as state, suppressed
      from entity_edges where source in ('alice','bob') order by source"
