/* test_pgvec_generation_pg.c: the kb and kb_pdf embedding rows carry the
 * generation of the document they embed.
 *
 * Needs a live Postgres with pgvector: the upsert inserts through a
 * `::vector` cast and sources the generation from a subselect over
 * kb_documents, and the SQLite shim can execute neither. Reads
 * AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) when it is unset, mirroring
 * test_content_scope_pg.c, so `make unit-tests` stays green without one.
 *
 * WHY THIS EXISTS AT ALL. Routing a kb search means sending the project's
 * current generation as an exact filter, because a provider cannot join
 * kb_documents the way the pgvector query does. That only answers correctly if
 * the embedding row actually carries the right generation. The single existing
 * test that touches pgvec_kb_upsert asserts that it returns 0 or -1 and does
 * not crash -- it never looks at the row. So the write half of the routed path
 * had no coverage at all, and the failure it would miss is silent: a NULL
 * generation matches no filter, so every routed kb search returns nothing and
 * looks like a corpus with no hits rather than a broken write.
 *
 * WHAT IS PINNED HERE:
 *
 *  1. The generation comes from the DOCUMENT, not from the caller. The upsert
 *     takes no generation argument; it resolves one through a subselect keyed on
 *     point_id, which IS kb_documents.id. This asserts the value that lands
 *     equals the document's rather than a default or a zero.
 *  2. The subselect binds :point_id TWICE in one statement (once in VALUES,
 *     once inside the subselect). That relies on the parameter rewriter mapping
 *     a repeated name to one $N. If it ever stopped, every kb embedding would
 *     take a NULL generation, so the reuse is pinned rather than assumed.
 *  3. Re-upserting the SAME point after its document moves to a new generation
 *     carries the new value through ON CONFLICT. A stale generation here is the
 *     re-scan case, and it would hide freshly embedded rows from every routed
 *     search.
 *  4. A point with NO kb_documents row leaves the generation NULL rather than
 *     failing or defaulting. That is the orphan case the schema comment calls
 *     out: the pgvector join already excludes it, and NULL is what makes the
 *     routed form exclude it too.
 *  5. kb_pdf_embeddings does all of the above, because it is a separate
 *     relation with its own copy of the write path.
 */
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/pgvec_transport.h"
#include "modules/db2/c/db_schema.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vector column's width is fixed when the schema is APPLIED --
 * `vector(__EMBED_DIM__)` is substituted then -- so a test cannot pick its own.
 * A four-float vector against a database applied at the default width is
 * rejected by the column, which is what the first run of this test hit. Read
 * the applied width instead and build vectors to match. */
static int g_dim;

static void exec_or_die(const char *sql)
{
   char errbuf[512] = "";
   if (aimee_pg_exec(db2_conn(), sql, errbuf, sizeof(errbuf)) != 0)
   {
      fprintf(stderr, "pgvec_generation_pg: exec failed: %s\n  sql: %s\n", errbuf, sql);
      exit(1);
   }
}

/* Read the generation a vector row carries. Returns 0 and sets *is_null when the
 * column is NULL, which is a DISTINCT outcome from any value and the whole point
 * of case 4. Returns -1 when no row exists at all. */
static int read_generation(const char *table, int64_t point_id, int64_t *out, int *is_null)
{
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT generation FROM %s WHERE point_id = :point_id", table);
   char errbuf[512] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(db2_conn(), sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      fprintf(stderr, "pgvec_generation_pg: prepare failed: %s\n", errbuf);
      exit(1);
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   int found = -1;
   if (aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
   {
      *is_null = aimee_pg_column_is_null(stmt, 0);
      *out = *is_null ? 0 : aimee_pg_column_int64(stmt, 0);
      found = 0;
   }
   aimee_pg_finalize(stmt);
   return found;
}

static void seed_document(int64_t id, const char *project, long long generation)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO kb_documents "
            "  (id, project, generation, file_path, file_hash, chunk_index, content) "
            "VALUES (%lld, '%s', %lld, '/t/%lld.md', 'hash-%lld', 0, 'body') "
            "ON CONFLICT (id) DO UPDATE SET generation = EXCLUDED.generation",
            (long long)id, project, generation, (long long)id, (long long)id);
   exec_or_die(sql);
}

static void check(const char *table, int (*upsert)(int64_t, const float *, int, const char *),
                  const char *label)
{
   float *vec = calloc((size_t)g_dim, sizeof(float));
   if (!vec)
   {
      fprintf(stderr, "%s: out of memory for a %d-wide vector\n", label, g_dim);
      exit(1);
   }
   vec[0] = 1.0f;
   int64_t generation = 0;
   int is_null = 0;

   /* 1 + 2: the document's generation is what lands, through a statement that
    * binds :point_id twice. */
   seed_document(9001, "genproj", 3);
   if (upsert(9001, vec, g_dim, "{\"project\":\"genproj\"}") != 0)
   {
      fprintf(stderr, "%s: upsert of a documented point failed\n", label);
      exit(1);
   }
   if (read_generation(table, 9001, &generation, &is_null) != 0)
   {
      fprintf(stderr, "%s: upsert wrote no row\n", label);
      exit(1);
   }
   if (is_null || generation != 3)
   {
      fprintf(stderr, "%s: generation is %s%lld, expected 3\n", label, is_null ? "NULL/" : "",
              (long long)generation);
      exit(1);
   }
   printf("%s: ok, the embedding carries the document's generation\n", label);

   /* 3: a re-scan moves the document forward; ON CONFLICT must carry it. */
   seed_document(9001, "genproj", 4);
   if (upsert(9001, vec, g_dim, "{\"project\":\"genproj\"}") != 0)
   {
      fprintf(stderr, "%s: re-upsert failed\n", label);
      exit(1);
   }
   if (read_generation(table, 9001, &generation, &is_null) != 0 || is_null || generation != 4)
   {
      fprintf(stderr, "%s: re-upsert left generation at %s%lld, expected 4\n", label,
              is_null ? "NULL/" : "", (long long)generation);
      exit(1);
   }
   printf("%s: ok, ON CONFLICT carries the new generation\n", label);

   /* 4: an orphan takes NULL rather than a default, so no generation filter
    * matches it and the routed form excludes it exactly as the join does. */
   if (upsert(9002, vec, g_dim, "{\"project\":\"genproj\"}") != 0)
   {
      fprintf(stderr, "%s: upsert of an undocumented point failed\n", label);
      exit(1);
   }
   if (read_generation(table, 9002, &generation, &is_null) != 0)
   {
      fprintf(stderr, "%s: orphan upsert wrote no row\n", label);
      exit(1);
   }
   if (!is_null)
   {
      fprintf(stderr, "%s: orphan generation is %lld, expected NULL\n", label,
              (long long)generation);
      exit(1);
   }
   printf("%s: ok, a point with no document row takes a NULL generation\n", label);
   free(vec);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("pgvec_generation_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "pgvec_generation_pg: db2_init failed for %s\n", url);
      return 1;
   }
   /* The upsert guards the vector length against the DECLARED dimension, and the
    * column enforces the APPLIED one. They have to be the same number, and the
    * applied one is the fact on disk, so take it from there. */
   g_dim = db2_embedding_dim_get(db2_conn());
   if (g_dim <= 0)
   {
      fprintf(stderr, "pgvec_generation_pg: no schema_embedding_dim recorded\n");
      return 1;
   }
   db2_set_embedding_dim(g_dim);
   printf("pgvec_generation_pg: schema applied at %d dimensions\n", g_dim);

   exec_or_die("DELETE FROM kb_embeddings WHERE point_id IN (9001, 9002)");
   exec_or_die("DELETE FROM kb_pdf_embeddings WHERE point_id IN (9001, 9002)");
   exec_or_die("DELETE FROM kb_documents WHERE id IN (9001, 9002)");

   check("kb_embeddings", pgvec_kb_upsert, "kb");
   check("kb_pdf_embeddings", pgvec_kbpdf_upsert, "kb_pdf");

   exec_or_die("DELETE FROM kb_embeddings WHERE point_id IN (9001, 9002)");
   exec_or_die("DELETE FROM kb_pdf_embeddings WHERE point_id IN (9001, 9002)");
   exec_or_die("DELETE FROM kb_documents WHERE id IN (9001, 9002)");

   printf("pgvec_generation_pg: all checks passed\n");
   return 0;
}
