/* test_fact_lifecycle.c: typed-fact §5 confidence classes + §4 correction/
 * retraction, against the sqlite shim. P3. */
#include "../headers/aimee.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/fact_mutation.h"
#include "../db2/rel_types_store.h"
#include "../db2/entity_edges.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "memory_ontology.h"
#include "memory_fact_gate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int found;
   char cls[8];
   double conf;
   char superseded[40];
   char invalidated[40];
   char lifecycle[24];
   int suppressed;
   char asserted[40];
} est_t;

/* Read the stored §4/§5 fields of a semantic edge (first match). */
static est_t edge_state(const char *src, const char *rel)
{
   est_t s;
   memset(&s, 0, sizeof(s));
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT confidence_class, confidence, superseded_at, suppressed, asserted_at,"
       " invalidated_at,lifecycle_state"
       " FROM entity_edges WHERE source = ?1 AND relation = ?2 AND edge_class = 'semantic' LIMIT 1",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", src);
   aimee_pg_bind_text(st, "?2", rel);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      s.found = 1;
      snprintf(s.cls, sizeof(s.cls), "%s", aimee_pg_column_text(st, 0));
      s.conf = aimee_pg_column_double(st, 1);
      snprintf(s.superseded, sizeof(s.superseded), "%s", aimee_pg_column_text(st, 2));
      s.suppressed = aimee_pg_column_int(st, 3);
      snprintf(s.asserted, sizeof(s.asserted), "%s", aimee_pg_column_text(st, 4));
      snprintf(s.invalidated, sizeof(s.invalidated), "%s", aimee_pg_column_text(st, 5));
      snprintf(s.lifecycle, sizeof(s.lifecycle), "%s", aimee_pg_column_text(st, 6));
   }
   aimee_pg_finalize(st);
   return s;
}

/* Force a semantic edge's asserted_at into the past so expiry can act on it. */
static void backdate(const char *src, const char *ts)
{
   void *conn = db2_conn();
   char err[256] = "";
   char cid[128];
   snprintf(cid, sizeof(cid), "test-backdate:%s", src);
   aimee_pg_stmt_t *open = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,"
       "authority_rank,status,reversible) VALUES(?1,'test.backdate','test','system',20,'open',1)",
       err, sizeof(err));
   assert(open);
   aimee_pg_bind_text(open, "?1", cid);
   assert(aimee_pg_step(open, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(open);
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE entity_edges SET asserted_at=?2,commit_id=?3"
                                          " WHERE source=?1 AND edge_class='semantic'",
                                          err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", src);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_text(st, "?3", cid);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
   st = aimee_pg_prepare(conn, "UPDATE fact_graph_commits SET status='applied' WHERE commit_id=?1",
                         err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", cid);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

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

static void test_pure(void)
{
   assert(fact_class_confidence("A") == 1.0);
   assert(fact_class_confidence("B") == 0.6);
   assert(fact_class_confidence("C") == 0.4);
   assert(fact_class_confidence("Z") == 0.4);
   assert(fact_class_confidence(NULL) == 0.4);
   assert(fact_class_rank("A") == 3 && fact_class_rank("B") == 2 && fact_class_rank("C") == 1);
   assert(fact_class_rank("x") == 0 && fact_class_rank(NULL) == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_USER, FACT_GATE_ACCEPT), "A") == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_MODEL, FACT_GATE_ACCEPT), "B") == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_MODEL, FACT_GATE_NOVEL), "C") == 0);
   /* a novel rel_type is speculation — Class C even from a user authority. */
   assert(strcmp(fact_class_for(FACT_AUTHORITY_USER, FACT_GATE_NOVEL), "C") == 0);
   printf("  PASS: test_pure\n");
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);
   test_pure();

   /* §5 class assignment via the commit point. Model ACCEPT -> Class B. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   est_t s = edge_state("alice", "works_for");
   assert(s.found && strcmp(s.cls, "B") == 0 && s.conf == 0.6);
   assert(strcmp(s.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   assert(s.superseded[0] == '\0' && s.suppressed == 0 && s.asserted[0] != '\0');

   /* A user assertion on the same triple upgrades B -> A (and confidence 1.0). */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   s = edge_state("alice", "works_for");
   assert(strcmp(s.cls, "A") == 0 && s.conf == 1.0);
   assert(strcmp(s.lifecycle, FACT_LIFECYCLE_PERSISTENT) == 0);

   /* A lower-authority re-commit never downgrades A. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   s = edge_state("alice", "works_for");
   assert(strcmp(s.cls, "A") == 0 && s.conf == 1.0);

   /* Model NOVEL -> Class C; a user NOVEL is still C (novel = speculation). */
   assert(db2_fact_commit("bob", NODE_PERSON, "frobnicates", "thing", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   assert(strcmp(edge_state("bob", "frobnicates").cls, "C") == 0);
   assert(db2_fact_commit("carol", NODE_PERSON, "wibbles", "y", NODE_OTHER, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_NOVEL);
   assert(strcmp(edge_state("carol", "wibbles").cls, "C") == 0);

   /* §5 promotion: a Class B confirmed >= threshold times becomes durable (0.8),
    * still Class B (never A). */
   for (int i = 0; i < 3; i++)
      assert(db2_fact_commit("dave", NODE_PERSON, "works_for", "ecorp", NODE_ORG,
                             FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(strcmp(edge_state("dave", "works_for").cls, "B") == 0);
   assert(edge_state("dave", "works_for").conf == 0.6);
   assert(db2_fact_promote_durable(3) >= 1);
   s = edge_state("dave", "works_for");
   assert(strcmp(s.cls, "B") == 0 && s.conf == 0.8); /* durable, not promoted to A */
   assert(strcmp(s.lifecycle, FACT_LIFECYCLE_PERSISTENT) == 0);
   assert(db2_fact_promote_durable(0) == -1); /* bad threshold */

   /* §5 expiry: an unconfirmed (weight 1) Class C aged past the TTL is superseded;
    * a confirmed (weight>=2) Class C survives. */
   backdate("bob", "2000-01-01 00:00:00");
   assert(strcmp(edge_state("bob", "frobnicates").lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   assert(db2_fact_expire_speculative(30) >= 1);
   assert(db2_fact_current_count("bob") == 0); /* superseded, no longer current */
   s = edge_state("bob", "frobnicates");
   assert(strcmp(s.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0 && s.invalidated[0] != '\0');
   assert(db2_fact_expire_speculative(0) == -1);
   /* confirm carol/wibbles (weight 2) then backdate: it must NOT expire. */
   assert(db2_fact_commit("carol", NODE_PERSON, "wibbles", "y", NODE_OTHER, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_NOVEL);
   backdate("carol", "2000-01-01 00:00:00");
   (void)db2_fact_expire_speculative(30);
   assert(db2_fact_current_count("carol") == 1); /* confirmed C survives */

   /* §4/§5 retract authority guard: alice/works_for was upgraded to Class A by the
    * user commit above, so a MODEL retraction must NOT touch it (a model may not
    * delete a user-stated fact) — it returns 0 and the fact stands. A USER
    * retraction then supersedes it (row retained, not suppressed). */
   assert(db2_fact_current_count("alice") == 1);
   assert(db2_fact_retract("alice", "works_for", NULL, FACT_AUTHORITY_MODEL) == 0);
   assert(db2_fact_current_count("alice") == 1); /* model refused: Class A stands */
   assert(db2_fact_retract("alice", "works_for", NULL, FACT_AUTHORITY_USER) >= 1);
   assert(db2_fact_current_count("alice") == 0);
   s = edge_state("alice", "works_for");
   assert(s.superseded[0] == '\0' && s.suppressed == 0 && s.invalidated[0] != '\0');
   assert(strcmp(s.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);

   /* A user retraction must outrank later machine re-extraction.  ivan/works_for
    * is MODEL-asserted (rank 10); the USER retraction must stamp USER authority
    * (rank 30) onto the row, otherwise the row keeps the asserter's rank and the
    * next SYSTEM drain (rank 20) that re-derives the same triple from new text
    * clears invalidated_at and resurrects a fact the user deleted. */
   assert(db2_fact_commit("ivan", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   assert(scalar_int("SELECT authority_rank FROM entity_edges WHERE source='ivan'"
                     " AND relation='works_for' AND edge_class='semantic'") == FACT_ACTOR_MODEL);
   assert(db2_fact_retract("ivan", "works_for", NULL, FACT_AUTHORITY_USER) >= 1);
   assert(scalar_int("SELECT authority_rank FROM entity_edges WHERE source='ivan'"
                     " AND relation='works_for' AND edge_class='semantic'") == FACT_ACTOR_USER);
   {
      fact_actor_t sys;
      assert(db2_fact_actor_internal(FACT_ACTOR_SYSTEM, &sys) == 0);
      fact_evidence_input_t rev = {.source_kind = "message",
                                   .source_id = "message:ivan-reextract",
                                   .evidence_hash = "hash-ivan-reextract",
                                   .observed_at = "2026-01-02 00:00:00"};
      fact_assertion_input_t rai = {.source = "ivan",
                                    .relation = "works_for",
                                    .target = "acme",
                                    .subject_kind = NODE_PERSON,
                                    .object_kind = NODE_ORG,
                                    .confidence_class = "B",
                                    .confidence = 0.7,
                                    .assertion_kind = FACT_KIND_WORLD_FACT,
                                    .evidence = &rev,
                                    .functional = 1};
      fact_mutation_result_t rmr;
      assert(db2_fact_mutation_assert(&sys, &rai, &rmr) == 0);
      assert(strcmp(rmr.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);
      assert(db2_fact_current_count("ivan") == 0);
      s = edge_state("ivan", "works_for");
      assert(strcmp(s.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0 && s.invalidated[0] != '\0');
   }

   /* §4 retract — immutable (parent_of): model refused, user wins. */
   assert(db2_fact_commit("ann", NODE_PERSON, "parent_of", "ben", NODE_PERSON, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_retract("ann", "parent_of", NULL, FACT_AUTHORITY_MODEL) ==
          FACT_RETRACT_IMMUTABLE);
   assert(db2_fact_current_count("ann") == 1); /* unchanged */
   assert(db2_fact_retract("ann", "parent_of", NULL, FACT_AUTHORITY_USER) >= 1);
   assert(db2_fact_current_count("ann") == 0);

   /* §4 retract — hard_delete (also_known_as) on a MODEL-authored (Class B) fact:
    * not immutable and not user-stated, so a model authority may tombstone it
    * (suppressed + superseded, row retained + auditable). */
   assert(db2_fact_commit("frank", NODE_PERSON, "also_known_as", "frankie", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("frank") == 0); /* model candidate is quarantined from recall */
   assert(db2_fact_retract("frank", "also_known_as", NULL, FACT_AUTHORITY_MODEL) >= 1);
   s = edge_state("frank", "also_known_as");
   assert(s.suppressed == 0 && s.superseded[0] == '\0' && s.invalidated[0] != '\0');
   assert(db2_fact_current_count("frank") == 0);

   /* §4 retract — a user-stated (Class A) fact: model refused, user hard_deletes. */
   assert(db2_fact_commit("eve", NODE_PERSON, "also_known_as", "evie", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_retract("eve", "also_known_as", NULL, FACT_AUTHORITY_MODEL) == 0);
   assert(db2_fact_current_count("eve") == 1); /* model refused */
   assert(db2_fact_retract("eve", "also_known_as", NULL, FACT_AUTHORITY_USER) >= 1);
   assert(db2_fact_current_count("eve") == 0);

   /* §4 retract — target (old-value) scoping: only the matching edge is retracted,
    * sibling values of the same (subject, relation) are untouched. */
   assert(db2_fact_commit("gus", NODE_PERSON, "member_of", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("gus", NODE_PERSON, "member_of", "globex", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("gus") == 2);
   assert(db2_fact_retract("gus", "member_of", "acme", FACT_AUTHORITY_USER) == 1);
   assert(db2_fact_current_count("gus") == 1); /* globex remains */

   /* Commit correction: a FUNCTIONAL relation supersedes the prior object. hank
    * works_for acme, then globex -> globex current, acme archived (superseded). */
   assert(db2_fact_commit("hank", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("hank") == 1);
   assert(db2_fact_commit("hank", NODE_PERSON, "works_for", "globex", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("hank") == 1); /* globex supersedes acme */

   /* Commit correction: an IMMUTABLE relation rejects a contradicting object. iris
    * born_in kyoto is fixed; a later born_in osaka is dropped. */
   assert(db2_fact_commit("iris", NODE_PERSON, "born_in", "kyoto", NODE_PLACE, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("iris") == 1);
   (void)db2_fact_commit("iris", NODE_PERSON, "born_in", "osaka", NODE_PLACE, FACT_AUTHORITY_USER,
                         1);
   assert(db2_fact_current_count("iris") == 1); /* still kyoto, immutable */

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

   ai.target = "globex";
   ai.evidence = &ev1;
   assert(db2_fact_mutation_assert(&model_actor, &ai, &mr) == 0);
   assert(mr.quarantined == 1 && strcmp(mr.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   assert(db2_fact_current_count("jane") == 1); /* incumbent only; candidate is not recallable */
   int64_t candidate_id = mr.assertion_id;
   assert(db2_fact_mutation_review(&operator_actor, candidate_id, FACT_REVIEW_APPROVE, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_PROMOTED) == 0);
   assert(db2_fact_current_count("jane") == 1); /* approved value superseded incumbent */
   assert(db2_fact_mutation_review(&operator_actor, candidate_id, FACT_REVIEW_UNDO, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0);
   assert(db2_fact_current_count("jane") == 1); /* undo restored incumbent */

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
   assert(rollback_commit[0] != '\0' && db2_fact_current_count("rollback-subject") == 0);

   /* An async delivery retry carries the same immutable evidence mention. It
    * must be a true no-op after rollback, rather than treating the replay as a
    * new correction that resurrects the invalidated assertion. */
   int commits_before_replay = scalar_int("SELECT COUNT(*) FROM fact_graph_commits");
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(mr.assertion_id > 0 && mr.changed == 0 && mr.evidence_added == 0);
   assert(mr.commit_id[0] == '\0');
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);
   assert(db2_fact_current_count("rollback-subject") == 0);
   assert(scalar_int("SELECT COUNT(*) FROM fact_graph_commits") == commits_before_replay);

   /* A later drain that re-extracts the same triple from NEW text is not a
    * replay, so the evidence guard above does not apply.  The rollback recorded
    * the operator's authority on the row, so the reactivate gate must refuse
    * this SYSTEM re-assertion and the rolled-back triple stays retracted. */
   fact_evidence_input_t reextract_ev = ev1;
   reextract_ev.source_id = "message:rollback-reextract";
   reextract_ev.evidence_hash = "hash-rollback-reextract";
   ai.evidence = &reextract_ev;
   assert(db2_fact_mutation_assert(&system_actor, &ai, &mr) == 0);
   assert(strcmp(mr.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0);
   assert(db2_fact_current_count("rollback-subject") == 0);
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
   assert(db2_fact_current_count("batch-a") == 0 && db2_fact_current_count("batch-b") == 0);
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
   assert(scalar_int("SELECT COUNT(*) FROM kb_audit_event WHERE action LIKE 'fact.%'") > 0);
   (void)jane_acme;

   /* bad args / no-op. */
   assert(db2_fact_retract(NULL, "works_for", NULL, FACT_AUTHORITY_MODEL) == -1);
   assert(db2_fact_retract("nobody", "works_for", NULL, FACT_AUTHORITY_MODEL) == 0);
   assert(db2_fact_current_count("") == -1);

   db2_test_shim_close();
   printf("fact_lifecycle: all tests passed\n");
   return 0;
}
