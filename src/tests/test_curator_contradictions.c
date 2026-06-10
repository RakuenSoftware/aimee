/* test_curator_contradictions.c: detect_contradictions over the sqlite shim.
 * Empty path plus a seeded regression: two committed claim vectors that share a
 * subject+attribute but disagree on value get a `contradicts` edge. Guards both
 * the curator_claim_vectors self-join and the artifact_links column name
 * (`kind`, not `link_kind`) in the NOT-EXISTS dedup. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../kb/kb_curator_contradictions.h"

/* Link stubs for the opt-in fuzzy phase: it is gated behind the deep tier (off in
 * these tests), so mine_fuzzy_contradictions returns before calling any of these
 * at runtime — they exist only to satisfy the linker. */
int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   (void)text;
   (void)command;
   (void)out;
   (void)max_dim;
   return 0;
}
int pgvec_curator_claim_search(const char *which_vec, const char *claim_kind, const float *vec,
                               int dim, int limit, int64_t *ids, double *scores, int max)
{
   (void)which_vec;
   (void)claim_kind;
   (void)vec;
   (void)dim;
   (void)limit;
   (void)ids;
   (void)scores;
   (void)max;
   return 0;
}
int pgvec_curator_claim_deep_similarity(int64_t a, int64_t b, double *out)
{
   (void)a;
   (void)b;
   (void)out;
   return 0;
}
int pgvec_curator_claim_fields(int64_t point_id, char *artifact_out, int alen, char *subject_out,
                               int slen, char *value_out, int vlen)
{
   (void)point_id;
   (void)artifact_out;
   (void)alen;
   (void)subject_out;
   (void)slen;
   (void)value_out;
   (void)vlen;
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
   assert(kb_curator_detect_contradictions_one(NULL) == 0);
   db2_test_shim_close();
   printf("  detect_contradictions graceful on empty/shim OK\n");
}

static void test_contradiction(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* Two claim artifacts (FK targets for artifact_links). */
   seed(db, "INSERT INTO artifacts (id,kind,state) VALUES"
            " ('a1','claim','committed'),('a2','claim','committed'),('a3','claim','committed')");
   /* Same subject+attribute, different value → contradiction (a1,a2). */
   seed(db, "INSERT INTO curator_claim_vectors (point_id,artifact_id,subject,attribute,value)"
            " VALUES (1,'a1','auth','token format','JWT'),"
            "        (2,'a2','auth','token format','opaque'),"
            "        (3,'a3','auth','token format','JWT')"); /* a3 == a1 value: no contradiction */

   int rc = kb_curator_detect_contradictions_one(NULL);
   assert(rc == 1);
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE kind='contradicts'"
                    " AND from_id='a1' AND to_id='a2'") == 1);
   /* a1/a3 share the value → not a contradiction */
   assert(count(db, "SELECT COUNT(*) FROM artifact_links WHERE kind='contradicts'"
                    " AND ((from_id='a1' AND to_id='a3') OR (from_id='a3' AND to_id='a1'))") == 0);

   db2_test_shim_close();
   printf("  detect_contradictions links a value-disagreeing claim pair OK\n");
}

int main(void)
{
   test_empty();
   test_contradiction();
   printf("curator_contradictions: all tests passed\n");
   return 0;
}
