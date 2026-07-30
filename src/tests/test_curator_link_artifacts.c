/* test_curator_link_artifacts.c: link_artifacts bridge over the sqlite shim.
 * Empty path plus a seeded regression: a committed code_unit whose
 * domain_concepts name a committed entity gets a `mentions` edge and is marked
 * processed (guards the artifacts.payload SELECT/LIKE column names). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "kb_curator_link_artifacts.h"

/* Controllable stubs for the embed + vector-NN deps (the sqlite shim has no
 * pgvector). Default: embedding succeeds (384-d) but the NN search returns
 * nothing, so the semantic pass is a no-op and the string-match tests are
 * unaffected. test_semantic_link programs a single hit. */
static int g_search_n = 0;
static int64_t g_search_pid = 0;
static double g_search_score = 0.0;
static char g_lookup_aid[64] = "";

int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   (void)text;
   (void)command;
   (void)input_type;
   int dim = max_dim < 384 ? max_dim : 384;
   for (int i = 0; i < dim; i++)
      out[i] = 0.0f;
   return dim;
}
int pgvec_curator_entity_search(const char *scope_kind, const char *scope_id, const float *vec,
                                int dim, int limit, int64_t *ids, double *scores, int max)
{
   (void)scope_kind;
   (void)scope_id;
   (void)vec;
   (void)dim;
   (void)limit;
   if (g_search_n >= 1 && max >= 1)
   {
      ids[0] = g_search_pid;
      scores[0] = g_search_score;
      return 1;
   }
   return 0;
}
int pgvec_curator_entity_lookup(int64_t point_id, char *artifact_id_out, int aid_len,
                                char *name_out, int name_len)
{
   (void)point_id;
   if (name_out && name_len > 0)
      name_out[0] = '\0';
   if (artifact_id_out && aid_len > 0)
   {
      snprintf(artifact_id_out, (size_t)aid_len, "%s", g_lookup_aid);
      return g_lookup_aid[0] ? 1 : 0;
   }
   return 0;
}

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static int count(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

static void test_empty(void)
{
   db2_test_shim_open();
   assert(kb_curator_link_artifacts_one(NULL) == 0);
   db2_test_shim_close();
   printf("  link_artifacts graceful on empty/shim OK\n");
}

static void test_seeded_links(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('e1','entity','committed','{\"name\":\"pgvector\"}')");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('cu','code_unit','committed','{\"domain_concepts\":[\"pgvector\"]}')");

   int rc = kb_curator_link_artifacts_one(NULL);
   assert(rc == 1);
   /* a `mentions` edge code_unit -> entity, and the code_unit is marked done */
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE from_id='cu' AND to_id='e1'"
                    " AND kind='mentions'") == 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='cu'"
                    " AND reflected_at IS NOT NULL AND reflected_at <> ''") == 1);
   /* idempotent / cursor advances: next call finds nothing */
   assert(kb_curator_link_artifacts_one(NULL) == 0);

   db2_test_shim_close();
   printf("  link_artifacts writes a mentions edge for a seeded code_unit OK\n");
}

static void test_normalized_links(void)
{
   /* Code extraction emits snake_case `workspace_provider`; doc extraction
    * emits the prose entity name `Workspace Provider`. They name the same
    * concept, so a format-insensitive match must still link them. */
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('e1','entity','committed','{\"name\":\"Workspace Provider\"}')");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('cu','code_unit','committed',"
            "'{\"domain_concepts\":[\"workspace_provider\"]}')");

   int rc = kb_curator_link_artifacts_one(NULL);
   assert(rc == 1);
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE from_id='cu' AND to_id='e1'"
                    " AND kind='mentions'") == 1);

   db2_test_shim_close();
   printf("  link_artifacts matches snake_case concept to prose entity name OK\n");
}

static void test_semantic_link(void)
{
   /* The concept string matches no entity name, but the NN search resolves it
    * to a committed entity above threshold -> link via embedding similarity. */
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('e2','entity','committed','{\"name\":\"Detached Resource Provider\"}')");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('cu2','code_unit','committed',"
            "'{\"domain_concepts\":[\"zzz_no_string_match\"]}')");
   g_search_n = 1;
   g_search_pid = 123;
   g_search_score = 0.91; /* >= CURATOR_LINK_SEMANTIC_THRESHOLD */
   snprintf(g_lookup_aid, sizeof(g_lookup_aid), "e2");

   int rc = kb_curator_link_artifacts_one(NULL);
   assert(rc == 1);
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE from_id='cu2' AND to_id='e2'"
                    " AND kind='mentions'") == 1);

   g_search_n = 0;
   g_lookup_aid[0] = '\0';
   db2_test_shim_close();
   printf("  link_artifacts links via embedding NN when the string doesn't match OK\n");
}

static void test_semantic_below_threshold(void)
{
   /* A near neighbour below threshold must NOT be linked. */
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('e3','entity','committed','{\"name\":\"Unrelated Thing\"}')");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('cu3','code_unit','committed','{\"domain_concepts\":[\"qqq\"]}')");
   g_search_n = 1;
   g_search_pid = 7;
   g_search_score = 0.40; /* < CURATOR_LINK_SEMANTIC_THRESHOLD */
   snprintf(g_lookup_aid, sizeof(g_lookup_aid), "e3");

   int rc = kb_curator_link_artifacts_one(NULL);
   assert(rc == 1); /* code_unit processed... */
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE to_id='e3'") ==
          0); /* ...but not linked */

   g_search_n = 0;
   g_lookup_aid[0] = '\0';
   db2_test_shim_close();
   printf("  link_artifacts skips below-threshold NN neighbours OK\n");
}

int main(void)
{
   test_empty();
   test_seeded_links();
   test_normalized_links();
   test_semantic_link();
   test_semantic_below_threshold();
   printf("curator_link_artifacts: all tests passed\n");
   return 0;
}
