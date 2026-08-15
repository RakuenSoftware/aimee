/* test_corpus_structural.c: corpus doc classification, section trees, and refs. */

#include "artifacts.h"
#include "corpus_structural.h"
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

static void query_text(const char *sql, char *out, size_t out_len)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   const char *v = aimee_pg_column_text(st, 0);
   snprintf(out, out_len, "%s", v ? v : "");
   aimee_pg_finalize(st);
}

static void test_classification_and_audit(void)
{
   open_db();

   int64_t code_id = db2_kb_doc_write("hash-code", "src/example.c", "global", "code", "",
                                      "```c\nint main(void) { return 0; }\n```\n", NULL);
   assert(code_id > 0);
   assert(db2_corpus_classify_doc(code_id, "tester") == 0);

   char doc_type[64];
   query_text("SELECT doc_type FROM docs WHERE filename = 'src/example.c'", doc_type,
              sizeof(doc_type));
   assert(strcmp(doc_type, "code") == 0);
   assert(query_int("SELECT COUNT(*) FROM audit_events WHERE target_surface = 'docs'") == 1);

   close_db();
   printf("  classification_and_audit: ok\n");
}

static void test_sections_and_doc_section_citation(void)
{
   open_db();

   const char *body = "# Title\n\nIntro.\n\n## Child\n\nChild text.\n\n### Deep\n\nDeep text.\n";
   int64_t doc_id = db2_kb_doc_write("hash-sections", "docs/proposals/pending/sample.md", "global",
                                     "passthrough", "", body, NULL);
   assert(doc_id > 0);
   assert(db2_corpus_sections_rebuild(doc_id) == 3);

   db2_corpus_section_t rows[8];
   int n = db2_corpus_sections_list(doc_id, rows, 8);
   assert(n == 3);
   assert(strcmp(rows[0].heading, "Title") == 0);
   assert(rows[0].depth == 1);
   assert(rows[0].parent_id == 0);
   assert(strcmp(rows[1].heading_path, "Title > Child") == 0);
   assert(rows[1].depth == 2);
   assert(rows[1].parent_id == rows[0].id);
   assert(strcmp(rows[2].heading_path, "Title > Child > Deep") == 0);
   assert(rows[2].parent_id == rows[1].id);
   assert(rows[0].span_start < rows[1].span_start);
   assert(rows[1].span_start < rows[2].span_start);
   assert(rows[1].span_end >= rows[2].span_end);

   char artifact_id[37];
   db2_artifact_gen_id(artifact_id, sizeof(artifact_id));
   assert(db2_artifact_write(artifact_id, "claim", "committed", "global", "", "", 0.9,
                             "{\"claim\":\"sample\"}") == 0);
   char section_id[32];
   snprintf(section_id, sizeof(section_id), "%lld", (long long)rows[1].id);
   assert(db2_artifact_cite(artifact_id, "doc_section", section_id) == 0);
   assert(query_int("SELECT COUNT(*) FROM artifact_citations WHERE source_kind = 'doc_section'") ==
          1);

   close_db();
   printf("  sections_and_doc_section_citation: ok\n");
}

static void test_references_resolve_and_stale(void)
{
   open_db();

   int64_t target = db2_kb_doc_write("hash-target", "docs/other-proposal.md", "global",
                                     "passthrough", "", "# Target\n", NULL);
   assert(target > 0);
   const char *source_body =
       "# Source\n\nSee other-proposal.md for details.\n\n## Links\n\nAlso see missing.md and "
       "[the target](docs/other-proposal.md).\n";
   int64_t source = db2_kb_doc_write("hash-source", "docs/source.md", "global", "passthrough", "",
                                     source_body, NULL);
   assert(source > 0);
   assert(db2_corpus_sections_rebuild(source) == 2);
   assert(db2_corpus_extract_references(source) >= 2);

   db2_corpus_reference_t refs[8];
   int n = db2_corpus_references_list(source, refs, 8);
   assert(n >= 2);

   int saw_resolved = 0;
   int saw_unresolved = 0;
   for (int i = 0; i < n; i++)
   {
      if (refs[i].to_doc_id == target && strcmp(refs[i].resolution, "resolved") == 0)
         saw_resolved = 1;
      if (strcmp(refs[i].raw_target, "missing.md") == 0 &&
          strcmp(refs[i].resolution, "unresolved") == 0)
         saw_unresolved = 1;
   }
   assert(saw_resolved);
   assert(saw_unresolved);

   assert(db2_corpus_mark_references_stale_for_doc(target) >= 1);
   n = db2_corpus_references_list(source, refs, 8);
   int saw_stale = 0;
   for (int i = 0; i < n; i++)
      if (refs[i].to_doc_id == target && strcmp(refs[i].resolution, "stale") == 0)
         saw_stale = 1;
   assert(saw_stale);

   close_db();
   printf("  references_resolve_and_stale: ok\n");
}

int main(void)
{
   printf("corpus_structural:\n");
   test_classification_and_audit();
   test_sections_and_doc_section_citation();
   test_references_resolve_and_stale();
   printf("corpus_structural: all tests passed\n");
   return 0;
}
