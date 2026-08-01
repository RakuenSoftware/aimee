/* test_curator_index_narrative.c: smoke test for the narrative vector indexer.
 * The pure text/id helpers are static; this exercises the public drain entry
 * under the sqlite shim (no seeded narrative artifacts -> returns 0, no crash). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "kb_curator_index_narrative.h"

/* Stub the heavy embed + vector deps the handler references but this test never
 * reaches (the handler returns early with no seeded narrative artifacts). */
int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   (void)text;
   (void)command;
   (void)input_type;
   (void)out;
   (void)max_dim;
   return 0;
}
int pgvec_curator_narrative_upsert(int64_t point_id, const float *vec, int dim,
                                   const char *artifact_id, const char *kind, const char *doc_id,
                                   const char *status, const char *priority,
                                   const char *payload_json)
{
   (void)point_id;
   (void)vec;
   (void)dim;
   (void)artifact_id;
   (void)kind;
   (void)doc_id;
   (void)status;
   (void)priority;
   (void)payload_json;
   return 0;
}

static void test_seeded_commits(void)
{
   /* Regression: the proposed-doc_summary SELECT must use the real `payload` column
    * (not `payload_json`); a seeded proposed doc_summary must be found + committed. */
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   assert(sqlite3_exec(db,
                       "INSERT INTO artifacts (id,kind,state,payload)"
                       " VALUES ('seed1','doc_summary','proposed','{\"summary\":\"x\"}')",
                       NULL, NULL, NULL) == SQLITE_OK);
   int rc = kb_curator_index_narrative_one(NULL);
   assert(rc == 1);
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT state FROM artifacts WHERE id='seed1'", -1, &st, NULL) ==
          SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   const char *state = (const char *)sqlite3_column_text(st, 0);
   assert(state && strcmp(state, "committed") == 0);
   sqlite3_finalize(st);
   db2_test_shim_close();
   printf("  index_narrative commits a seeded proposed doc_summary OK\n");
}

int main(void)
{
   db2_test_shim_open();
   int rc = kb_curator_index_narrative_one(NULL);
   assert(rc == 0);
   db2_test_shim_close();
   printf("  index_narrative graceful on empty/shim OK\n");
   test_seeded_commits();
   printf("curator_index_narrative: all tests passed\n");
   return 0;
}
