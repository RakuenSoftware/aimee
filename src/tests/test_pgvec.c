/* test_pgvec.c: smoke tests for the pgvector transport in src/modules/db2/c/.
 *
 * The test shim provides sqlite-backed aimee_pg_* stubs.  pgvec SQL will
 * fail at the statement level (sqlite does not understand the vector type or
 * <=> operator), so each pgvec call returns an error value.  These tests only
 * verify that:
 *   - All public API entry points exist and are callable
 *   - Graceful -1/NULL returns on DB unavailability
 *   - Collection/table name constants are distinct and non-empty
 *   - Latency snapshot accumulates across calls
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/lifecycle.h" /* db2_set_embedding_dim */
#include "../modules/db2/c/pgvec_transport.h"
#include "../modules/db2/c/pgvec_scope_query.h"
#include "../modules/db2/c/memory_vectors.h"
#include "../modules/db2/c/kb_vectors.h"
#include "../modules/db2/c/vector_verify.h"
#include "../modules/db2/c/pgvec_db3.h"
#include "../modules/db2/c/db_postgres.h"

static void test_collection_names(void)
{
   const char *mem = pgvec_memory_vector_collection_name();
   const char *kb = pgvec_kb_vector_collection_name();
   assert(mem && mem[0]);
   assert(kb && kb[0]);
   assert(strcmp(mem, kb) != 0);
   /* The new curator_entity_vectors table is a sibling collection, not
    * the same as memory or kb. Verify it is its own non-empty name and
    * distinct from the other two. */
   const char *cur = PGVEC_CURATOR_ENTITY_TABLE;
   assert(cur && cur[0]);
   assert(strcmp(cur, mem) != 0);
   assert(strcmp(cur, kb) != 0);
   /* The new curator_narrative_vectors table is another sibling collection.
    * Must be distinct from all of the other three. */
   const char *narr = PGVEC_CURATOR_NARRATIVE_TABLE;
   assert(narr && narr[0]);
   assert(strcmp(narr, mem) != 0);
   assert(strcmp(narr, kb) != 0);
   assert(strcmp(narr, cur) != 0);
   /* The new curator_claim_vectors table holds TWO named vector columns
    * (subj_attr_vec + value_vec) and must be distinct from the others. */
   const char *claim = PGVEC_CURATOR_CLAIM_TABLE;
   assert(claim && claim[0]);
   assert(strcmp(claim, mem) != 0);
   assert(strcmp(claim, kb) != 0);
   assert(strcmp(claim, cur) != 0);
   assert(strcmp(claim, narr) != 0);
   /* The new curator_code_unit_vectors table holds THREE named vector columns
    * (intent_vec + signature_vec + body_vec) and must be distinct from the
    * others. */
   const char *code_unit = PGVEC_CURATOR_CODE_UNIT_TABLE;
   assert(code_unit && code_unit[0]);
   assert(strcmp(code_unit, mem) != 0);
   assert(strcmp(code_unit, kb) != 0);
   assert(strcmp(code_unit, cur) != 0);
   assert(strcmp(code_unit, narr) != 0);
   assert(strcmp(code_unit, claim) != 0);
   printf("pgvec: collection names distinct and non-empty OK\n");
}

static void test_schema_version_nonempty(void)
{
   const char *v = pgvec_schema_version();
   assert(v && v[0]);
   printf("pgvec: schema version '%s' non-empty OK\n", v);
}

static void test_upsert_graceful_on_no_db(void)
{
   /* Under the sqlite shim, vector SQL fails — must not crash. */
   float vec[4] = {0.1f, 0.2f, 0.3f, 0.4f};
   int rc;

   rc = pgvec_memory_upsert(1, vec, 4, "{\"record_type\":\"memory\",\"kind\":\"fact\"}");
   (void)rc; /* -1 expected under shim; 0 if somehow succeeds */

   /* curator_entity_upsert should return the same kind of value the existing
    * kb_upsert does under the shim: it either succeeds (0) or fails (-1),
    * and never crashes. Verify it is in the same {0, -1} value set. */
   int rc_kb = pgvec_kb_upsert(1, vec, 4, "{\"project\":\"test\"}");
   int rc_ce =
       pgvec_curator_entity_upsert(1, vec, 4, "workspace", "proj", "Acme Corp", "entity-1", "{}");
   int rc_cn = pgvec_curator_narrative_upsert(1, vec, 4, "artifact-1", "doc_summary", "doc-1",
                                              "active", "high", "{}");
   /* curator_claim_upsert takes TWO vec buffers (subj_attr + value) and the
    * full set of claim columns. */
   float vec2[4] = {0.5f, 0.6f, 0.7f, 0.8f};
   int rc_cl = pgvec_curator_claim_upsert(1, vec, vec2, 4, "artifact-c1", "Alice", "role",
                                          "Engineer", "stated", "{}");
   /* curator_code_unit_upsert takes THREE vec buffers (intent + signature +
    * body) and the full set of code-unit columns. */
   float vec3[4] = {0.9f, 0.1f, 0.2f, 0.3f};
   int rc_ccu = pgvec_curator_code_unit_upsert(1, vec, vec2, vec3, 4, "artifact-u1", "src/foo.c",
                                               "function", "int foo(int x);", "deadbeef", "{}");
   /* curator_entity_lookup reverse-maps a point_id to its artifact_id.  The
    * sqlite shim may have persisted the point_id=1 upsert above, so a hit (1)
    * is valid here; -1 (no conn) and 0 (no such row) are the other graceful
    * outcomes.  The contract is simply: never crash, return value in {-1,0,1}. */
   char aid[64];
   char nm[64];
   int rc_celk = pgvec_curator_entity_lookup(1, aid, sizeof(aid), nm, sizeof(nm));
   assert(rc_celk == 1 || rc_celk == 0 || rc_celk == -1);

   assert(rc_kb == 0 || rc_kb == -1);
   assert(rc_ce == 0 || rc_ce == -1);
   assert(rc_cn == 0 || rc_cn == -1);
   assert(rc_cl == 0 || rc_cl == -1);
   assert(rc_ccu == 0 || rc_ccu == -1);

   int rc_mem_del = pgvec_memory_delete(1);
   (void)rc_mem_del;
   int rc_kb_del = pgvec_kb_delete(1);
   int rc_ce_del = pgvec_curator_entity_delete(1);
   int rc_cn_del = pgvec_curator_narrative_delete(1);
   int rc_cl_del = pgvec_curator_claim_delete(1);
   int rc_ccu_del = pgvec_curator_code_unit_delete(1);
   assert(rc_kb_del == 0 || rc_kb_del == -1);
   assert(rc_ce_del == 0 || rc_ce_del == -1);
   assert(rc_cn_del == 0 || rc_cn_del == -1);
   assert(rc_cl_del == 0 || rc_cl_del == -1);
   assert(rc_ccu_del == 0 || rc_ccu_del == -1);

   printf("pgvec: upsert/delete graceful on no-vector-db OK\n");
}

static void test_search_graceful_on_no_db(void)
{
   float vec[4] = {0.1f, 0.2f, 0.3f, 0.4f};
   int64_t ids[8];
   double scores[8];
   int n;

   n = pgvec_memory_search(vec, 4, "memory", NULL, 0, "", "", 5, ids, scores, 8);
   assert(n <= 0);

   n = pgvec_kb_search("test", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);

   n = pgvec_curator_entity_search("workspace", "proj", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* With no filters the search must also stay graceful. */
   n = pgvec_curator_entity_search(NULL, NULL, vec, 4, 5, ids, scores, 8);
   assert(n <= 0);

   n = pgvec_curator_narrative_search("doc_summary", "active", "high", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* With no filters the search must also stay graceful. */
   n = pgvec_curator_narrative_search(NULL, NULL, NULL, vec, 4, 5, ids, scores, 8);
   assert(n <= 0);

   /* curator_claim_search: rank on subj_attr_vec. */
   n = pgvec_curator_claim_search("subj_attr", "stated", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* curator_claim_search: rank on value_vec. */
   n = pgvec_curator_claim_search("value", "stated", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* With no claim_kind filter (NULL/""): still graceful. */
   n = pgvec_curator_claim_search("subj_attr", NULL, vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   n = pgvec_curator_claim_search("value", "", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* Bogus which_vec must be rejected with -1, NOT silently pass through and
    * interpolate caller text into the SQL. */
   int rc_bogus = pgvec_curator_claim_search("bogus", "stated", vec, 4, 5, ids, scores, 8);
   assert(rc_bogus == -1);
   rc_bogus = pgvec_curator_claim_search(NULL, "stated", vec, 4, 5, ids, scores, 8);
   assert(rc_bogus == -1);

   /* curator_code_unit_search: rank on each of the THREE named vector
    * columns. */
   n = pgvec_curator_code_unit_search("intent", "function", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   n = pgvec_curator_code_unit_search("signature", "function", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   n = pgvec_curator_code_unit_search("body", "function", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* With no def_kind filter (NULL/""): still graceful. */
   n = pgvec_curator_code_unit_search("intent", NULL, vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   n = pgvec_curator_code_unit_search("body", "", vec, 4, 5, ids, scores, 8);
   assert(n <= 0);
   /* Bogus which_vec must be rejected with -1, NOT silently pass through and
    * interpolate caller text into the SQL. */
   int rc_ccu_bogus =
       pgvec_curator_code_unit_search("bogus", "function", vec, 4, 5, ids, scores, 8);
   assert(rc_ccu_bogus == -1);
   rc_ccu_bogus = pgvec_curator_code_unit_search(NULL, "function", vec, 4, 5, ids, scores, 8);
   assert(rc_ccu_bogus == -1);

   printf("pgvec: search graceful on no-vector-db OK\n");
}

static void test_scroll_graceful_on_no_db(void)
{
   int64_t ids[8];
   int64_t next = -1;
   int done = 0;
   int n = pgvec_scroll(PGVEC_MEMORY_TABLE, -1, ids, 8, &next, &done);
   /* Under shim: table doesn't exist, n < 0 or n = 0 */
   (void)n;
   printf("pgvec: scroll graceful on no-vector-db OK\n");
}

static void test_scope_hints(void)
{
   pgvec_memory_vector_scope_hint_set("ws1", "proj1");
   /* Just verify it doesn't crash and sets state */
   pgvec_memory_vector_scope_hint_clear();

   pgvec_memory_vector_scope_hint_set(NULL, "myproj");
   pgvec_memory_vector_scope_hint_clear();

   printf("pgvec: scope hint set/clear OK\n");
}

static void test_memory_scope_sql_uses_canonical_owner_scope(void)
{
   const char *filter = PGVEC_MEMORY_SCOPE_FILTER_SQL;
   const char *rank = PGVEC_MEMORY_SCOPE_RANK_SQL;
   assert(strstr(filter, "memory_scopes") != NULL);
   assert(strstr(filter, "memory_workspaces") != NULL);
   assert(strstr(filter, "memory_units") != NULL);
   assert(strstr(filter, "e.point_id - 1000000000000") != NULL);
   assert(strstr(rank, "memory_workspaces") != NULL);
   printf("pgvec: canonical memory owner scope SQL OK\n");
}

static void test_latency_snapshot(void)
{
   int64_t total, count, maxv;
   pgvec_search_latency_snapshot(&total, &count, &maxv, 1);
   assert(total >= 0 && count >= 0 && maxv >= 0);
   printf("pgvec: latency snapshot callable OK\n");
}

static void test_public_api_symbols(void)
{
   /* Verify all public header symbols resolve at link time. */
   (void)pgvec_memory_vector_collection_exists;
   (void)pgvec_memory_vector_collection_recreate;
   (void)pgvec_memory_vector_ensure_payload_indexes;
   (void)pgvec_memory_vector_collection_name;
   (void)pgvec_memory_vector_upsert_memory;
   (void)pgvec_memory_vector_upsert_unit;
   (void)pgvec_memory_vector_delete_point;
   (void)pgvec_memory_vector_search_record_type;
   (void)pgvec_memory_vector_search_with_kinds;
   (void)pgvec_memory_vector_scope_hint_set;
   (void)pgvec_memory_vector_scope_hint_clear;

   (void)pgvec_kb_vector_collection_name;
   (void)pgvec_kb_vector_upsert_document;
   (void)pgvec_kb_vector_upsert_document_batch;
   (void)pgvec_kb_vector_delete_point;
   (void)pgvec_kb_vector_delete_project;
   (void)pgvec_kb_vector_search_project;

   printf("pgvec: all public API symbols resolve OK\n");
}

static void test_corpus_index_type_hnsw_default(void)
{
   /* auto + no vectorscale + below threshold → hnsw */
   const char *t = pgvec_corpus_index_type("auto", 0, 0, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   /* explicit hnsw → always hnsw regardless of scale or extension */
   t = pgvec_corpus_index_type("hnsw", 999999999, 1, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   /* NULL/empty configured → hnsw */
   t = pgvec_corpus_index_type(NULL, 0, 1, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   printf("pgvec: corpus_index_type default hnsw OK\n");
}

static void test_corpus_index_type_diskann(void)
{
   /* diskann forced + vectorscale present → diskann */
   const char *t = pgvec_corpus_index_type("diskann", 0, 1, 1000000);
   assert(strcmp(t, "diskann") == 0);
   /* diskann forced + vectorscale absent → hnsw fallback */
   t = pgvec_corpus_index_type("diskann", 0, 0, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   /* auto + above threshold + vectorscale present → diskann */
   t = pgvec_corpus_index_type("auto", 2000000, 1, 1000000);
   assert(strcmp(t, "diskann") == 0);
   /* auto + at threshold + vectorscale present → diskann */
   t = pgvec_corpus_index_type("auto", 1000000, 1, 1000000);
   assert(strcmp(t, "diskann") == 0);
   /* auto + above threshold + vectorscale absent → hnsw */
   t = pgvec_corpus_index_type("auto", 2000000, 0, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   /* auto + below threshold + vectorscale present → hnsw */
   t = pgvec_corpus_index_type("auto", 999999, 1, 1000000);
   assert(strcmp(t, "hnsw") == 0);
   printf("pgvec: corpus_index_type diskann selection OK\n");
}

static void test_corpus_ensure_index_graceful(void)
{
   /* Under the sqlite shim, pgvec SQL fails gracefully — must not crash */
   int rc = pgvec_ensure_corpus_index("corpus_section_vectors", "hnsw", 0);
   (void)rc;
   rc = pgvec_ensure_corpus_index("corpus_section_vectors", "diskann", 0);
   (void)rc;
   rc = pgvec_ensure_corpus_index("corpus_section_vectors", "hnsw", 1);
   (void)rc;
   rc = pgvec_ensure_corpus_index(NULL, "hnsw", 0);
   assert(rc == -1);
   printf("pgvec: corpus ensure_index graceful OK\n");
}

/* --- The routing decision the search wrappers make -----------------------
 *
 * pgvec_code_search, pgvec_kb_search and pgvec_kbpdf_search each answer the
 * same question before they do anything: can this search be expressed as
 * filters a provider could serve, or must it stay on pgvector? The answer turns
 * on resolving the project's current generation, because the pgvector form
 * expresses "current" as a JOIN and a provider cannot join.
 *
 * That decision had no test at all, which is how pgvec_code_search came to be
 * declared in the header with no definition behind it: nothing called the
 * symbol, so nothing missed it. These cases call the wrappers and assert on
 * what the provider was asked, so the next time the wrapper is renamed or the
 * condition is dropped, something says so.
 *
 * The generation lookup is ordinary SQL over `projects`, which the sqlite shim
 * executes, so the decision is reachable here even though the pgvector query
 * behind it is not. */

typedef struct
{
   int calls;
   char last_project[AIMEE_DB3_MAX_SCOPE];
   uint32_t last_filter_count;
   char last_filter_key[AIMEE_DB3_MAX_LABEL_KEY];
   char last_filter_value[AIMEE_DB3_MAX_LABEL_VALUE];
} route_probe_t;

static int probe_search(void *context, const aimee_db3_search_request_t *request,
                        aimee_db3_search_reply_t *reply)
{
   route_probe_t *probe = context;
   probe->calls++;
   snprintf(probe->last_project, sizeof(probe->last_project), "%s", request->project);
   probe->last_filter_count = request->filter_count;
   probe->last_filter_key[0] = probe->last_filter_value[0] = '\0';
   if (request->filter_count > 0)
   {
      snprintf(probe->last_filter_key, sizeof(probe->last_filter_key), "%s",
               request->filters[0].key);
      snprintf(probe->last_filter_value, sizeof(probe->last_filter_value), "%s",
               request->filters[0].value);
   }
   reply->request_id = request->request_id;
   reply->generation = request->required_generation;
   reply->count = 1;
   reply->candidates[0].point_id = 77;
   reply->candidates[0].score = 0.5;
   return 0;
}

static int probe_allow(void *context, const char *workspace, const char *project, int64_t point_id)
{
   (void)context;
   (void)workspace;
   (void)project;
   (void)point_id;
   return 1;
}

static void seed_project(const char *name, const char *lifecycle, long long generation)
{
   char sql[512];
   char errbuf[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO projects (name, root, scanned_at, lifecycle_state, current_generation) "
            "VALUES ('%s', '/tmp/%s', '2026-01-01', '%s', %lld)",
            name, name, lifecycle, generation);
   (void)aimee_pg_exec(db2_conn(), sql, errbuf, sizeof(errbuf));
}

static void test_search_routing_decision(void)
{
   float vec[4] = {1.0f, 0.0f, 0.0f, 0.0f};
   int64_t ids[8];
   double scores[8];

   seed_project("routed", "current", 7);
   seed_project("gone", "detached", 5);

   route_probe_t code = {0};
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_CODE, probe_search, &code, probe_allow,
                                  &code) == 0);
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_CODE, 456, 1, 0, probe_search, &code) == 0);

   /* A current project routes, and carries its resolved generation as an exact
    * filter -- the whole point of the join-to-filter reduction. */
   int n = pgvec_code_search("routed", vec, 4, 5, ids, scores, 8);
   assert(n == 1 && ids[0] == 77);
   assert(code.calls == 1);
   assert(strcmp(code.last_project, "routed") == 0);
   assert(code.last_filter_count == 1);
   assert(strcmp(code.last_filter_key, "generation") == 0);
   assert(strcmp(code.last_filter_value, "7") == 0);

   /* A detached project resolves to nothing. Routing it without the lifecycle
    * condition would answer from the generation it was detached at, so the
    * search must stay on pgvector -- which under the shim means it fails rather
    * than reaching the provider. */
   int before = code.calls;
   (void)pgvec_code_search("gone", vec, 4, 5, ids, scores, 8);
   assert(code.calls == before);

   /* A project-less search spans every project, each with its own current
    * generation. That is a per-row condition, not one filter, so it does not
    * route either. */
   (void)pgvec_code_search("", vec, 4, 5, ids, scores, 8);
   assert(code.calls == before);
   pgvec_db3_route_clear(PGVEC_DB3_COLLECTION_CODE);

   /* kb and kb_pdf resolve the same value through the same helper. */
   route_probe_t kb = {0};
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_KB, probe_search, &kb, probe_allow, &kb) ==
          0);
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_KB, 456, 1, 0, probe_search, &kb) == 0);
   n = pgvec_kb_search("routed", vec, 4, 5, ids, scores, 8);
   assert(n == 1 && ids[0] == 77);
   assert(kb.calls == 1 && strcmp(kb.last_filter_key, "generation") == 0);
   assert(strcmp(kb.last_filter_value, "7") == 0);

   /* An excluded project cannot be sent as an equality filter, so a scoped
    * search with one stays on pgvector even though the project is current. */
   before = kb.calls;
   (void)pgvec_kb_search_scoped("routed", "other", vec, 4, 5, ids, scores, 8);
   assert(kb.calls == before);
   pgvec_db3_route_clear(PGVEC_DB3_COLLECTION_KB);

   route_probe_t pdf = {0};
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_KB_PDF, probe_search, &pdf, probe_allow,
                                  &pdf) == 0);
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_KB_PDF, 456, 1, 0, probe_search, &pdf) == 0);
   n = pgvec_kbpdf_search("routed", vec, 4, 5, ids, scores, 8);
   assert(n == 1 && ids[0] == 77);
   assert(pdf.calls == 1 && strcmp(pdf.last_filter_value, "7") == 0);
   pgvec_db3_route_clear(PGVEC_DB3_COLLECTION_KB_PDF);

   printf("pgvec: search routing decision OK\n");
}

int main(void)
{
   db2_test_shim_open();
   /* These tests upsert tiny 4-dim vectors; declare that dim so the upsert
    * dim guard (pgvec_*_upsert vs db2_embedding_dim) accepts them. */
   db2_set_embedding_dim(4);

   test_collection_names();
   test_schema_version_nonempty();
   test_upsert_graceful_on_no_db();
   test_search_graceful_on_no_db();
   test_scroll_graceful_on_no_db();
   test_scope_hints();
   test_memory_scope_sql_uses_canonical_owner_scope();
   test_latency_snapshot();
   test_public_api_symbols();
   test_corpus_index_type_hnsw_default();
   test_corpus_index_type_diskann();
   test_corpus_ensure_index_graceful();
   test_search_routing_decision();

   db2_test_shim_close();
   printf("pgvec: all tests passed\n");
   return 0;
}
