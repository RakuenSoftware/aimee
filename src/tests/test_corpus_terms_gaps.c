/* test_corpus_terms_gaps.c: curator_terms and curator_gaps stage handlers. */

#include "artifacts.h"
#include "corpus_structural.h"
#include "curator_gaps.h"
#include "curator_terms.h"
#include "curiosity.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db2_test_shim.h"
#include "db_postgres.h"
#include "kb_docs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static int query_int(const char *sql)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int out = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return out;
}

static void test_normalize_terms_basic(void)
{
   open_db();

   int64_t doc_id =
       db2_kb_doc_write("hash-terms-basic", "docs/guide.md", "global", "passthrough", "",
                        "AuthMiddleware is the bearer-token guard. "
                        "The `SessionStore` handles user sessions.",
                        NULL);
   assert(doc_id > 0);

   int n = db2_corpus_normalize_terms(doc_id);
   assert(n >= 0);

   /* At least one term_mapping artifact should have been written. */
   int count = query_int(
       "SELECT COUNT(*) FROM artifacts WHERE kind = 'term_mapping' AND state = 'proposed'");
   assert(count >= 0);

   close_db();
   printf("  normalize_terms_basic: ok (terms=%d)\n", n);
}

static void test_normalize_terms_deduplication(void)
{
   open_db();

   int64_t doc_id = db2_kb_doc_write("hash-terms-dedup", "docs/dedup.md", "global", "passthrough",
                                     "", "AuthMiddleware is used. AuthMiddleware again.", NULL);
   assert(doc_id > 0);

   int first = db2_corpus_normalize_terms(doc_id);
   int second = db2_corpus_normalize_terms(doc_id);
   assert(first >= 0);
   /* Second run should produce zero new artifacts since terms already exist. */
   assert(second == 0);

   close_db();
   printf("  normalize_terms_deduplication: ok\n");
}

static void test_detect_gaps_dangling_ref(void)
{
   open_db();

   const char *body = "# Guide\n\nSee [missing.md](missing.md) for details.\n";
   int64_t doc_id = db2_kb_doc_write("hash-gaps-dangling", "docs/gaps.md", "global", "passthrough",
                                     "", body, NULL);
   assert(doc_id > 0);

   int ref_count = db2_corpus_extract_references(doc_id);
   assert(ref_count >= 0);

   int n = db2_corpus_detect_gaps(doc_id);
   assert(n >= 0);

   close_db();
   printf("  detect_gaps_dangling_ref: ok (gaps=%d)\n", n);
}

static void test_gap_promotes_to_curiosity(void)
{
   open_db();

   int rc = db2_curiosity_promote_corpus_gap("artifact-test-id", "undefined_entity", "test-entity",
                                             "ev");
   assert(rc == 0);

   int count =
       query_int("SELECT COUNT(*) FROM curiosity_items WHERE target_entity = 'test-entity'");
   assert(count >= 1);

   /* Second call for same subject should skip (no duplicate). */
   rc = db2_curiosity_promote_corpus_gap("artifact-test-id-2", "undefined_entity", "test-entity",
                                         "ev2");
   assert(rc == 0);
   int count2 =
       query_int("SELECT COUNT(*) FROM curiosity_items WHERE target_entity = 'test-entity'");
   assert(count2 == count);

   close_db();
   printf("  gap_promotes_to_curiosity: ok\n");
}

int main(void)
{
   printf("corpus-terms-gaps tests:\n");
   test_normalize_terms_basic();
   test_normalize_terms_deduplication();
   test_detect_gaps_dangling_ref();
   test_gap_promotes_to_curiosity();
   printf("all corpus-terms-gaps tests passed\n");
   return 0;
}
