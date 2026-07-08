/* test_curator_pipeline.c: end-to-end curator chain over the sqlite shim. Drives
 * the REAL passes in sequence — resolve_entities -> index_code_unit ->
 * link_artifacts -> /v1/implements — proving a doc-concept resolves through to
 * the code that implements it. Embedding/pgvector are stubbed (the chain is
 * graph/SQL, not vector, dependent). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../kb/kb_curator_resolve_entities.h"
#include "../kb/kb_curator_index_code_unit.h"
#include "../kb/kb_curator_link_artifacts.h"
#include "../kb_curator_serve.h"

/* ── stubs: embedding + vector sinks (return "unavailable"/no-op) ─────────── */
int memory_embed_text(const char *t, const char *c, float *o, int d)
{
   (void)t;
   (void)c;
   (void)o;
   (void)d;
   return 0;
}
int pgvec_curator_entity_upsert(int64_t p, const float *v, int d, const char *sk, const char *si,
                                const char *cn, const char *aid, const char *pl)
{
   (void)p;
   (void)v;
   (void)d;
   (void)sk;
   (void)si;
   (void)cn;
   (void)aid;
   (void)pl;
   return 0;
}
int pgvec_curator_entity_search(const char *sk, const char *si, const float *v, int d, int lim,
                                int64_t *ids, double *sc, int max)
{
   (void)sk;
   (void)si;
   (void)v;
   (void)d;
   (void)lim;
   (void)ids;
   (void)sc;
   (void)max;
   return 0;
}
int pgvec_curator_code_unit_upsert(int64_t p, const float *iv, const float *sv, const float *bv,
                                   int d, const char *aid, const char *fp, const char *dk,
                                   const char *sig, const char *bh, const char *pl)
{
   (void)p;
   (void)iv;
   (void)sv;
   (void)bv;
   (void)d;
   (void)aid;
   (void)fp;
   (void)dk;
   (void)sig;
   (void)bh;
   (void)pl;
   return 0;
}

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

int main(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* extract's output: a proposed entity mention + a proposed code_unit whose
    * domain_concepts name that entity, with the code_unit's source file cited. */
   seed(db, "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload) VALUES"
            " ('ent','entity','proposed','project','projA',"
            "'{\"name\":\"pgvector\",\"context\":\"vector store\"}'),"
            " ('cu','code_unit','proposed','project','projA',"
            "'{\"summary\":\"hnsw search\",\"domain_concepts\":[\"pgvector\"]}')");
   seed(db, "INSERT INTO artifact_citations (artifact_id,source_kind,source_id)"
            " VALUES ('cu','kb_file','src/vec.c')");

   /* 1. resolve_entities commits the entity mention. */
   assert(kb_curator_resolve_entities_one(NULL) == 1);
   /* 2. index_code_unit commits the code_unit. */
   assert(kb_curator_index_code_unit_one(NULL) == 1);
   /* 3. link_artifacts wires code_unit --mentions--> entity by concept name. */
   assert(kb_curator_link_artifacts_one(NULL) == 1);

   /* 4. /v1/implements turns the topic into the implementing code unit. */
   char buf[4096];
   int n = kb_curator_implements_json("pgvector", buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "\"cu\"") && strstr(buf, "src/vec.c") && strstr(buf, "hnsw search"));

   db2_test_shim_close();
   printf("  end-to-end: resolve -> index -> link -> /v1/implements OK\n");
   printf("curator_pipeline: all tests passed\n");
   return 0;
}
