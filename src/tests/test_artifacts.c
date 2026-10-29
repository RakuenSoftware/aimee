/* test_artifacts.c — unit tests for the charter artifact tables.
 *
 * Tests:
 *   1. kb_store_artifact_write: inserts a row; duplicate id is silently ignored.
 *   2. kb_store_artifact_set_state: transitions artifact state.
 *   3. kb_store_artifact_cite: inserts artifact_citations row.
 *   4. kb_store_artifact_link: inserts artifact_links row.
 *   5. kb_store_audit_event_write: inserts audit_events row.
 *   6. kb_store_artifact_count: counts by kind and state.
 *   7. learning_evidence_write_feedback: writes positive/negative evidence.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "artifacts.h"
#include "anti_patterns.h"
#include "evidence_vectors.h"
#include "feature_rows.h"
#include "modules/kb/c/kb_store_internal.h"
#include "support/embedding_literal.h"
#include "modules/kb/c/kb_store_test_shim.h"
#include "modules/kb/c/db_postgres.h"
#include <aimee/kb/host_contracts.h>
#include "cJSON.h"
#include "kb_mdl.h"
#include "modules/learning/learning_evidence.h"

/* Exercise learning's generic command dispatch and promotion audit. The Go
 * memory tests cover the store implementation and its transaction boundary. */
static int g_mem_insert_calls;
static char g_mem_insert_kind[64];
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.store") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "tier")), "L1") == 0);
   const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "kind"));
   g_mem_insert_calls++;
   snprintf(g_mem_insert_kind, sizeof(g_mem_insert_kind), "%s", kind ? kind : "");
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", 4242);
   *result = resp;
   return 1;
}

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation")),
                 "directive-create") == 0);

   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "cause")),
                 "promoted_directive") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "question")), "q1") ==
          0);
   *result = cJSON_Parse("{\"status\":\"ok\",\"directive\":{\"id\":4243}}");
   return 1;
}

/* Stub the DB1 working-profile observer (weak in learning_evidence.c) so the
 * working_profile promotion dispatch is exercisable without linking DB1. */
static int g_wp_observe_calls;
static char g_wp_observe_field[64];
int db1_working_profile_local_observe(const char *field, const char *value, double confidence,
                                      const char *session_id, int threshold)
{
   (void)value;
   (void)confidence;
   (void)session_id;
   (void)threshold;
   g_wp_observe_calls++;
   snprintf(g_wp_observe_field, sizeof(g_wp_observe_field), "%s", field ? field : "");
   return 0;
}

static void open_db(void)
{
   kb_store_test_shim_close();
   kb_store_test_shim_open();
}

static void close_db(void)
{
   kb_store_test_shim_close();
}

/* Scalar query through the KB_STORE connection. Up to two text binds cover every
 * call site here; a NULL bind is simply not applied. */
static int scalar_q(const char *sql, const char *b1, const char *b2)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(kb_store_conn(), sql, err, sizeof(err));
   assert(st);
   if (b1)
      aimee_pg_bind_text(st, "?1", b1);
   if (b2)
      aimee_pg_bind_text(st, "?2", b2);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* Seed SQL through the KB_STORE connection rather than the shim's raw sqlite handle:
 * kb_store_test_shim_handle() is NULL when the shim is backed by real Postgres, and
 * these statements are plain SQL both engines accept. */
static void exec_ok(const char *sql)
{
   char err[256] = "";
   if (aimee_pg_exec(kb_store_conn(), sql, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "exec_ok failed: %s\n  sql: %s\n", err, sql);
      assert(0 && "exec_ok");
   }
}

static int score_mdl(const char *candidate, const char *evidence, double *l_candidate,
                     double *l_residual, double *total)
{
   kb_mdl_score_t score = {0};
   if (!l_candidate || !l_residual || !total || kb_mdl_score(candidate, evidence, &score) != 0)
      return -1;
   *l_candidate = score.l_candidate;
   *l_residual = score.l_residual;
   *total = score.total;
   return 0;
}

/* ---- 1. artifact_write ---- */
static void test_artifact_write(void)
{
   open_db();

   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));
   assert(strlen(id) == 36);

   int rc = kb_store_artifact_write(id, "feedback_positive", "proposed", "user", "jbailes",
                                    "jbailes", 1.0, "{\"title\":\"test\"}");
   assert(rc == 0);

   /* duplicate id must be silently ignored, not error */
   int rc2 = kb_store_artifact_write(id, "feedback_positive", "proposed", "user", "jbailes",
                                     "jbailes", 1.0, "{\"title\":\"test\"}");
   assert(rc2 == 0);

   /* count must be 1 (not 2) because duplicate was ignored */
   int count = kb_store_artifact_count("feedback_positive", "proposed");
   assert(count == 1);

   close_db();
   printf("  artifact_write: ok\n");
}

/* ---- 2. artifact_set_state ---- */
static void test_artifact_set_state(void)
{
   open_db();

   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));

   assert(kb_store_artifact_write(id, "session_summary", "proposed", "user", "", "", 1.0, "{}") ==
          0);
   assert(kb_store_artifact_set_state(id, "committed") == 0);

   assert(kb_store_artifact_count("session_summary", "committed") == 1);
   assert(kb_store_artifact_count("session_summary", "proposed") == 0);

   close_db();
   printf("  artifact_set_state: ok\n");
}

static void test_synthesis_commit_emits_mdl_features(void)
{
   open_db();

   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));

   const char *payload =
       "{\"candidate\":\"Use bearer-token authentication.\","
       "\"evidence_bundle\":\"The API accepts Bearer tokens in the Authorization header.\","
       "\"rank_in_cluster\":2}";
   int rc =
       kb_store_artifact_write(id, "synthesis", "proposed", "project", "aimee", "", 0.9, payload);
   assert(rc == 0);
   assert(kb_store_artifact_set_state(id, "committed") == 0);

   char features[512];
   assert(kb_store_feature_row_read(id, "artifact", "mdl-v1", features, sizeof(features)) == -1);
   aimee_kb_store_register_mdl_score_provider(score_mdl);
   assert(kb_store_artifact_set_state(id, "committed") == 0);
   assert(kb_store_feature_row_read(id, "artifact", "mdl-v1", features, sizeof(features)) == 0);
   /* Parse rather than substring-match. feature_rows.features is jsonb, and
    * Postgres normalises jsonb on the way back out -- it reorders keys and prints a
    * space after every colon, so a needle like "\"mdl.rank_in_cluster\":2" never
    * matches what the server returns. Under the sqlite shim the column is plain text
    * holding the exact bytes cJSON emitted, so the needle matched and the assertion
    * was really testing the serializer's formatting, not the row's content. */
   {
      cJSON *feat = cJSON_Parse(features);
      assert(feat != NULL);
      assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(feat, "mdl.l_candidate")));
      assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(feat, "mdl.l_residual")));
      assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(feat, "mdl.total")));
      cJSON *rank = cJSON_GetObjectItemCaseSensitive(feat, "mdl.rank_in_cluster");
      assert(cJSON_IsNumber(rank) && rank->valueint == 2);
      cJSON_Delete(feat);
   }

   close_db();
   printf("  synthesis_commit_emits_mdl_features: ok\n");
}

/* ---- 3. artifact_cite ---- */
static void test_artifact_cite(void)
{
   open_db();

   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));
   assert(kb_store_artifact_write(id, "feedback_negative", "proposed", "user", "", "", 0.9, "{}") ==
          0);

   assert(kb_store_artifact_cite(id, "session_turn", "sess-123") == 0);
   /* duplicate must be silently ignored */
   assert(kb_store_artifact_cite(id, "session_turn", "sess-123") == 0);

   close_db();
   printf("  artifact_cite: ok\n");
}

/* ---- 4. artifact_link ---- */
static void test_artifact_link(void)
{
   open_db();

   char id_a[64], id_b[64];
   kb_store_artifact_gen_id(id_a, sizeof(id_a));
   kb_store_artifact_gen_id(id_b, sizeof(id_b));

   assert(kb_store_artifact_write(id_a, "session_summary", "proposed", "user", "", "", 1.0, "{}") ==
          0);
   assert(kb_store_artifact_write(id_b, "session_summary", "proposed", "user", "", "", 1.0, "{}") ==
          0);

   assert(kb_store_artifact_link(id_a, id_b, "supersedes") == 0);
   /* duplicate must be silently ignored */
   assert(kb_store_artifact_link(id_a, id_b, "supersedes") == 0);

   close_db();
   printf("  artifact_link: ok\n");
}

/* ---- 5. audit_event_write ---- */
static void test_audit_event_write(void)
{
   open_db();

   char art_id[64], evt_id[64];
   kb_store_artifact_gen_id(art_id, sizeof(art_id));
   kb_store_artifact_gen_id(evt_id, sizeof(evt_id));

   assert(kb_store_artifact_write(art_id, "feedback_positive", "committed", "user", "jbailes",
                                  "jbailes", 1.0, "{}") == 0);

   int rc = kb_store_audit_event_write(evt_id, art_id, "rule", "rule-42", "jbailes", "user",
                                       "jbailes", 1.0, 0, NULL, "{\"weight\":50}");
   assert(rc == 0);

   /* duplicate id must be silently ignored */
   int rc2 = kb_store_audit_event_write(evt_id, art_id, "rule", "rule-42", "jbailes", "user",
                                        "jbailes", 1.0, 0, NULL, "{\"weight\":50}");
   assert(rc2 == 0);

   close_db();
   printf("  audit_event_write: ok\n");
}

/* ---- 6. artifact_count ---- */
static void test_artifact_count(void)
{
   open_db();

   char id1[64], id2[64], id3[64];
   kb_store_artifact_gen_id(id1, sizeof(id1));
   kb_store_artifact_gen_id(id2, sizeof(id2));
   kb_store_artifact_gen_id(id3, sizeof(id3));

   assert(kb_store_artifact_write(id1, "feedback_positive", "proposed", "user", "", "", 1.0,
                                  "{}") == 0);
   assert(kb_store_artifact_write(id2, "feedback_negative", "proposed", "user", "", "", 1.0,
                                  "{}") == 0);
   assert(kb_store_artifact_write(id3, "feedback_positive", "committed", "user", "", "", 1.0,
                                  "{}") == 0);

   assert(kb_store_artifact_count("feedback_positive", "proposed") == 1);
   assert(kb_store_artifact_count("feedback_positive", "committed") == 1);
   assert(kb_store_artifact_count("feedback_positive", NULL) == 2);
   assert(kb_store_artifact_count(NULL, "proposed") == 2);
   assert(kb_store_artifact_count(NULL, NULL) == 3);

   close_db();
   printf("  artifact_count: ok\n");
}

/* ---- 7. learning_evidence_write_feedback ---- */
static void test_learning_evidence_feedback(void)
{
   open_db();

   char artifact_id[64];
   int rc = learning_evidence_write_feedback("positive", "Always run tests", "good practice", NULL,
                                             artifact_id, sizeof(artifact_id));
   assert(rc == 0);
   assert(strlen(artifact_id) == 36);
   assert(kb_store_artifact_count("feedback_positive", "proposed") == 1);

   char artifact_id2[64];
   int rc2 = learning_evidence_write_feedback("negative", "Never force push main", NULL, "jbailes",
                                              artifact_id2, sizeof(artifact_id2));
   assert(rc2 == 0);
   assert(strlen(artifact_id2) == 36);
   assert(kb_store_artifact_count("feedback_negative", "proposed") == 1);

   /* invalid polarity maps to feedback_negative kind */
   char artifact_id3[64];
   int rc3 = learning_evidence_write_feedback("principle", "prefer small commits", NULL, NULL,
                                              artifact_id3, sizeof(artifact_id3));
   /* "principle" is not positive/negative so evidence write should still work
    * with the default kind */
   (void)rc3;

   close_db();
   printf("  learning_evidence_feedback: ok\n");
}

/* ---- 8. kb_store_artifact_list_proposed ---- */
static void test_artifact_list_proposed(void)
{
   open_db();

   /* Write two proposed artifacts with different surfaces */
   char id1[37], id2[37], id3[37];
   kb_store_artifact_gen_id(id1, sizeof(id1));
   kb_store_artifact_gen_id(id2, sizeof(id2));
   kb_store_artifact_gen_id(id3, sizeof(id3));

   /* id1: proposed memory */
   kb_store_artifact_write(id1, "preference", "proposed", "user", "u1", "", 0.9,
                           "{\"content\":\"prefer short commits\"}");
   char long_source_id[700];
   memset(long_source_id, 'x', sizeof(long_source_id) - 1);
   long_source_id[0] = 'p';
   long_source_id[1] = ':';
   long_source_id[sizeof(long_source_id) - 1] = '\0';
   assert(kb_store_artifact_cite(id1, "kb_file", long_source_id) == 0);
   /* need to set target_surface manually via set_state won't help; write inserts '' for
    * target_surface. Use raw update to set target_surface for test */
   /* Actually: kb_store_artifact_write doesn't accept target_surface param — that's correct per the
    * header. list_proposed with NULL surface should still return all proposed rows. */

   /* id2: proposed rule, set to committed (should not appear in proposed list) */
   kb_store_artifact_write(id2, "rule", "proposed", "user", "u1", "", 0.7,
                           "{\"rule\":\"be concise\"}");
   kb_store_artifact_set_state(id2, "committed");

   /* id3: proposed preference */
   kb_store_artifact_write(id3, "preference", "proposed", "user", "u1", "", 0.6,
                           "{\"content\":\"prefer clear names\"}");

   /* List all proposed: should have id1 and id3, not id2 */
   kb_store_artifact_proposed_t rows[10];
   int n = kb_store_artifact_list_proposed(NULL, 20, rows, 10);
   assert(n >= 2);
   /* Check no committed artifact appears */
   for (int i = 0; i < n; i++)
   {
      assert(strcmp(rows[i].id, id2) != 0);
      if (strcmp(rows[i].id, id1) == 0)
         assert(strstr(rows[i].citation_ids, long_source_id) != NULL);
   }

   close_db();
   printf("  artifact_list_proposed: ok\n");
}

/* ---- 9. kb_store_artifact_reject ---- */
static void test_artifact_reject(void)
{
   open_db();

   char id[37];
   kb_store_artifact_gen_id(id, sizeof(id));
   kb_store_artifact_write(id, "preference", "proposed", "user", "u1", "", 0.8,
                           "{\"content\":\"test reject\"}");

   int rc = kb_store_artifact_reject(id, "wrong", "user", "this is wrong", "{}");
   assert(rc == 0);

   /* After reject, count of proposed should not include this id */
   kb_store_artifact_proposed_t rows[10];
   int n = kb_store_artifact_list_proposed(NULL, 20, rows, 10);
   for (int i = 0; i < n; i++)
      assert(strcmp(rows[i].id, id) != 0);

   close_db();
   printf("  artifact_reject: ok\n");
}

/* ---- 10. kb_store_artifact_stamp_reflected ---- */
static void test_artifact_stamp_reflected(void)
{
   open_db();

   char id[37];
   kb_store_artifact_gen_id(id, sizeof(id));
   kb_store_artifact_write(id, "session_summary", "proposed", "user", "u1", "", 0.5, "{}");

   int rc = kb_store_artifact_stamp_reflected(id);
   assert(rc == 0);

   close_db();
   printf("  artifact_stamp_reflected: ok\n");
}

/* ---- 11. kb_store_artifact_invalidate_citing: whole-source invalidation ---- */
static void test_artifact_invalidate_citing(void)
{
   open_db();

   char a[37], b[37], c[37], d[37];
   kb_store_artifact_gen_id(a, sizeof(a));
   kb_store_artifact_gen_id(b, sizeof(b));
   kb_store_artifact_gen_id(c, sizeof(c));
   kb_store_artifact_gen_id(d, sizeof(d));

   /* a (proposed) and b (committed) cite doc-1; both should go stale.
    * c cites a different source; d cites doc-1 but is already rejected. */
   kb_store_artifact_write(a, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   kb_store_artifact_write(b, "doc_summary", "committed", "project", "p", "", 0.9, "{}");
   kb_store_artifact_write(c, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   kb_store_artifact_write(d, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   assert(kb_store_artifact_cite(a, "kb_doc", "doc-1") == 0);
   assert(kb_store_artifact_cite(b, "kb_doc", "doc-1") == 0);
   assert(kb_store_artifact_cite(c, "kb_doc", "doc-2") == 0);
   assert(kb_store_artifact_cite(d, "kb_doc", "doc-1") == 0);
   assert(kb_store_artifact_reject(d, "x", "y", "z", "{}") == 0);

   int n = kb_store_artifact_invalidate_citing("kb_doc", "doc-1", 0, 0);
   assert(n == 2);
   assert(kb_store_artifact_count("doc_summary", "stale") == 2);    /* a, b */
   assert(kb_store_artifact_count("doc_summary", "proposed") == 1); /* c untouched */
   assert(kb_store_artifact_count("doc_summary", "rejected") == 1); /* d untouched */

   /* Idempotent: nothing live cites doc-1 anymore. */
   assert(kb_store_artifact_invalidate_citing("kb_doc", "doc-1", 0, 0) == 0);

   close_db();
   printf("  artifact_invalidate_citing: ok\n");
}

/* ---- 12. kb_store_artifact_invalidate_citing: span overlap ---- */
static void test_artifact_invalidate_span_overlap(void)
{
   open_db();

   char e[37], f[37];
   kb_store_artifact_gen_id(e, sizeof(e));
   kb_store_artifact_gen_id(f, sizeof(f));
   kb_store_artifact_write(e, "claim", "committed", "project", "p", "", 0.9, "{}");
   kb_store_artifact_write(f, "claim", "committed", "project", "p", "", 0.9, "{}");

   /* Spanned citations (kb_store_artifact_cite only emits 0,0, so seed directly):
    * e cites doc-9 [10,20), f cites doc-9 [100,110). */
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO artifact_citations (artifact_id, source_kind, source_id, span_start, "
            "span_end) VALUES ('%s','kb_doc','doc-9',10,20),('%s','kb_doc','doc-9',100,110)",
            e, f);
   exec_ok(sql);

   /* An edit at [5,15) overlaps e's span but not f's. */
   int n = kb_store_artifact_invalidate_citing("kb_doc", "doc-9", 5, 15);
   assert(n == 1);
   assert(kb_store_artifact_count("claim", "stale") == 1);     /* e */
   assert(kb_store_artifact_count("claim", "committed") == 1); /* f untouched */

   close_db();
   printf("  artifact_invalidate_span_overlap: ok\n");
}

/* ---- 13. kb_store_artifact_filter_facets: typed-facet precision ---- */
static void test_artifact_filter_facets(void)
{
   open_db();

   char a[37], b[37], c[37], d[37];
   kb_store_artifact_gen_id(a, sizeof(a));
   kb_store_artifact_gen_id(b, sizeof(b));
   kb_store_artifact_gen_id(c, sizeof(c));
   kb_store_artifact_gen_id(d, sizeof(d));

   /* a: done + pgvector (target). b: done but no pgvector. c: draft + pgvector.
    * d: matches facets but is rejected (not live). */
   kb_store_artifact_write(
       a, "doc_summary", "committed", "project", "p", "", 0.9,
       "{\"status\":\"done\",\"priority\":\"high\",\"components\":[\"pgvector\",\"kb\"]}");
   kb_store_artifact_write(b, "doc_summary", "committed", "project", "p", "", 0.9,
                           "{\"status\":\"done\",\"components\":[\"auth\"]}");
   kb_store_artifact_write(c, "doc_summary", "proposed", "project", "p", "", 0.9,
                           "{\"status\":\"draft\",\"components\":[\"pgvector\"]}");
   kb_store_artifact_write(d, "doc_summary", "proposed", "project", "p", "", 0.9,
                           "{\"status\":\"done\",\"components\":[\"pgvector\"]}");
   assert(kb_store_artifact_reject(d, "x", "y", "z", "{}") == 0);

   kb_store_artifact_row_t rows[16];

   /* status=done AND component=pgvector → only a (precision 1.0). */
   int n =
       kb_store_artifact_filter_facets(0, NULL, "doc_summary", "done", NULL, "pgvector", rows, 16);
   assert(n == 1);
   assert(strcmp(rows[0].id, a) == 0);

   /* component=pgvector alone, live only → a and c (not d, rejected). */
   n = kb_store_artifact_filter_facets(0, NULL, NULL, NULL, NULL, "pgvector", rows, 16);
   assert(n == 2);
   for (int i = 0; i < n; i++)
      assert(strcmp(rows[i].id, d) != 0);

   /* priority=high → only a. */
   n = kb_store_artifact_filter_facets(0, NULL, NULL, NULL, "high", NULL, rows, 16);
   assert(n == 1 && strcmp(rows[0].id, a) == 0);

   /* kind filter excludes everything when it doesn't match. */
   n = kb_store_artifact_filter_facets(0, NULL, "code_unit", "done", NULL, "pgvector", rows, 16);
   assert(n == 0);

   /* No facets → all live doc_summary (a, b, c), not d. */
   n = kb_store_artifact_filter_facets(0, NULL, "doc_summary", NULL, NULL, NULL, rows, 16);
   assert(n == 3);

   /* Project-local search never leaks an otherwise matching artifact from a
    * different project; explicit all (NULL above) remains available. */
   char other[37];
   kb_store_artifact_gen_id(other, sizeof(other));
   kb_store_artifact_write(other, "doc_summary", "committed", "project", "q", "", 0.9,
                           "{\"status\":\"done\",\"components\":[\"pgvector\"]}");
   n = kb_store_artifact_filter_facets(0, "p", "doc_summary", "done", NULL, "pgvector", rows, 16);
   assert(n == 1 && strcmp(rows[0].id, a) == 0);
   n = kb_store_artifact_filter_facets(0, "q", "doc_summary", "done", NULL, "pgvector", rows, 16);
   assert(n == 1 && strcmp(rows[0].id, other) == 0);
   n = kb_store_artifact_filter_facets_scoped(0, NULL, "p", "doc_summary", "done", NULL, "pgvector",
                                              rows, 16);
   assert(n == 1 && strcmp(rows[0].id, other) == 0);

   close_db();
   printf("  artifact_filter_facets: ok\n");
}

/* ---- 14. kb_store_artifact_filter_facets: release binding ---- */
static void test_artifact_filter_facets_release(void)
{
   open_db();

   /* Two docs; release 1 contains only doc 10. Issued one statement at a time:
    * libpq's prepared-statement path (unlike sqlite3_exec) takes a single command
    * per call, so a semicolon-joined script is a syntax error there. */
   exec_ok("INSERT INTO docs (id, content_hash, filename) VALUES "
           "(10,'h10','in.md'),(20,'h20','out.md')");
   exec_ok("INSERT INTO doc_releases (id, name, state) VALUES (1,'r1','active')");
   exec_ok("INSERT INTO release_docs (release_id, doc_id) VALUES (1,10)");

   char in_rel[37], out_rel[37], other_project[37];
   kb_store_artifact_gen_id(in_rel, sizeof(in_rel));
   kb_store_artifact_gen_id(out_rel, sizeof(out_rel));
   kb_store_artifact_gen_id(other_project, sizeof(other_project));
   kb_store_artifact_write(in_rel, "doc_summary", "committed", "project", "p", "", 0.9,
                           "{\"status\":\"done\"}");
   kb_store_artifact_write(out_rel, "doc_summary", "committed", "project", "p", "", 0.9,
                           "{\"status\":\"done\"}");
   kb_store_artifact_write(other_project, "doc_summary", "committed", "project", "q", "", 0.9,
                           "{\"status\":\"done\"}");
   assert(kb_store_artifact_cite(in_rel, "kb_document", "10") == 0);  /* in release 1 */
   assert(kb_store_artifact_cite(out_rel, "kb_document", "20") == 0); /* not in release 1 */
   assert(kb_store_artifact_cite(other_project, "kb_document", "10") == 0);

   kb_store_artifact_row_t rows[16];

   /* Bound to release 1 → only the artifact citing doc 10. */
   int n = kb_store_artifact_filter_facets(1, NULL, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 2);
   int saw_in_rel = 0, saw_other_project = 0;
   for (int i = 0; i < n; i++)
   {
      saw_in_rel |= strcmp(rows[i].id, in_rel) == 0;
      saw_other_project |= strcmp(rows[i].id, other_project) == 0;
   }
   assert(saw_in_rel && saw_other_project);

   /* Combining release, kind, and project keeps each SQL binding independent. */
   n = kb_store_artifact_filter_facets(1, "p", "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 1 && strcmp(rows[0].id, in_rel) == 0);
   n = kb_store_artifact_filter_facets(1, "q", "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 1 && strcmp(rows[0].id, other_project) == 0);
   n = kb_store_artifact_filter_facets(1, "p", "code_unit", "done", NULL, NULL, rows, 16);
   assert(n == 0);

   /* Unbound (release_id <= 0) → both. */
   n = kb_store_artifact_filter_facets(0, NULL, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 3);

   /* Bound to a release with no docs → nothing. */
   n = kb_store_artifact_filter_facets(99, NULL, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 0);

   close_db();
   printf("  artifact_filter_facets_release: ok\n");
}

/* ---- 18. rejection suppression: post-thumbs-down candidate gen yields fewer ---- */

/* ---- 24. evidence_vectors embed-ops queue lifecycle ---- */
static void test_evidence_vectors_queue(void)
{
   open_db();

   /* An evidence artifact (FK target for the ops queue). */
   char a[37];
   kb_store_artifact_gen_id(a, sizeof(a));
   kb_store_artifact_write(a, "session_turn", "proposed", "project", "p", "", 0.9, "{}");

   /* enqueue → one pending op; idempotent. */
   assert(kb_store_evidence_enqueue(a, "evidence") == 0);
   assert(kb_store_evidence_enqueue(a, "evidence") == 0); /* ON CONFLICT DO NOTHING */
   assert(kb_store_evidence_ops_count("pending") == 1);
   assert(kb_store_evidence_ops_count(NULL) == 1);

   kb_store_evidence_pending_t pend[8];
   assert(kb_store_evidence_list_pending(pend, 8) == 1);
   assert(strcmp(pend[0].artifact_id, a) == 0);
   assert(strcmp(pend[0].collection, "evidence") == 0);

   /* store the embedding → op moves to ok, a vector row exists. */
   char vec[EMBEDDING_LITERAL_MAX];
   assert(kb_store_evidence_store_vector(a, "evidence", embedding_literal(vec, sizeof(vec), 0.5)) ==
          0);
   assert(kb_store_evidence_ops_count("pending") == 0);
   assert(kb_store_evidence_ops_count("ok") == 1);
   assert(scalar_q("SELECT COUNT(*) FROM evidence_vectors WHERE artifact_id = ?1", a, NULL) == 1);

   /* mark_failed bumps attempts; reset_stuck returns it to pending. */
   char b[37];
   kb_store_artifact_gen_id(b, sizeof(b));
   kb_store_artifact_write(b, "session_turn", "proposed", "project", "p", "", 0.9, "{}");
   assert(kb_store_evidence_enqueue(b, "evidence") == 0);
   assert(kb_store_evidence_mark_failed(b, "embedder down") == 0);
   assert(kb_store_evidence_mark_failed(b, "embedder down") == 0);
   assert(kb_store_evidence_ops_count("failed") == 1);
   assert(kb_store_evidence_reset_stuck(2) == 1); /* attempts>=2 reset */
   assert(kb_store_evidence_ops_count("pending") == 1);
   assert(kb_store_evidence_ops_count("failed") == 0);

   close_db();
   printf("  evidence_vectors_queue: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("artifacts:\n");

   test_artifact_write();
   test_artifact_set_state();
   test_synthesis_commit_emits_mdl_features();
   test_artifact_cite();
   test_artifact_link();
   test_audit_event_write();
   test_artifact_count();
   test_learning_evidence_feedback();
   test_artifact_list_proposed();
   test_artifact_reject();
   test_artifact_stamp_reflected();
   test_artifact_invalidate_citing();
   test_artifact_invalidate_span_overlap();
   test_artifact_filter_facets();
   test_artifact_filter_facets_release();
   test_evidence_vectors_queue();

   printf("All artifacts tests passed.\n");
   return 0;
}
