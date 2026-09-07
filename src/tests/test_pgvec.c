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

static void test_collection_names(void)
{
   const char *mem = PGVEC_MEMORY_TABLE;
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

static void test_collection_readiness_accepts_supported_ann_indexes(void)
{
   assert(pgvec_table_ready(PGVEC_MEMORY_TABLE) == 1); /* HNSW */
   assert(pgvec_table_ready(PGVEC_KB_TABLE) == 1);     /* DiskANN */
   /* Existing indexes satisfy ensure without trying to create a redundant
    * second ANN index (SQLite cannot execute either backend's DDL). */
   assert(pgvec_ensure_index(PGVEC_MEMORY_TABLE, 4, 0) == 0);
   assert(pgvec_ensure_index(PGVEC_KB_TABLE, 4, 0) == 0);
   printf("pgvec: HNSW and DiskANN both satisfy collection readiness OK\n");
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

static void test_latency_snapshot(void)
{
   int64_t total, count, maxv;
   pgvec_search_latency_snapshot(&total, &count, &maxv, 1);
   assert(total >= 0 && count >= 0 && maxv >= 0);
   printf("pgvec: latency snapshot callable OK\n");
}

static void test_public_api_symbols(void)
{
   /* Verify the DB2-owned KB vector API resolves at link time. Memory-vector
    * ownership and its ABI contract are covered by server-go/modules/memory. */
   (void)pgvec_kb_vector_collection_name;
   (void)pgvec_kb_vector_upsert_document;
   (void)pgvec_kb_vector_upsert_document_batch;
   (void)pgvec_kb_vector_delete_point;
   (void)pgvec_kb_vector_delete_project;
   (void)pgvec_kb_vector_search_project;

   printf("pgvec: all public API symbols resolve OK\n");
}

/* pgvectorscale is the DEFAULT index, and these cases exist because the tree
 * used to say otherwise in two places at once: this policy returned HNSW for an
 * unset setting and gated "auto" behind a million-row threshold, while
 * schema.sql created DiskANN whenever the extension was present. Two policies,
 * opposite defaults, and no production caller of this one to make the
 * disagreement visible. */
static void test_corpus_index_type_defaults_to_diskann(void)
{
   /* Unset, "auto" and "diskann" all mean the same thing: use the access method
    * this system indexes with, whenever it is installed. */
   const char *t = pgvec_corpus_index_type(NULL, 0, 1, 0);
   assert(strcmp(t, "diskann") == 0);
   t = pgvec_corpus_index_type("", 0, 1, 0);
   assert(strcmp(t, "diskann") == 0);
   t = pgvec_corpus_index_type("auto", 0, 1, 0);
   assert(strcmp(t, "diskann") == 0);
   t = pgvec_corpus_index_type("diskann", 0, 1, 0);
   assert(strcmp(t, "diskann") == 0);

   /* A corpus of ZERO rows still gets DiskANN. The old threshold would have sent
    * it to HNSW, which is exactly backwards for a small corpus of wide vectors:
    * HNSW caps at 2000 dimensions and DiskANN does not, so the threshold sent
    * the one case that cannot use HNSW straight to it. */
   t = pgvec_corpus_index_type("auto", 0, 1, 1000000);
   assert(strcmp(t, "diskann") == 0);
   printf("pgvec: corpus_index_type defaults to diskann OK\n");
}

static void test_corpus_index_type_falls_back_without_the_extension(void)
{
   /* Without pgvectorscale there is only one answer available. */
   const char *t = pgvec_corpus_index_type(NULL, 0, 0, 0);
   assert(strcmp(t, "hnsw") == 0);
   t = pgvec_corpus_index_type("auto", 2000000, 0, 0);
   assert(strcmp(t, "hnsw") == 0);
   t = pgvec_corpus_index_type("diskann", 0, 0, 0);
   assert(strcmp(t, "hnsw") == 0);

   /* An operator who names HNSW gets it even where DiskANN is available: a
    * setting that was silently overridden would be a lie. */
   t = pgvec_corpus_index_type("hnsw", 999999999, 1, 0);
   assert(strcmp(t, "hnsw") == 0);
   printf("pgvec: corpus_index_type falls back without vectorscale OK\n");
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

int main(void)
{
   db2_test_shim_open();
   /* These tests upsert tiny 4-dim vectors; declare that dim so the upsert
    * dim guard (pgvec_*_upsert vs db2_embedding_dim) accepts them. */
   db2_set_embedding_dim(4);

   test_collection_names();
   test_collection_readiness_accepts_supported_ann_indexes();
   test_schema_version_nonempty();
   test_upsert_graceful_on_no_db();
   test_search_graceful_on_no_db();
   test_scroll_graceful_on_no_db();
   test_latency_snapshot();
   test_public_api_symbols();
   test_corpus_index_type_defaults_to_diskann();
   test_corpus_index_type_falls_back_without_the_extension();
   test_corpus_ensure_index_graceful();

   db2_test_shim_close();
   printf("pgvec: all tests passed\n");
   return 0;
}
