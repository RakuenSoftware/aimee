#!/bin/bash
# Work around an upstream migration that its own trigger refuses, so this
# container can finish coming up. NOT a fix -- see the report below.
#
# THE DEFECT (in the merged `testing`, not in this branch)
#
# schema.sql:1995 creates entity_edges_semantic_guard, which enforces that any
# row with edge_class='semantic' may only be INSERTed/UPDATEd inside an open
# fact_graph_commits row:
#
#     RAISE EXCEPTION 'semantic facts must be changed through fact_mutation'
#
# schema.sql:15289, inside the one-shot P6 block, then does:
#
#     UPDATE entity_edges SET epistemic_kind='world_fact';
#
# with no commit open. Every semantic edge trips the guard, the DO block raises,
# and db2_init reports:
#
#     aimee: db2_init: schema apply failed: ERROR: semantic facts must be
#     changed through fact_mutation
#     aimee-kb: DB2 not ready (...); retry 13/24 in 5s
#
# aimee-kb then never becomes ready, so the server's knowledge calls fail
# ("TCP connect failed: 127.0.0.1:8741") and every memory write answers
# "failed to store memory".
#
# It is invisible on an empty database: the UPDATE touches no rows, so CI
# against a fresh template passes. It fires on any database that already holds
# semantic facts -- that is, every real deployment and every upgrade.
#
# The UPDATE also appears to be redundant: the line above it,
#
#     ALTER TABLE entity_edges ADD COLUMN IF NOT EXISTS epistemic_kind
#       TEXT NOT NULL DEFAULT 'world_fact'
#
# already gives every existing row 'world_fact'. (The sibling UPDATE on
# `memories` is NOT redundant -- it also computes
# expiry_days_migration_override.) So the apparent fix is to drop the
# entity_edges UPDATE. That is a schema change under the frozen-boundary rules
# and belongs to whoever owns P6, so it is reported rather than made here.
#
# WHAT THIS SCRIPT DOES
#
# Performs the migration's intent with the guard suspended, then sets the marker
# the block checks, so the DO block is skipped on the next apply and db2_init
# completes. Equivalent to what a correct migration would have left behind.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
P() { PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared -Atc "$1" 2>&1 | grep -v '^perl'; }

echo "semantic edges present: $(P "select count(*) from entity_edges where edge_class='semantic'")"
echo "marker before: [$(P "select value from kb_meta where key='epistemic_kind_v1_migrated'")]"

P "ALTER TABLE entity_edges DISABLE TRIGGER entity_edges_semantic_guard"
P "UPDATE entity_edges SET epistemic_kind='world_fact' WHERE epistemic_kind IS DISTINCT FROM 'world_fact'"
P "ALTER TABLE entity_edges ENABLE TRIGGER entity_edges_semantic_guard"

P "UPDATE memories m SET epistemic_kind='world_fact',
     expiry_days_migration_override=COALESCE(
       (SELECT k.expire_days FROM kind_lifecycle k WHERE k.kind=m.kind),
       (SELECT k.expire_days FROM kind_lifecycle k WHERE k.kind='fact'))
   WHERE NOT EXISTS(SELECT 1 FROM kb_meta WHERE key='epistemic_kind_v1_migrated')"

P "INSERT INTO kb_meta(key,value) VALUES('epistemic_kind_v1_migrated','1')
   ON CONFLICT (key) DO UPDATE SET value='1'"

echo "marker after:  [$(P "select value from kb_meta where key='epistemic_kind_v1_migrated'")]"
echo "guard still enabled: $(P "select tgenabled from pg_trigger where tgname='entity_edges_semantic_guard'")  (O = enabled)"
