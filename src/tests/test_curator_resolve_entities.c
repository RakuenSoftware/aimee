/* test_curator_resolve_entities.c: unit tests for the resolve_entities pure
 * helpers (embed-text composition + deterministic point id) and a graceful
 * no-data call into the drain handler under the sqlite test shim. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include <sqlite3.h>

#include "db2_test_shim.h"
#include "../kb/kb_curator_resolve_entities.h"

/* Stub the heavy embed + vector deps the handler references. Returns a full
 * 384-dim vector so the resolve match/upsert path is exercised (the builtin
 * embedder is not linked here). Keeps the link off memory_core.o /
 * pgvec_transport.o. */
int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   (void)text;
   (void)command;
   int dim = max_dim < 384 ? max_dim : 384;
   for (int i = 0; i < dim; i++)
      out[i] = 0.01f * (float)(i + 1);
   return dim;
}

/* Self-contained scope-lattice stub (the real one lives in kb_curator_promote.o,
 * which this test does not link): project -> workspace -> global, no ids. */
int kb_curator_broaden_scope(void *conn, const char *kind, const char *id, char *out_kind,
                             size_t kn, char *out_id, size_t in)
{
   (void)conn;
   (void)id;
   if (out_id && in)
      out_id[0] = '\0';
   if (kind && strcmp(kind, "project") == 0)
   {
      snprintf(out_kind, kn, "workspace");
      return 1;
   }
   if (kind && strcmp(kind, "workspace") == 0)
   {
      snprintf(out_kind, kn, "global");
      return 1;
   }
   return 0;
}
/* Test-controlled stub state: g_match_scope is the scope_kind at which the
 * search reports a hit (NULL = never); g_upserts counts vector creations. */
static const char *g_match_scope = NULL;
static double g_match_score = 0.0;
static int g_upserts = 0;

int pgvec_curator_entity_upsert(int64_t point_id, const float *vec, int dim, const char *scope_kind,
                                const char *scope_id, const char *canonical_name,
                                const char *artifact_id, const char *payload_json)
{
   (void)point_id;
   (void)vec;
   (void)dim;
   (void)scope_kind;
   (void)scope_id;
   (void)canonical_name;
   (void)artifact_id;
   (void)payload_json;
   g_upserts++;
   return 0;
}
int pgvec_curator_entity_search(const char *scope_kind, const char *scope_id, const float *vec,
                                int dim, int limit, int64_t *ids, double *scores, int max)
{
   (void)scope_id;
   (void)vec;
   (void)dim;
   (void)limit;
   (void)max;
   if (g_match_scope && scope_kind && strcmp(scope_kind, g_match_scope) == 0)
   {
      if (ids)
         ids[0] = 4242;
      if (scores)
         scores[0] = g_match_score;
      return 1;
   }
   return 0;
}
int pgvec_curator_entity_lookup(int64_t point_id, char *artifact_id_out, int aid_len,
                                char *name_out, int name_len)
{
   (void)point_id;
   if (artifact_id_out && aid_len > 0)
      artifact_id_out[0] = '\0';
   if (name_out && name_len > 0)
      name_out[0] = '\0';
   return 0;
}
int kb_curator_judge_same_entity(const config_t *cfg, const char *judge_cmd,
                                 const char *mention_name, const char *mention_context,
                                 const char *candidate_name, double score, int *out_same,
                                 char *errbuf, size_t errlen)
{
   (void)cfg;
   (void)judge_cmd;
   (void)mention_name;
   (void)mention_context;
   (void)candidate_name;
   (void)score;
   (void)out_same;
   if (errbuf && errlen)
      errbuf[0] = '\0';
   return -1;
}

static void test_embed_text(void)
{
   char buf[256];
   kb_curator_entity_embed_text("auth middleware", "validates bearer tokens", buf, sizeof(buf));
   assert(strcmp(buf, "auth middleware - validates bearer tokens") == 0);

   kb_curator_entity_embed_text("pgvector", "", buf, sizeof(buf));
   assert(strcmp(buf, "pgvector") == 0);

   kb_curator_entity_embed_text("pgvector", NULL, buf, sizeof(buf));
   assert(strcmp(buf, "pgvector") == 0);

   kb_curator_entity_embed_text("", "ctx only", buf, sizeof(buf));
   assert(buf[0] == '\0');

   kb_curator_entity_embed_text(NULL, NULL, buf, sizeof(buf));
   assert(buf[0] == '\0');
   printf("  embed_text composition OK\n");
}

static void test_point_id(void)
{
   int64_t a = kb_curator_entity_point_id("11111111-2222-3333-4444-555555555555");
   int64_t b = kb_curator_entity_point_id("11111111-2222-3333-4444-555555555555");
   int64_t c = kb_curator_entity_point_id("99999999-8888-7777-6666-555555555555");
   assert(a == b);         /* deterministic */
   assert(a != c);         /* distinct inputs differ */
   assert(a > 0 && c > 0); /* masked positive */
   assert(kb_curator_entity_point_id(NULL) >= 0);
   assert(kb_curator_entity_point_id("") >= 0);
   printf("  point_id deterministic + positive OK\n");
}

static void test_resolve_graceful(void)
{
   /* No proposed entity mentions seeded → handler returns 0 and does not crash. */
   int rc = kb_curator_resolve_entities_one(NULL);
   assert(rc == 0 || rc == 1);
   printf("  resolve_one graceful on empty/shim OK (rc=%d)\n", rc);
}

static void test_resolve_seeded(void)
{
   /* Regression: the proposed-entity claim SELECT must reference the real
    * `payload` column (not `payload_json`). A seeded proposed entity must be
    * found and committed; if the column name drifts, resolve_one returns 0 and
    * the state stays proposed. */
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   assert(sqlite3_exec(db,
                       "INSERT INTO artifacts (id,kind,state,payload)"
                       " VALUES ('ent1','entity','proposed',"
                       "'{\"name\":\"Auth Middleware\",\"context\":\"validates tokens\"}')",
                       NULL, NULL, NULL) == SQLITE_OK);
   int rc = kb_curator_resolve_entities_one(NULL);
   assert(rc == 1);
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT state FROM artifacts WHERE id='ent1'", -1, &st, NULL) ==
          SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   const char *state = (const char *)sqlite3_column_text(st, 0);
   assert(state && strcmp(state, "committed") == 0);
   sqlite3_finalize(st);
   printf("  resolve_one commits a seeded proposed entity OK\n");
}

static void test_resolve_cross_scope(void)
{
   /* A project-scoped mention whose only canonical match lives at the broader
    * GLOBAL scope must resolve onto it (no new vector written), exercising the
    * scope-lattice walk project -> workspace -> global. */
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   g_upserts = 0;
   g_match_scope = "global";
   g_match_score = 0.95; /* confident merge */
   assert(sqlite3_exec(db,
                       "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload)"
                       " VALUES ('ent-x','entity','proposed','project','proj-z',"
                       "'{\"name\":\"pgvector\"}')",
                       NULL, NULL, NULL) == SQLITE_OK);
   int rc = kb_curator_resolve_entities_one(NULL);
   assert(rc == 1);
   assert(g_upserts == 0); /* resolved onto the global entity; no duplicate vector */

   /* Negative control: a mention with no match at any scope creates a vector. */
   g_upserts = 0;
   g_match_scope = NULL;
   assert(sqlite3_exec(db,
                       "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload)"
                       " VALUES ('ent-y','entity','proposed','project','proj-z',"
                       "'{\"name\":\"unique-thing-no-match\"}')",
                       NULL, NULL, NULL) == SQLITE_OK);
   rc = kb_curator_resolve_entities_one(NULL);
   assert(rc == 1);
   assert(g_upserts == 1); /* no match anywhere -> new canonical vector created */
   g_match_scope = NULL;
   printf("  resolve_one resolves a project mention onto a broader-scope entity OK\n");
}

int main(void)
{
   db2_test_shim_open();
   test_embed_text();
   test_point_id();
   test_resolve_graceful();
   test_resolve_seeded();
   test_resolve_cross_scope();
   db2_test_shim_close();
   printf("curator_resolve_entities: all tests passed\n");
   return 0;
}
