/* Remaining native graph mutation/rollback fixture. Ingestion, confidence
 * and candidate maintenance run against Go in fact_ingestion_parity_test.go. */
#include "../headers/aimee.h"
#include "../modules/db2/c/entity_edges.h"
#include "../modules/db2/c/fact_mutation.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "aimee/db2/graph_kinds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int scalar_int(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   int value = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int current_count(const char *entity)
{
   if (!entity || !entity[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT COUNT(*) FROM entity_edges"
                            " WHERE (source = ?1 OR target = ?2) AND edge_class = 'semantic'"
                            "   AND superseded_at = '' AND invalidated_at = '' AND suppressed = 0"
                            "   AND lifecycle_state IN ('persistent','promoted')";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   int c = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return c;
}

int main(void)
{
   db2_test_shim_open();
   /* Unified mutation seam: evidence is separate, lower-authority functional
    * contradictions are quarantined, review is undoable, and commits roll back
    * as batches. */
   fact_actor_t system_actor, model_actor;
   assert(db2_fact_actor_internal(FACT_ACTOR_SYSTEM, &system_actor) == 0);
   assert(db2_fact_actor_internal(FACT_ACTOR_MODEL, &model_actor) == 0);
   fact_actor_t operator_actor = {.rank = FACT_ACTOR_OPERATOR, .authenticated = 1};
   snprintf(operator_actor.principal, sizeof(operator_actor.principal), "test:operator");
   snprintf(operator_actor.role, sizeof(operator_actor.role), "operator");

   fact_evidence_input_t ev1 = {.source_kind = "message",
                                .source_id = "message:1",
                                .source_span = "bytes:0-12",
                                .evidence_hash = "hash-1",
                                .observed_at = "2026-01-01 00:00:00",
                                .ingest_run_id = "run:1"};
   fact_assertion_input_t ai = {.source = "jane",
                                .relation = "works_for",
                                .target = "acme",
                                .subject_kind = NODE_PERSON,
                                .object_kind = NODE_ORG,
                                .confidence_class = "B",
                                .confidence = 0.7,
                                .assertion_kind = FACT_KIND_WORLD_FACT,
                                .valid_from = "2026-01-01 00:00:00",
                                .evidence = &ev1,
                                .functional = 1};
   fact_mutation_result_t mr;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(mr.changed == 1 && strcmp(mr.lifecycle, FACT_LIFECYCLE_PERSISTENT) == 0);
   int64_t jane_acme = mr.assertion_id;

   fact_evidence_input_t ev2 = ev1;
   ev2.source_id = "document:2";
   ev2.evidence_hash = "hash-2";
   ai.evidence = &ev2;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(mr.evidence_added == 1);
   assert(scalar_int("SELECT COUNT(*) FROM fact_evidence WHERE source_id IN"
                     " ('message:1','document:2')") == 2);

   /* Functional incumbents are found through normalized (subject, relation)
    * identity. A correction whose subject only differs in case/spacing must
    * retire the old value rather than leave two current values behind. */
   fact_assertion_input_t normalized_functional = ai;
   normalized_functional.source = "surface-subject";
   normalized_functional.target = "old-value";
   normalized_functional.evidence = &ev1;
   assert(db2_fact_mutation_assert(&system_actor, &normalized_functional, &mr) == 0);
   int64_t normalized_incumbent = mr.assertion_id;
   normalized_functional.source = "  SURFACE-SUBJECT  ";
   normalized_functional.target = "new-value";
   assert(db2_fact_mutation_assert(&system_actor, &normalized_functional, &mr) == 0);
   char normalized_current_sql[512];
   snprintf(normalized_current_sql, sizeof(normalized_current_sql),
            "SELECT COUNT(*) FROM entity_edges WHERE identity_subject_key="
            "(SELECT identity_subject_key FROM entity_edges WHERE id=%lld)"
            " AND superseded_at='' AND invalidated_at='' AND suppressed=0",
            (long long)mr.assertion_id);
   assert(scalar_int(normalized_current_sql) == 1);
   char normalized_old_sql[256];
   snprintf(normalized_old_sql, sizeof(normalized_old_sql),
            "SELECT COUNT(*) FROM entity_edges WHERE id=%lld AND lifecycle_state='superseded'",
            (long long)normalized_incumbent);
   assert(scalar_int(normalized_old_sql) == 1);

   ai.target = "globex";
   ai.evidence = &ev1;
   assert(db2_fact_mutation_assert(&model_actor, &ai, &mr) == 0);
   assert(mr.quarantined == 1 && strcmp(mr.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   assert(current_count("jane") == 1); /* incumbent only; candidate is not recallable */
   int64_t candidate_id = mr.assertion_id;
   assert(db2_fact_mutation_review(&operator_actor, candidate_id, FACT_REVIEW_APPROVE, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_PROMOTED) == 0);
   char approve_commit[FACT_COMMIT_ID_MAX];
   snprintf(approve_commit, sizeof(approve_commit), "%s", mr.commit_id);
   assert(current_count("jane") == 1); /* approved value superseded incumbent */
   assert(db2_fact_mutation_review(&operator_actor, candidate_id, FACT_REVIEW_UNDO, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   char undo_commit[FACT_COMMIT_ID_MAX];
   snprintf(undo_commit, sizeof(undo_commit), "%s", mr.commit_id);
   assert(current_count("jane") == 1); /* undo restored incumbent */

   /* Human rejection is durable negative memory.  A later extraction with
    * different evidence must not resurrect the exact value; only an explicit
    * operator undo removes the tombstone. */
   ai.source = "refusal-survives";
   ai.target = "acme";
   ai.evidence = &ev1;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   int64_t rejected_assertion_id = mr.assertion_id;
   assert(db2_fact_mutation_review(&operator_actor, rejected_assertion_id, FACT_REVIEW_REJECT,
                                   &mr) == 0);
   fact_evidence_input_t reextract_ev = ev1;
   reextract_ev.source_id = "document:reextracted";
   reextract_ev.evidence_hash = "hash-reextracted";
   ai.evidence = &reextract_ev;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == FACT_MUTATION_TOMBSTONED);
   assert(scalar_int("SELECT COUNT(*) FROM memory_rejection_tombstones"
                     " WHERE object_kind='fact' AND source='refusal-survives'"
                     " AND relation='works_for' AND target='acme' AND active=1") == 1);
   assert(db2_fact_mutation_review(&operator_actor, rejected_assertion_id, FACT_REVIEW_UNDO, &mr) ==
          0);
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);

   ai.source = "jane";
   ai.target = "globex";
   ai.evidence = &ev1;

   /* Rolling the latest transition creates a revert commit.  That neutralizing
    * commit must not itself become a descendant that makes walking the prior
    * decision backward impossible. */
   char stacked_rollback[FACT_COMMIT_ID_MAX];
   int stacked_rc = db2_fact_commit_rollback(&operator_actor, undo_commit, stacked_rollback);
   assert(stacked_rc > 0);
   assert(scalar_int("SELECT COUNT(*) FROM entity_edges WHERE id="
                     " (SELECT MAX(id) FROM entity_edges WHERE source='jane'"
                     " AND target='globex') AND lifecycle_state='promoted'") == 1);
   assert(db2_fact_commit_rollback(&operator_actor, approve_commit, stacked_rollback) > 0);
   assert(scalar_int("SELECT COUNT(*) FROM entity_edges WHERE id="
                     " (SELECT MAX(id) FROM entity_edges WHERE source='jane'"
                     " AND target='globex') AND lifecycle_state='candidate'") == 1);

   ai.source = "rollback-subject";
   ai.target = "acme";
   ai.evidence = &ev1;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   char asserted_commit[FACT_COMMIT_ID_MAX];
   snprintf(asserted_commit, sizeof(asserted_commit), "%s", mr.commit_id);
   fact_commit_change_t preview[4];
   assert(db2_fact_commit_preview(asserted_commit, preview, 4) == 1);
   assert(preview[0].assertion_id == mr.assertion_id && strcmp(preview[0].action, "insert") == 0);
   char rollback_commit[FACT_COMMIT_ID_MAX];
   assert(db2_fact_commit_rollback(&operator_actor, asserted_commit, rollback_commit) == 1);
   assert(rollback_commit[0] != '\0' && current_count("rollback-subject") == 0);

   /* An async delivery retry carries the same immutable evidence mention. It
    * must be a true no-op after rollback, rather than treating the replay as a
    * new correction that resurrects the invalidated assertion. The retry uses
    * compatibility-width letters, case and surrounding whitespace so this also
    * proves normalized identity cannot route around the tombstone. */
   int commits_before_replay = scalar_int("SELECT COUNT(*) FROM fact_graph_commits");
   ai.source = "  ROLLBACK-SUBJECT  ";
   ai.target = "ＡＣＭＥ";
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(mr.assertion_id > 0 && mr.changed == 0 && mr.evidence_added == 0);
   assert(mr.commit_id[0] == '\0');
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);
   assert(current_count("rollback-subject") == 0);
   assert(scalar_int("SELECT COUNT(*) FROM fact_graph_commits") == commits_before_replay);

   /* A later drain that re-extracts the same triple from NEW text is not a
    * replay, so the evidence guard above does not apply -- and unlike
    * db2_fact_mutation_invalidate, rollback writes no rejection tombstone. The
    * only thing left standing between a rolled-back insertion and its return is
    * the reactivate gate, which compares the incoming actor against the row's
    * authority_rank. Unless the rollback recorded the operator's authority there,
    * that rank is still the original asserter's and SYSTEM re-extraction wins. */
   fact_evidence_input_t rb_reextract_ev = ev1;
   rb_reextract_ev.source_id = "message:rollback-reextract";
   rb_reextract_ev.evidence_hash = "hash-rollback-reextract";
   ai.source = "rollback-subject";
   ai.target = "acme";
   ai.evidence = &rb_reextract_ev;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);
   assert(current_count("rollback-subject") == 0);
   ai.evidence = &ev1;

   /* One ingest-run id groups independently committed assertions into one
    * previewable, all-or-nothing rollback. */
   fact_evidence_input_t batch_ev = ev1;
   batch_ev.ingest_run_id = "run:atomic-batch";
   batch_ev.actor_principal = "identity:user:alice";
   batch_ev.source_id = "message:batch-a";
   batch_ev.evidence_hash = "hash-batch-a";
   ai.source = "batch-a";
   ai.target = "acme";
   ai.evidence = &batch_ev;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   batch_ev.source_id = "message:batch-b";
   batch_ev.evidence_hash = "hash-batch-b";
   ai.source = "batch-b";
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   fact_commit_change_t batch_preview[8];
   assert(db2_fact_ingest_run_preview("run:atomic-batch", batch_preview, 8) == 2);
   assert(scalar_int("SELECT COUNT(*) FROM fact_evidence WHERE ingest_run_id='run:atomic-batch'"
                     " AND actor_principal='identity:user:alice'") == 2);
   assert(scalar_int("SELECT COUNT(*) FROM entity_mental_models"
                     " WHERE entity IN ('batch-a','batch-b')") == 2);
   assert(db2_fact_ingest_run_rollback(&operator_actor, "run:atomic-batch", rollback_commit) == 2);
   assert(current_count("batch-a") == 0 && current_count("batch-b") == 0);
   assert(scalar_int("SELECT COUNT(*) FROM fact_evidence WHERE ingest_run_id='run:atomic-batch'"
                     " AND invalidated_at<>''") == 2);
   assert(scalar_int("SELECT COUNT(*) FROM entity_mental_models"
                     " WHERE entity IN ('batch-a','batch-b')") == 0);

   /* Mental models are derived views only: callers cannot insert an unsourced
    * mental-model replacement row. */
   ai.source = "derived-only";
   ai.assertion_kind = FACT_KIND_MENTAL_MODEL;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == -1);
   ai.assertion_kind = FACT_KIND_WORLD_FACT;

   /* The schema trigger rejects semantic SQL that bypasses an open graph
    * commit, including direct inserts. */
   {
      char err[256] = "";
      aimee_pg_stmt_t *direct =
          aimee_pg_prepare(db2_conn(),
                           "INSERT INTO entity_edges(source,relation,target,edge_class)"
                           " VALUES('bypass','works_for','acme','semantic')",
                           err, sizeof(err));
      assert(direct);
      assert(aimee_pg_step(direct, err, sizeof(err)) == AIMEE_PG_ERR);
      aimee_pg_finalize(direct);
   }

   fact_erasure_impact_t impact;
   assert(db2_fact_erasure_preview("jane", "works_for", "acme", &impact) == 0);
   assert(impact.assertion_count == 1 && impact.evidence_count == 2);
   char erase_commit[FACT_COMMIT_ID_MAX];
   assert(db2_fact_erasure_execute(&operator_actor, "jane", "works_for", "acme", &impact,
                                   erase_commit) == 1);
   assert(erase_commit[0] != '\0');
   assert(scalar_int("SELECT COUNT(*) FROM entity_edges WHERE id="
                     " (SELECT MAX(id) FROM entity_edges WHERE source='jane' AND target='acme')") ==
          0);
   assert(scalar_int("SELECT COUNT(*) FROM kb_audit_outbox WHERE action LIKE 'fact.%'") > 0);
   (void)jane_acme;

   /* bad args / no-op. */
   /* Preserve the native graph read boundary formerly covered by the removed
    * ingestion fixture: co-occurrence lists and walks must exclude candidates. */
   int added = 0;
   assert(db2_entity_edge_upsert("graph-parity", "co_seen_with", "neighbor", 0, 0, 0, 0, &added) ==
          0);
   fact_evidence_input_t graph_ev = {.source_kind = "observation", .source_id = "graph-parity"};
   fact_assertion_input_t graph_input = {.source = "graph-parity",
                                         .relation = "works_for",
                                         .target = "candidate-org",
                                         .subject_kind = NODE_PERSON,
                                         .object_kind = NODE_ORG,
                                         .confidence_class = "B",
                                         .confidence = 0.6,
                                         .assertion_kind = FACT_KIND_WORLD_FACT,
                                         .evidence = &graph_ev};
   assert(db2_fact_mutation_assert(&model_actor, &graph_input, &mr) == 0);
   edge_t graph_rows[8];
   assert(db2_entity_edge_list_by_entity("graph-parity", graph_rows, 8) == 1);
   assert(strcmp(graph_rows[0].relation, "co_seen_with") == 0);
   assert(db2_entity_edge_search_by_token("graph-parity", graph_rows, 8, 8) == 1);
   assert(db2_entity_edge_walk_step("graph-parity", graph_rows, 8) == 1);
   db2_entity_neighbor_t graph_neighbors[8];
   assert(db2_entity_edge_neighbors("graph-parity", graph_neighbors, 8, 50) == 1);
   assert(strcmp(graph_neighbors[0].node, "neighbor") == 0);
   assert(db2_fact_mutation_review(&operator_actor, mr.assertion_id, FACT_REVIEW_REJECT, &mr) == 0);
   assert(db2_entity_edge_walk_step("graph-parity", graph_rows, 8) == 1);
   assert(db2_entity_edge_neighbors("graph-parity", graph_neighbors, 8, 50) == 1);
   assert(strcmp(graph_neighbors[0].node, "neighbor") == 0);

   assert(current_count("") == -1);

   db2_test_shim_close();
   printf("fact_lifecycle: all tests passed\n");
   return 0;
}
