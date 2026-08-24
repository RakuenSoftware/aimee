/* test_learning_version.c — unit tests for version-bump replay
 * (kb/kb_learning_version.c) and the store_vector idempotency it relies on.
 *
 * Tests:
 *   1. store_vector is idempotent per artifact (re-embed overwrites, no dupes).
 *   2. first call records the baseline, no replay; unchanged version is a no-op.
 *   3. model_version bump re-embeds evidence ops, leaves synth ops alone.
 *   4. prompt_version bump replays synth ops, leaves evidence ops alone.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "learning_synth_ops.h"
#include "modules/db2/c/db2_test_shim.h"
#include "support/embedding_literal.h"
#include "../kb/kb_learning_version.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

/* Seed an evidence artifact with an embedded vector (op status 'ok') and a
 * synthesis op marked done (status 'ok'). Returns the artifact id. */
static void seed(char *id_out, size_t id_len)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   assert(db2_artifact_write(id, "feedback_negative", "proposed", "user", "u", "u", 1.0,
                             "{\"content\":\"x\"}") == 0);
   assert(db2_evidence_enqueue(id, "evidence") == 0);
   char vec[EMBEDDING_LITERAL_MAX];
   assert(db2_evidence_store_vector(id, "evidence", embedding_literal(vec, sizeof(vec), 0.5)) ==
          0); /* op -> ok */
   assert(db2_synth_enqueue(id) == 0);
   assert(db2_synth_mark_done(id) == 0); /* op -> ok */
   if (id_out)
      snprintf(id_out, id_len, "%s", id);
}

/* ---- 1. store_vector idempotency -------------------------------------- */
static void test_store_vector_idempotent(void)
{
   open_db();
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   assert(db2_artifact_write(id, "feedback_negative", "proposed", "user", "u", "u", 1.0, "{}") ==
          0);
   assert(db2_evidence_enqueue(id, "evidence") == 0);

   char first[EMBEDDING_LITERAL_MAX], second[EMBEDDING_LITERAL_MAX];
   embedding_literal(first, sizeof(first), 0.5);
   embedding_literal(second, sizeof(second), 0.25);
   assert(db2_evidence_store_vector(id, "evidence", first) == 0);
   assert(db2_evidence_store_vector(id, "evidence", second) == 0); /* re-embed */

   db2_evidence_vector_row_t rows[8];
   int got = db2_evidence_vectors_list(rows, 8);
   assert(got == 1);                               /* one row, not two */
   assert(strcmp(rows[0].embedding, second) == 0); /* latest wins */

   printf("  test_store_vector_idempotent: PASS\n");
}

/* ---- 2. baseline + no-op ---------------------------------------------- */
static void test_first_run_no_replay(void)
{
   open_db();
   seed(NULL, 0);

   learning_version_replay_t r;
   assert(learning_version_replay("v1", "v1", &r) == 0);
   assert(r.model_bumped == 0 && r.prompt_bumped == 0); /* baseline only */
   assert(db2_evidence_ops_count("ok") == 1);
   assert(db2_synth_ops_count("ok") == 1);

   /* Same versions again: still a no-op. */
   assert(learning_version_replay("v1", "v1", &r) == 0);
   assert(r.model_bumped == 0 && r.prompt_bumped == 0);
   assert(db2_evidence_ops_count("ok") == 1);

   printf("  test_first_run_no_replay: PASS\n");
}

/* ---- 3. model bump re-embeds only ------------------------------------- */
static void test_model_bump_reembeds(void)
{
   open_db();
   seed(NULL, 0);
   learning_version_replay_t r;
   assert(learning_version_replay("v1", "v1", &r) == 0); /* baseline */

   assert(learning_version_replay("v2", "v1", &r) == 0);
   assert(r.model_bumped == 1 && r.prompt_bumped == 0);
   assert(r.embed_ops_reset == 1);
   assert(db2_evidence_ops_count("pending") == 1); /* re-enqueued */
   assert(db2_evidence_ops_count("ok") == 0);
   assert(db2_synth_ops_count("ok") == 1); /* synthesis untouched */
   assert(db2_synth_ops_count("pending") == 0);

   printf("  test_model_bump_reembeds: PASS\n");
}

/* ---- 4. prompt bump replays synthesis only ---------------------------- */
static void test_prompt_bump_replays(void)
{
   open_db();
   seed(NULL, 0);
   learning_version_replay_t r;
   assert(learning_version_replay("v1", "v1", &r) == 0); /* baseline */

   assert(learning_version_replay("v1", "v2", &r) == 0);
   assert(r.prompt_bumped == 1 && r.model_bumped == 0);
   assert(r.synth_ops_reset == 1);
   assert(db2_synth_ops_count("pending") == 1); /* replayed */
   assert(db2_synth_ops_count("ok") == 0);
   assert(db2_evidence_ops_count("ok") == 1); /* embedding untouched */
   assert(db2_evidence_ops_count("pending") == 0);

   printf("  test_prompt_bump_replays: PASS\n");
}

int main(void)
{
   printf("test_learning_version:\n");
   test_store_vector_idempotent();
   test_first_run_no_replay();
   test_model_bump_reembeds();
   test_prompt_bump_replays();
   db2_test_shim_close();
   printf("test_learning_version: ALL PASS\n");
   return 0;
}
