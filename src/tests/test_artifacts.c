/* test_artifacts.c — unit tests for the charter artifact tables.
 *
 * Tests:
 *   1. db2_artifact_write: inserts a row; duplicate id is silently ignored.
 *   2. db2_artifact_set_state: transitions artifact state.
 *   3. db2_artifact_cite: inserts artifact_citations row.
 *   4. db2_artifact_link: inserts artifact_links row.
 *   5. db2_audit_event_write: inserts audit_events row.
 *   6. db2_artifact_count: counts by kind and state.
 *   7. learning_evidence_write_feedback: writes positive/negative evidence.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "artifacts.h"
#include "anti_patterns.h"
#include "evidence_vectors.h"
#include "feature_rows.h"
#include "db2_test_shim.h"
#include "cJSON.h"
#include "learning_evidence.h"

/* Stub the memory-store typed verb so learning_promote's memory dispatch is
 * exercisable without linking the whole memory subsystem. The real
 * db2_kb_service_memory_insert_json (memory_insert path) is covered by the
 * memory tests; here we just confirm promote routes to it and records the
 * audit. Records the last call so the test can assert routing. */
static int g_mem_insert_calls;
static char g_mem_insert_kind[64];
cJSON *db2_kb_service_memory_insert_json(const char *tier, const char *kind, const char *key,
                                         const char *content, double confidence,
                                         const char *session_id)
{
   (void)tier;
   (void)key;
   (void)content;
   (void)confidence;
   (void)session_id;
   g_mem_insert_calls++;
   snprintf(g_mem_insert_kind, sizeof(g_mem_insert_kind), "%s", kind ? kind : "");
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", 4242);
   return resp;
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
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. artifact_write ---- */
static void test_artifact_write(void)
{
   open_db();

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   assert(strlen(id) == 36);

   int rc = db2_artifact_write(id, "feedback_positive", "proposed", "user", "jbailes", "jbailes",
                               1.0, "{\"title\":\"test\"}");
   assert(rc == 0);

   /* duplicate id must be silently ignored, not error */
   int rc2 = db2_artifact_write(id, "feedback_positive", "proposed", "user", "jbailes", "jbailes",
                                1.0, "{\"title\":\"test\"}");
   assert(rc2 == 0);

   /* count must be 1 (not 2) because duplicate was ignored */
   int count = db2_artifact_count("feedback_positive", "proposed");
   assert(count == 1);

   close_db();
   printf("  artifact_write: ok\n");
}

/* ---- 2. artifact_set_state ---- */
static void test_artifact_set_state(void)
{
   open_db();

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   assert(db2_artifact_write(id, "session_summary", "proposed", "user", "", "", 1.0, "{}") == 0);
   assert(db2_artifact_set_state(id, "committed") == 0);

   assert(db2_artifact_count("session_summary", "committed") == 1);
   assert(db2_artifact_count("session_summary", "proposed") == 0);

   close_db();
   printf("  artifact_set_state: ok\n");
}

static void test_synthesis_commit_emits_mdl_features(void)
{
   open_db();

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   const char *payload =
       "{\"candidate\":\"Use bearer-token authentication.\","
       "\"evidence_bundle\":\"The API accepts Bearer tokens in the Authorization header.\","
       "\"rank_in_cluster\":2}";
   int rc = db2_artifact_write(id, "synthesis", "proposed", "project", "aimee", "", 0.9, payload);
   assert(rc == 0);
   assert(db2_artifact_set_state(id, "committed") == 0);

   char features[512];
   assert(db2_feature_row_read(id, "artifact", "mdl-v1", features, sizeof(features)) == 0);
   assert(strstr(features, "\"mdl.l_candidate\":") != NULL);
   assert(strstr(features, "\"mdl.l_residual\":") != NULL);
   assert(strstr(features, "\"mdl.total\":") != NULL);
   assert(strstr(features, "\"mdl.rank_in_cluster\":2") != NULL);

   close_db();
   printf("  synthesis_commit_emits_mdl_features: ok\n");
}

/* ---- 3. artifact_cite ---- */
static void test_artifact_cite(void)
{
   open_db();

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   assert(db2_artifact_write(id, "feedback_negative", "proposed", "user", "", "", 0.9, "{}") == 0);

   assert(db2_artifact_cite(id, "session_turn", "sess-123") == 0);
   /* duplicate must be silently ignored */
   assert(db2_artifact_cite(id, "session_turn", "sess-123") == 0);

   close_db();
   printf("  artifact_cite: ok\n");
}

/* ---- 4. artifact_link ---- */
static void test_artifact_link(void)
{
   open_db();

   char id_a[64], id_b[64];
   db2_artifact_gen_id(id_a, sizeof(id_a));
   db2_artifact_gen_id(id_b, sizeof(id_b));

   assert(db2_artifact_write(id_a, "session_summary", "proposed", "user", "", "", 1.0, "{}") == 0);
   assert(db2_artifact_write(id_b, "session_summary", "proposed", "user", "", "", 1.0, "{}") == 0);

   assert(db2_artifact_link(id_a, id_b, "supersedes") == 0);
   /* duplicate must be silently ignored */
   assert(db2_artifact_link(id_a, id_b, "supersedes") == 0);

   close_db();
   printf("  artifact_link: ok\n");
}

/* ---- 5. audit_event_write ---- */
static void test_audit_event_write(void)
{
   open_db();

   char art_id[64], evt_id[64];
   db2_artifact_gen_id(art_id, sizeof(art_id));
   db2_artifact_gen_id(evt_id, sizeof(evt_id));

   assert(db2_artifact_write(art_id, "feedback_positive", "committed", "user", "jbailes", "jbailes",
                             1.0, "{}") == 0);

   int rc = db2_audit_event_write(evt_id, art_id, "rule", "rule-42", "jbailes", "user", "jbailes",
                                  1.0, 0, NULL, "{\"weight\":50}");
   assert(rc == 0);

   /* duplicate id must be silently ignored */
   int rc2 = db2_audit_event_write(evt_id, art_id, "rule", "rule-42", "jbailes", "user", "jbailes",
                                   1.0, 0, NULL, "{\"weight\":50}");
   assert(rc2 == 0);

   close_db();
   printf("  audit_event_write: ok\n");
}

/* ---- 6. artifact_count ---- */
static void test_artifact_count(void)
{
   open_db();

   char id1[64], id2[64], id3[64];
   db2_artifact_gen_id(id1, sizeof(id1));
   db2_artifact_gen_id(id2, sizeof(id2));
   db2_artifact_gen_id(id3, sizeof(id3));

   assert(db2_artifact_write(id1, "feedback_positive", "proposed", "user", "", "", 1.0, "{}") == 0);
   assert(db2_artifact_write(id2, "feedback_negative", "proposed", "user", "", "", 1.0, "{}") == 0);
   assert(db2_artifact_write(id3, "feedback_positive", "committed", "user", "", "", 1.0, "{}") ==
          0);

   assert(db2_artifact_count("feedback_positive", "proposed") == 1);
   assert(db2_artifact_count("feedback_positive", "committed") == 1);
   assert(db2_artifact_count("feedback_positive", NULL) == 2);
   assert(db2_artifact_count(NULL, "proposed") == 2);
   assert(db2_artifact_count(NULL, NULL) == 3);

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
   assert(db2_artifact_count("feedback_positive", "proposed") == 1);

   char artifact_id2[64];
   int rc2 = learning_evidence_write_feedback("negative", "Never force push main", NULL, "jbailes",
                                              artifact_id2, sizeof(artifact_id2));
   assert(rc2 == 0);
   assert(strlen(artifact_id2) == 36);
   assert(db2_artifact_count("feedback_negative", "proposed") == 1);

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

/* ---- 8. db2_artifact_list_proposed ---- */
static void test_artifact_list_proposed(void)
{
   open_db();

   /* Write two proposed artifacts with different surfaces */
   char id1[37], id2[37], id3[37];
   db2_artifact_gen_id(id1, sizeof(id1));
   db2_artifact_gen_id(id2, sizeof(id2));
   db2_artifact_gen_id(id3, sizeof(id3));

   /* id1: proposed memory */
   db2_artifact_write(id1, "preference", "proposed", "user", "u1", "", 0.9,
                      "{\"content\":\"prefer short commits\"}");
   char long_source_id[700];
   memset(long_source_id, 'x', sizeof(long_source_id) - 1);
   long_source_id[0] = 'p';
   long_source_id[1] = ':';
   long_source_id[sizeof(long_source_id) - 1] = '\0';
   assert(db2_artifact_cite(id1, "kb_file", long_source_id) == 0);
   /* need to set target_surface manually via set_state won't help; write inserts '' for
    * target_surface. Use raw update to set target_surface for test */
   /* Actually: db2_artifact_write doesn't accept target_surface param — that's correct per the
    * header. list_proposed with NULL surface should still return all proposed rows. */

   /* id2: proposed rule, set to committed (should not appear in proposed list) */
   db2_artifact_write(id2, "rule", "proposed", "user", "u1", "", 0.7, "{\"rule\":\"be concise\"}");
   db2_artifact_set_state(id2, "committed");

   /* id3: proposed preference */
   db2_artifact_write(id3, "preference", "proposed", "user", "u1", "", 0.6,
                      "{\"content\":\"prefer clear names\"}");

   /* List all proposed: should have id1 and id3, not id2 */
   db2_artifact_proposed_t rows[10];
   int n = db2_artifact_list_proposed(NULL, 20, rows, 10);
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

/* ---- 9. db2_artifact_reject ---- */
static void test_artifact_reject(void)
{
   open_db();

   char id[37];
   db2_artifact_gen_id(id, sizeof(id));
   db2_artifact_write(id, "preference", "proposed", "user", "u1", "", 0.8,
                      "{\"content\":\"test reject\"}");

   int rc = db2_artifact_reject(id, "wrong", "user", "this is wrong", "{}");
   assert(rc == 0);

   /* After reject, count of proposed should not include this id */
   db2_artifact_proposed_t rows[10];
   int n = db2_artifact_list_proposed(NULL, 20, rows, 10);
   for (int i = 0; i < n; i++)
      assert(strcmp(rows[i].id, id) != 0);

   close_db();
   printf("  artifact_reject: ok\n");
}

/* ---- 10. db2_artifact_stamp_reflected ---- */
static void test_artifact_stamp_reflected(void)
{
   open_db();

   char id[37];
   db2_artifact_gen_id(id, sizeof(id));
   db2_artifact_write(id, "session_summary", "proposed", "user", "u1", "", 0.5, "{}");

   int rc = db2_artifact_stamp_reflected(id);
   assert(rc == 0);

   close_db();
   printf("  artifact_stamp_reflected: ok\n");
}

/* ---- 11. db2_artifact_invalidate_citing: whole-source invalidation ---- */
static void test_artifact_invalidate_citing(void)
{
   open_db();

   char a[37], b[37], c[37], d[37];
   db2_artifact_gen_id(a, sizeof(a));
   db2_artifact_gen_id(b, sizeof(b));
   db2_artifact_gen_id(c, sizeof(c));
   db2_artifact_gen_id(d, sizeof(d));

   /* a (proposed) and b (committed) cite doc-1; both should go stale.
    * c cites a different source; d cites doc-1 but is already rejected. */
   db2_artifact_write(a, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   db2_artifact_write(b, "doc_summary", "committed", "project", "p", "", 0.9, "{}");
   db2_artifact_write(c, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   db2_artifact_write(d, "doc_summary", "proposed", "project", "p", "", 0.9, "{}");
   assert(db2_artifact_cite(a, "kb_doc", "doc-1") == 0);
   assert(db2_artifact_cite(b, "kb_doc", "doc-1") == 0);
   assert(db2_artifact_cite(c, "kb_doc", "doc-2") == 0);
   assert(db2_artifact_cite(d, "kb_doc", "doc-1") == 0);
   assert(db2_artifact_reject(d, "x", "y", "z", "{}") == 0);

   int n = db2_artifact_invalidate_citing("kb_doc", "doc-1", 0, 0);
   assert(n == 2);
   assert(db2_artifact_count("doc_summary", "stale") == 2);    /* a, b */
   assert(db2_artifact_count("doc_summary", "proposed") == 1); /* c untouched */
   assert(db2_artifact_count("doc_summary", "rejected") == 1); /* d untouched */

   /* Idempotent: nothing live cites doc-1 anymore. */
   assert(db2_artifact_invalidate_citing("kb_doc", "doc-1", 0, 0) == 0);

   close_db();
   printf("  artifact_invalidate_citing: ok\n");
}

/* ---- 12. db2_artifact_invalidate_citing: span overlap ---- */
static void test_artifact_invalidate_span_overlap(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   char e[37], f[37];
   db2_artifact_gen_id(e, sizeof(e));
   db2_artifact_gen_id(f, sizeof(f));
   db2_artifact_write(e, "claim", "committed", "project", "p", "", 0.9, "{}");
   db2_artifact_write(f, "claim", "committed", "project", "p", "", 0.9, "{}");

   /* Spanned citations (db2_artifact_cite only emits 0,0, so seed directly):
    * e cites doc-9 [10,20), f cites doc-9 [100,110). */
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO artifact_citations (artifact_id, source_kind, source_id, span_start, "
            "span_end) VALUES ('%s','kb_doc','doc-9',10,20),('%s','kb_doc','doc-9',100,110)",
            e, f);
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

   /* An edit at [5,15) overlaps e's span but not f's. */
   int n = db2_artifact_invalidate_citing("kb_doc", "doc-9", 5, 15);
   assert(n == 1);
   assert(db2_artifact_count("claim", "stale") == 1);     /* e */
   assert(db2_artifact_count("claim", "committed") == 1); /* f untouched */

   close_db();
   printf("  artifact_invalidate_span_overlap: ok\n");
}

/* ---- 13. db2_artifact_filter_facets: typed-facet precision ---- */
static void test_artifact_filter_facets(void)
{
   open_db();

   char a[37], b[37], c[37], d[37];
   db2_artifact_gen_id(a, sizeof(a));
   db2_artifact_gen_id(b, sizeof(b));
   db2_artifact_gen_id(c, sizeof(c));
   db2_artifact_gen_id(d, sizeof(d));

   /* a: done + pgvector (target). b: done but no pgvector. c: draft + pgvector.
    * d: matches facets but is rejected (not live). */
   db2_artifact_write(
       a, "doc_summary", "committed", "project", "p", "", 0.9,
       "{\"status\":\"done\",\"priority\":\"high\",\"components\":[\"pgvector\",\"kb\"]}");
   db2_artifact_write(b, "doc_summary", "committed", "project", "p", "", 0.9,
                      "{\"status\":\"done\",\"components\":[\"auth\"]}");
   db2_artifact_write(c, "doc_summary", "proposed", "project", "p", "", 0.9,
                      "{\"status\":\"draft\",\"components\":[\"pgvector\"]}");
   db2_artifact_write(d, "doc_summary", "proposed", "project", "p", "", 0.9,
                      "{\"status\":\"done\",\"components\":[\"pgvector\"]}");
   assert(db2_artifact_reject(d, "x", "y", "z", "{}") == 0);

   db2_artifact_row_t rows[16];

   /* status=done AND component=pgvector → only a (precision 1.0). */
   int n = db2_artifact_filter_facets(0, "doc_summary", "done", NULL, "pgvector", rows, 16);
   assert(n == 1);
   assert(strcmp(rows[0].id, a) == 0);

   /* component=pgvector alone, live only → a and c (not d, rejected). */
   n = db2_artifact_filter_facets(0, NULL, NULL, NULL, "pgvector", rows, 16);
   assert(n == 2);
   for (int i = 0; i < n; i++)
      assert(strcmp(rows[i].id, d) != 0);

   /* priority=high → only a. */
   n = db2_artifact_filter_facets(0, NULL, NULL, "high", NULL, rows, 16);
   assert(n == 1 && strcmp(rows[0].id, a) == 0);

   /* kind filter excludes everything when it doesn't match. */
   n = db2_artifact_filter_facets(0, "code_unit", "done", NULL, "pgvector", rows, 16);
   assert(n == 0);

   /* No facets → all live doc_summary (a, b, c), not d. */
   n = db2_artifact_filter_facets(0, "doc_summary", NULL, NULL, NULL, rows, 16);
   assert(n == 3);

   close_db();
   printf("  artifact_filter_facets: ok\n");
}

/* ---- 14. db2_artifact_filter_facets: release binding ---- */
static void test_artifact_filter_facets_release(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* Two docs; release 1 contains only doc 10. */
   assert(sqlite3_exec(db,
                       "INSERT INTO docs (id, content_hash, filename) VALUES "
                       "(10,'h10','in.md'),(20,'h20','out.md');"
                       "INSERT INTO doc_releases (id, name, state) VALUES (1,'r1','active');"
                       "INSERT INTO release_docs (release_id, doc_id) VALUES (1,10);",
                       NULL, NULL, NULL) == SQLITE_OK);

   char in_rel[37], out_rel[37];
   db2_artifact_gen_id(in_rel, sizeof(in_rel));
   db2_artifact_gen_id(out_rel, sizeof(out_rel));
   db2_artifact_write(in_rel, "doc_summary", "committed", "project", "p", "", 0.9,
                      "{\"status\":\"done\"}");
   db2_artifact_write(out_rel, "doc_summary", "committed", "project", "p", "", 0.9,
                      "{\"status\":\"done\"}");
   assert(db2_artifact_cite(in_rel, "kb_document", "10") == 0);  /* in release 1 */
   assert(db2_artifact_cite(out_rel, "kb_document", "20") == 0); /* not in release 1 */

   db2_artifact_row_t rows[16];

   /* Bound to release 1 → only the artifact citing doc 10. */
   int n = db2_artifact_filter_facets(1, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 1);
   assert(strcmp(rows[0].id, in_rel) == 0);

   /* Unbound (release_id <= 0) → both. */
   n = db2_artifact_filter_facets(0, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 2);

   /* Bound to a release with no docs → nothing. */
   n = db2_artifact_filter_facets(99, "doc_summary", "done", NULL, NULL, rows, 16);
   assert(n == 0);

   close_db();
   printf("  artifact_filter_facets_release: ok\n");
}

/* ---- 15. learning_evidence_write_event: ingest + content-hash dedup ---- */
static void test_evidence_write_event(void)
{
   open_db();

   /* Each non-doc kind writes one proposed evidence artifact under its kind. */
   char id_session[37], id_tool[37], id_guard[37];
   assert(learning_evidence_write_event("session_turn", "project", "p", "ran git status", "op",
                                        id_session, sizeof(id_session)) == 0);
   assert(learning_evidence_write_event("tool_outcome", "project", "p", "Bash exit 0", "op",
                                        id_tool, sizeof(id_tool)) == 0);
   assert(learning_evidence_write_event("guardrail_event", "project", "p", "force-push flagged",
                                        "op", id_guard, sizeof(id_guard)) == 0);
   assert(id_session[0] && id_tool[0] && id_guard[0]);
   assert(db2_artifact_count("session_turn", "proposed") == 1);
   assert(db2_artifact_count("tool_outcome", "proposed") == 1);
   assert(db2_artifact_count("guardrail_event", "proposed") == 1);

   /* Idempotent: re-ingesting identical content returns the same id, no dup. */
   char id_dup[37];
   assert(learning_evidence_write_event("session_turn", "project", "p", "ran git status", "op",
                                        id_dup, sizeof(id_dup)) == 0);
   assert(strcmp(id_dup, id_session) == 0);
   assert(db2_artifact_count("session_turn", "proposed") == 1);

   /* Different content under the same kind → a distinct artifact. */
   char id_other[37];
   assert(learning_evidence_write_event("session_turn", "project", "p", "ran git log", "op",
                                        id_other, sizeof(id_other)) == 0);
   assert(strcmp(id_other, id_session) != 0);
   assert(db2_artifact_count("session_turn", "proposed") == 2);

   /* Scope and content_hash are recorded on the row. */
   db2_artifact_row_t row;
   assert(db2_artifact_read(id_tool, &row, NULL, 0, NULL) == 0);
   assert(strcmp(row.scope_kind, "project") == 0);
   assert(strcmp(row.scope_id, "p") == 0);
   assert(strstr(row.payload_json, "\"content_hash\":\"") != NULL);
   assert(strstr(row.payload_json, "\"source_kind\":\"tool_outcome\"") != NULL);

   /* Unknown / doc-side kinds are rejected by the non-doc ingest path. */
   assert(learning_evidence_write_event("doc_summary", "project", "p", "x", "op", NULL, 0) == -1);
   assert(learning_evidence_write_event(NULL, "project", "p", "x", "op", NULL, 0) == -1);

   close_db();
   printf("  evidence_write_event: ok\n");
}

/* ---- 16. learning_judge_commit: corroboration -> committed -> audit ---- */
static int audit_count_for(sqlite3 *db, const char *artifact_id, char *surface_out, int surface_len)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT COUNT(*), MAX(target_surface) FROM audit_events"
                             " WHERE source_artifact_id = ?1",
                             -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, artifact_id, -1, SQLITE_STATIC);
   int n = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      n = sqlite3_column_int(st, 0);
      const unsigned char *s = sqlite3_column_text(st, 1);
      if (surface_out && surface_len > 0)
         snprintf(surface_out, (size_t)surface_len, "%s", s ? (const char *)s : "");
   }
   sqlite3_finalize(st);
   return n;
}

/* Make a proposed candidate of `kind` cited by `n` distinct evidence sources. */
static void seed_candidate(const char *kind, int n_evidence, char *id_out)
{
   db2_artifact_gen_id(id_out, 37);
   db2_artifact_write(id_out, kind, "proposed", "project", "p", "", 0.9, "{}");
   for (int i = 0; i < n_evidence; i++)
   {
      char src[32];
      snprintf(src, sizeof(src), "ev-%s-%d", kind, i);
      assert(db2_artifact_cite(id_out, "session_turn", src) == 0);
   }
}

static void test_judge_commit(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   const char *kinds[] = {"preference", "workflow", "anti_pattern", "mistake_pattern"};
   const char *surfaces[] = {"memory", "workflow_pattern", "anti_pattern", "anti_pattern"};

   for (int i = 0; i < 4; i++)
   {
      char id[37];
      seed_candidate(kinds[i], 2, id); /* 2 distinct cited sources */
      /* Corroborated (>=2) -> committed, audit routed to the target surface. */
      assert(learning_judge_commit(id, kinds[i], 2) == 1);
      assert(db2_artifact_count(kinds[i], "committed") == 1);
      char surface[64] = "";
      assert(audit_count_for(db, id, surface, sizeof(surface)) == 1);
      assert(strcmp(surface, surfaces[i]) == 0);
   }

   /* Under-corroborated candidate stays proposed, no audit. */
   char weak[37];
   seed_candidate("preference", 1, weak);
   assert(learning_judge_commit(weak, "preference", 2) == 0);
   assert(audit_count_for(db, weak, NULL, 0) == 0);

   /* Unknown kind is rejected. */
   char any[37];
   seed_candidate("preference", 2, any);
   assert(learning_judge_commit(any, "bogus_kind", 2) == -1);

   close_db();
   printf("  judge_commit: ok\n");
}

/* ---- 17. learning_review_rollback: thumbs-down reverts + captures verdict ---- */
static void test_review_rollback(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* Commit a corroborated candidate, then thumbs-down to roll it back. */
   char id[37];
   seed_candidate("preference", 2, id);
   assert(learning_judge_commit(id, "preference", 2) == 1);
   assert(db2_artifact_count("preference", "committed") == 1);

   assert(learning_review_rollback(id, "bad_preference", "user:project", "counter example here") ==
          0);

   /* Rolled back to the before-snapshot state (proposed), not committed. */
   assert(db2_artifact_count("preference", "committed") == 0);
   assert(db2_artifact_count("preference", "proposed") == 1);

   /* The thumbs-down verdict is captured in the audit columns. */
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT verdict, verdict_tag, verdict_scope, counter_example"
                             " FROM audit_events WHERE source_artifact_id = ?1 AND verdict <> ''"
                             " ORDER BY id DESC LIMIT 1",
                             -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(st, 0), "thumbs_down") == 0);
   assert(strcmp((const char *)sqlite3_column_text(st, 1), "bad_preference") == 0);
   assert(strcmp((const char *)sqlite3_column_text(st, 2), "user:project") == 0);
   assert(strcmp((const char *)sqlite3_column_text(st, 3), "counter example here") == 0);
   sqlite3_finalize(st);

   close_db();
   printf("  review_rollback: ok\n");
}

/* ---- 18. rejection suppression: post-thumbs-down candidate gen yields fewer ---- */

/* Stand-in candidate-generation pass: of the (tag, scope) candidates a pass
 * would propose, count how many survive the rejection-suppression filter. */
static int gen_pass_survivors(const char *const (*cands)[2], int n)
{
   int survivors = 0;
   for (int i = 0; i < n; i++)
      if (!db2_artifact_verdict_suppressed(cands[i][0], cands[i][1]))
         survivors++;
   return survivors;
}

static void test_rejection_suppression(void)
{
   open_db();

   /* Three candidate (tag, scope) pairs a generation pass would propose. */
   const char *const cands[][2] = {
       {"force_push_main", "project:aimee"},
       {"no_ai_attribution", "user:jbailes"},
       {"use_index_not_grep", "user:jbailes"},
   };
   const int n = 3;

   /* Pre-rejection: nothing suppressed, all three survive. */
   assert(gen_pass_survivors(cands, n) == 3);

   /* Operator thumbs-down one candidate (commit then roll back with verdict). */
   char id[37];
   seed_candidate("preference", 2, id);
   assert(learning_judge_commit(id, "preference", 2) == 1);
   assert(learning_review_rollback(id, "no_ai_attribution", "user:jbailes", "wrong scope") == 0);

   /* Post-rejection: the rejected tag/scope is suppressed; the other two are
    * unaffected — strictly fewer proposals match the rejected combination. */
   assert(db2_artifact_verdict_suppressed("no_ai_attribution", "user:jbailes") == 1);
   assert(db2_artifact_verdict_suppressed("force_push_main", "project:aimee") == 0);
   /* Same tag, different scope is NOT suppressed. */
   assert(db2_artifact_verdict_suppressed("no_ai_attribution", "user:someone_else") == 0);
   assert(gen_pass_survivors(cands, n) == 2);

   close_db();
   printf("  rejection_suppression: ok\n");
}

/* ---- 19. learning_promote: anti_pattern target surface ---- */
static int anti_pattern_audit(sqlite3 *db, const char *artifact_id, int *flagged_out)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT target_surface, flagged_for_review FROM audit_events"
                             " WHERE source_artifact_id = ?1 AND target_surface = 'anti_pattern'"
                             " ORDER BY id DESC LIMIT 1",
                             -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, artifact_id, -1, SQLITE_STATIC);
   int found = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      found = 1;
      if (flagged_out)
         *flagged_out = sqlite3_column_int(st, 1);
   }
   sqlite3_finalize(st);
   return found;
}

static void make_committed(const char *kind, double confidence, const char *payload, char *id_out)
{
   db2_artifact_gen_id(id_out, 37);
   db2_artifact_write(id_out, kind, "committed", "project", "p", "", confidence, payload);
}

static void test_promote_anti_pattern(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* High-confidence anti_pattern candidate auto-applies into anti_patterns. */
   char ap[37];
   make_committed("anti_pattern", 0.95,
                  "{\"pattern\":\"force_push_main\",\"description\":\"never\"}", ap);
   assert(learning_promote(ap, 0.85) == 1);
   assert(db2_anti_pattern_exists_exact("force_push_main") == 1);
   int flagged = -1;
   assert(anti_pattern_audit(db, ap, &flagged) == 1 && flagged == 0);

   /* mistake_pattern also routes to the anti_pattern surface. */
   char mp[37];
   make_committed("mistake_pattern", 0.95, "{\"pattern\":\"commit_mid_verify\"}", mp);
   assert(learning_promote(mp, 0.85) == 1);
   assert(db2_anti_pattern_exists_exact("commit_mid_verify") == 1);

   /* Borderline confidence (just below threshold) auto-applies but flags. */
   char bd[37];
   make_committed("anti_pattern", 0.80, "{\"pattern\":\"borderline_one\"}", bd);
   assert(learning_promote(bd, 0.85) == 1);
   assert(db2_anti_pattern_exists_exact("borderline_one") == 1);
   flagged = -1;
   assert(anti_pattern_audit(db, bd, &flagged) == 1 && flagged == 1);

   /* Well below the window: held for review, no surface row. */
   char low[37];
   make_committed("anti_pattern", 0.50, "{\"pattern\":\"too_weak\"}", low);
   assert(learning_promote(low, 0.85) == 0);
   assert(db2_anti_pattern_exists_exact("too_weak") == 0);

   /* preference routes to the memory surface (covered by test_promote_memory). */
   char pref[37];
   make_committed("preference", 0.95, "{\"pattern\":\"x\"}", pref);
   assert(learning_promote(pref, 0.85) == 1);

   /* A proposed (not committed) candidate does not promote. */
   char prop[37];
   db2_artifact_gen_id(prop, sizeof(prop));
   db2_artifact_write(prop, "anti_pattern", "proposed", "project", "p", "", 0.95,
                      "{\"pattern\":\"y\"}");
   assert(learning_promote(prop, 0.85) == -1);

   close_db();
   printf("  promote_anti_pattern: ok\n");
}

/* ---- 20. learning_promote: memory target surface (typed-verb store) ---- */
static void test_promote_memory(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   g_mem_insert_calls = 0;
   g_mem_insert_kind[0] = '\0';

   /* A high-confidence preference candidate promotes via the memory store
    * typed verb and records a memory-surface audit row. */
   char id[37];
   make_committed("preference", 0.95, "{\"key\":\"no_ai_attribution\",\"content\":\"strip it\"}",
                  id);
   assert(learning_promote(id, 0.85) == 1);
   assert(g_mem_insert_calls == 1);
   assert(strcmp(g_mem_insert_kind, "preference") == 0);

   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT COUNT(*) FROM audit_events"
                             " WHERE source_artifact_id = ?1 AND target_surface = 'memory'"
                             "   AND target_id = '4242'",
                             -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(sqlite3_column_int(st, 0) == 1);
   sqlite3_finalize(st);

   /* Below the borderline window: held, the store verb is not called. */
   char low[37];
   make_committed("preference", 0.50, "{\"key\":\"weak\",\"content\":\"x\"}", low);
   assert(learning_promote(low, 0.85) == 0);
   assert(g_mem_insert_calls == 1); /* unchanged */

   close_db();
   printf("  promote_memory: ok\n");
}

/* ---- 21. four candidate kinds end-to-end: proposed -> committed -> audit
 *         -> target surface (preference, workflow, anti_pattern, mistake_pattern) ---- */
static void test_four_kinds_end_to_end(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   g_mem_insert_calls = 0;

   const char *kinds[] = {"preference", "workflow", "anti_pattern", "mistake_pattern"};
   const char *payloads[] = {
       "{\"key\":\"no_ai_attribution\",\"content\":\"strip it\"}",
       "{\"pattern\":\"edit-build-verify-pr\"}",
       "{\"pattern\":\"force_push_main\"}",
       "{\"pattern\":\"commit_mid_verify\"}",
   };

   for (int i = 0; i < 4; i++)
   {
      char id[37];
      db2_artifact_gen_id(id, sizeof(id));
      db2_artifact_write(id, kinds[i], "proposed", "project", "p", "", 0.95, payloads[i]);
      for (int j = 0; j < 2; j++)
      {
         char src[32];
         snprintf(src, sizeof(src), "ev-%d-%d", i, j);
         assert(db2_artifact_cite(id, "session_turn", src) == 0);
      }
      /* proposed -> committed (+ audit) -> promoted to its target surface. */
      assert(learning_judge_commit(id, kinds[i], 2) == 1);
      assert(db2_artifact_count(kinds[i], "committed") == 1);
      assert(learning_promote(id, 0.85) == 1);
   }

   /* Each kind landed in its target surface. */
   assert(g_mem_insert_calls == 1); /* preference -> memory store verb */
   assert(db2_anti_pattern_exists_exact("force_push_main") == 1);   /* anti_pattern */
   assert(db2_anti_pattern_exists_exact("commit_mid_verify") == 1); /* mistake_pattern */
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(
              db, "SELECT COUNT(*) FROM workflow_patterns WHERE pattern = 'edit-build-verify-pr'",
              -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(sqlite3_column_int(st, 0) == 1); /* workflow -> workflow_patterns */
   sqlite3_finalize(st);

   close_db();
   printf("  four_kinds_end_to_end: ok\n");
}

/* ---- 22. learning_promote: remaining DB2 surfaces via target_surface ---- */
static void test_promote_remaining_surfaces(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   const char *surfaces[] = {"rule", "epistemic_directive", "entity", "guardrail_exemplar"};
   for (int i = 0; i < 4; i++)
   {
      char id[37];
      db2_artifact_gen_id(id, sizeof(id));
      db2_artifact_write(
          id, "candidate", "committed", "project", "p", "", 0.95,
          "{\"title\":\"t1\",\"question\":\"q1\",\"alias\":\"a1\",\"node_key\":\"n1\","
          "\"content\":\"c1\"}");
      /* Explicit target_surface (as candidate generation would set) wins. */
      assert(db2_artifact_set_target_surface(id, surfaces[i]) == 0);
      assert(learning_promote(id, 0.85) == 1);

      sqlite3_stmt *st;
      assert(sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM audit_events WHERE source_artifact_id = ?1"
                                " AND target_surface = ?2",
                                -1, &st, NULL) == SQLITE_OK);
      sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 2, surfaces[i], -1, SQLITE_STATIC);
      assert(sqlite3_step(st) == SQLITE_ROW);
      assert(sqlite3_column_int(st, 0) == 1); /* promotion audit routed to the surface */
      sqlite3_finalize(st);
   }

   /* Spot-check the guardrail_exemplar registration landed an exemplar row. */
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM exemplar_vectors", -1, &st, NULL) ==
          SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(sqlite3_column_int(st, 0) == 1);
   sqlite3_finalize(st);

   close_db();
   printf("  promote_remaining_surfaces: ok\n");
}

/* ---- 23. learning_promote: working_profile surface (DB1 weak-symbol path) ---- */
static void test_promote_working_profile(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   g_wp_observe_calls = 0;

   char id[37];
   db2_artifact_gen_id(id, sizeof(id));
   db2_artifact_write(id, "candidate", "committed", "user", "jbailes", "", 0.95,
                      "{\"field\":\"verbosity\",\"value\":\"terse\"}");
   assert(db2_artifact_set_target_surface(id, "working_profile") == 0);
   assert(learning_promote(id, 0.85) == 1);
   assert(g_wp_observe_calls == 1);
   assert(strcmp(g_wp_observe_field, "verbosity") == 0);

   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT COUNT(*) FROM audit_events WHERE source_artifact_id = ?1"
                             " AND target_surface = 'working_profile'",
                             -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(sqlite3_column_int(st, 0) == 1);
   sqlite3_finalize(st);

   close_db();
   printf("  promote_working_profile: ok\n");
}

/* ---- 24. evidence_vectors embed-ops queue lifecycle ---- */
static void test_evidence_vectors_queue(void)
{
   open_db();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* An evidence artifact (FK target for the ops queue). */
   char a[37];
   db2_artifact_gen_id(a, sizeof(a));
   db2_artifact_write(a, "session_turn", "proposed", "project", "p", "", 0.9, "{}");

   /* enqueue → one pending op; idempotent. */
   assert(db2_evidence_enqueue(a, "evidence") == 0);
   assert(db2_evidence_enqueue(a, "evidence") == 0); /* ON CONFLICT DO NOTHING */
   assert(db2_evidence_ops_count("pending") == 1);
   assert(db2_evidence_ops_count(NULL) == 1);

   db2_evidence_pending_t pend[8];
   assert(db2_evidence_list_pending(pend, 8) == 1);
   assert(strcmp(pend[0].artifact_id, a) == 0);
   assert(strcmp(pend[0].collection, "evidence") == 0);

   /* store the embedding → op moves to ok, a vector row exists. */
   assert(db2_evidence_store_vector(a, "evidence", "[0.1,0.2,0.3]") == 0);
   assert(db2_evidence_ops_count("pending") == 0);
   assert(db2_evidence_ops_count("ok") == 1);
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM evidence_vectors WHERE artifact_id = ?1", -1,
                             &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
   assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
   sqlite3_finalize(st);

   /* mark_failed bumps attempts; reset_stuck returns it to pending. */
   char b[37];
   db2_artifact_gen_id(b, sizeof(b));
   db2_artifact_write(b, "session_turn", "proposed", "project", "p", "", 0.9, "{}");
   assert(db2_evidence_enqueue(b, "evidence") == 0);
   assert(db2_evidence_mark_failed(b, "embedder down") == 0);
   assert(db2_evidence_mark_failed(b, "embedder down") == 0);
   assert(db2_evidence_ops_count("failed") == 1);
   assert(db2_evidence_reset_stuck(2) == 1); /* attempts>=2 reset */
   assert(db2_evidence_ops_count("pending") == 1);
   assert(db2_evidence_ops_count("failed") == 0);

   close_db();
   printf("  evidence_vectors_queue: ok\n");
}

/* ---- 25. evidence capture enqueues for embedding ---- */
static void test_evidence_capture_enqueues(void)
{
   open_db();

   char id[64];
   assert(learning_evidence_write_event("session_turn", "project", "p", "ran git status", "op", id,
                                        sizeof(id)) == 0);
   /* Capture enqueued the new evidence artifact for embedding. */
   assert(db2_evidence_ops_count("pending") == 1);
   db2_evidence_pending_t pend[4];
   assert(db2_evidence_list_pending(pend, 4) == 1);
   assert(strcmp(pend[0].artifact_id, id) == 0);

   /* Idempotent re-capture of identical content does not double-enqueue. */
   char id2[64];
   assert(learning_evidence_write_event("session_turn", "project", "p", "ran git status", "op", id2,
                                        sizeof(id2)) == 0);
   assert(strcmp(id2, id) == 0);
   assert(db2_evidence_ops_count("pending") == 1);

   close_db();
   printf("  evidence_capture_enqueues: ok\n");
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
   test_evidence_write_event();
   test_judge_commit();
   test_review_rollback();
   test_rejection_suppression();
   test_promote_anti_pattern();
   test_promote_memory();
   test_four_kinds_end_to_end();
   test_promote_remaining_surfaces();
   test_promote_working_profile();
   test_evidence_vectors_queue();
   test_evidence_capture_enqueues();

   printf("All artifacts tests passed.\n");
   return 0;
}
