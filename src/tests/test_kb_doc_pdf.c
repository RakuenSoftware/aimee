/* test_kb_doc_pdf.c: structured-PDF Phase 1 (first increment). Pure tests of the
 * parse->normalize->chunk pipeline over `pdftotext -bbox-layout` XHTML, plus a
 * shim-backed ingest test asserting kb_documents + kb_doc_regions + embed enqueue. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "../db2/db_postgres.h"
#include "../db2/kb_payload.h"
#include "kb_doc_pdf.h"
#include "kb_http_pdf.h"
#include "kb_tsr_sidecar.h"
#include "support/mock_agent_http.h"

#define PASS(name) printf("  PASS: %s\n", (name))

/* A minimal two-page bbox-layout fixture: page 1 has two lines, page 2 one line.
 * Coords are raw points; page boxes are 600x800. */
static const char *FIXTURE_2PAGE =
    "<html><body><doc>\n"
    "<page width=\"600.000000\" height=\"800.000000\">\n"
    "<flow><block>\n"
    "<line xMin=\"60\" yMin=\"40\" xMax=\"300\" yMax=\"60\">"
    "<word xMin=\"60\" yMin=\"40\" xMax=\"150\" yMax=\"60\">Hello</word>"
    "<word xMin=\"160\" yMin=\"40\" xMax=\"300\" yMax=\"60\">world</word></line>\n"
    "<line xMin=\"60\" yMin=\"80\" xMax=\"420\" yMax=\"100\">"
    "<word xMin=\"60\" yMin=\"80\" xMax=\"420\" yMax=\"100\">A&amp;B &lt;tag&gt;</word></line>\n"
    "</block></flow>\n"
    "</page>\n"
    "<page width=\"600.000000\" height=\"800.000000\">\n"
    "<flow><block>\n"
    "<line xMin=\"60\" yMin=\"40\" xMax=\"900\" yMax=\"60\">"
    "<word xMin=\"60\" yMin=\"40\" xMax=\"900\" yMax=\"60\">Second page</word></line>\n"
    "</block></flow>\n"
    "</page>\n"
    "</doc></body></html>\n";

static void test_parse(void)
{
   kb_pdf_doc_t doc;
   assert(kb_pdf_parse_bbox_layout(FIXTURE_2PAGE, &doc) == 0);
   assert(doc.n_pages == 2);
   assert(doc.pages[0].width == 600.0 && doc.pages[0].height == 800.0);
   assert(doc.pages[0].n_lines == 2);
   assert(doc.pages[1].n_lines == 1);
   assert(strcmp(doc.pages[0].lines[0].text, "Hello world") == 0);
   /* entity decode */
   assert(strcmp(doc.pages[0].lines[1].text, "A&B <tag>") == 0);
   assert(strcmp(doc.pages[1].lines[0].text, "Second page") == 0);
   assert(doc.pages[0].lines[0].page_no == 1 && doc.pages[1].lines[0].page_no == 2);
   /* raw (un-normalized) bbox */
   assert(doc.pages[0].lines[0].x0 == 60.0 && doc.pages[0].lines[0].x1 == 300.0);
   kb_pdf_free_doc(&doc);
   PASS("parse");
}

static void test_normalize(void)
{
   kb_pdf_doc_t doc;
   assert(kb_pdf_parse_bbox_layout(FIXTURE_2PAGE, &doc) == 0);
   kb_pdf_normalize(&doc);
   assert(doc.normalized);
   kb_pdf_line_t *l = &doc.pages[0].lines[0];
   /* 60/600=0.1, 40/800=0.05, 300/600=0.5, 60/800=0.075 */
   assert(l->x0 > 0.099 && l->x0 < 0.101);
   assert(l->y0 > 0.049 && l->y0 < 0.051);
   assert(l->x1 > 0.499 && l->x1 < 0.501);
   /* all within [0,1] */
   for (int p = 0; p < doc.n_pages; p++)
      for (int i = 0; i < doc.pages[p].n_lines; i++)
      {
         kb_pdf_line_t *q = &doc.pages[p].lines[i];
         assert(q->x0 >= 0 && q->x0 <= 1 && q->y0 >= 0 && q->y0 <= 1);
         assert(q->x1 >= 0 && q->x1 <= 1 && q->y1 >= 0 && q->y1 <= 1);
      }
   /* page-2 line had xMax=900 > width 600 -> clamps to 1.0 */
   assert(doc.pages[1].lines[0].x1 == 1.0);
   kb_pdf_free_doc(&doc);
   PASS("normalize");
}

static void test_chunk_page_boundary(void)
{
   kb_pdf_doc_t doc;
   assert(kb_pdf_parse_bbox_layout(FIXTURE_2PAGE, &doc) == 0);
   kb_pdf_normalize(&doc);
   kb_pdf_chunk_t *chunks = NULL;
   int n = 0;
   assert(kb_pdf_chunk(&doc, &chunks, &n) == 2); /* page-boundary: one chunk per page */
   /* chunk 0 = page 1 (2 lines), chunk 1 = page 2 (1 line) */
   assert(chunks[0].page_start == 1 && chunks[0].page_end == 1);
   assert(chunks[1].page_start == 2 && chunks[1].page_end == 2);
   assert(chunks[0].n_lines == 2 && chunks[1].n_lines == 1);
   /* content joins the page's lines with '\n' */
   assert(strcmp(chunks[0].content, "Hello world\nA&B <tag>") == 0);
   /* global line ordinals are contiguous across chunks */
   assert(chunks[0].line_start == 0 && chunks[0].line_end == 1);
   assert(chunks[1].line_start == 2 && chunks[1].line_end == 2);
   kb_pdf_free_chunks(chunks, n);
   kb_pdf_free_doc(&doc);
   PASS("chunk_page_boundary");
}

/* Build a one-page fixture with `nlines` lines to exercise the line-count cap. */
static char *build_big_page(int nlines)
{
   size_t cap = (size_t)nlines * 120 + 256;
   char *b = malloc(cap);
   size_t off = 0;
   off += (size_t)snprintf(b + off, cap - off,
                           "<doc><page width=\"600\" height=\"800\"><flow><block>\n");
   for (int i = 0; i < nlines; i++)
      off += (size_t)snprintf(b + off, cap - off,
                              "<line xMin=\"10\" yMin=\"%d\" xMax=\"20\" yMax=\"%d\">"
                              "<word xMin=\"10\" yMin=\"%d\" xMax=\"20\" yMax=\"%d\">w%d</word>"
                              "</line>\n",
                              i, i + 1, i, i + 1, i);
   snprintf(b + off, cap - off, "</block></flow></page></doc>\n");
   return b;
}

static void test_chunk_line_cap(void)
{
   char *xhtml = build_big_page(250); /* > 2 * cap(100) */
   kb_pdf_doc_t doc;
   assert(kb_pdf_parse_bbox_layout(xhtml, &doc) == 0);
   assert(doc.pages[0].n_lines == 250);
   kb_pdf_chunk_t *chunks = NULL;
   int n = 0;
   assert(kb_pdf_chunk(&doc, &chunks, &n) == 3); /* 100 + 100 + 50 */
   assert(chunks[0].n_lines == KB_PDF_MAX_CHUNK_LINES);
   assert(chunks[1].n_lines == KB_PDF_MAX_CHUNK_LINES);
   assert(chunks[2].n_lines == 50);
   /* all same page despite the split */
   for (int i = 0; i < n; i++)
      assert(chunks[i].page_start == 1 && chunks[i].page_end == 1);
   kb_pdf_free_chunks(chunks, n);
   kb_pdf_free_doc(&doc);
   free(xhtml);
   PASS("chunk_line_cap");
}

static void test_degraded_no_line_tags(void)
{
   /* A page with bare <word>s and no <line> wrapper -> one line per word, no text lost. */
   const char *x = "<doc><page width=\"600\" height=\"800\">"
                   "<word xMin=\"10\" yMin=\"10\" xMax=\"50\" yMax=\"20\">alpha</word>"
                   "<word xMin=\"60\" yMin=\"10\" xMax=\"99\" yMax=\"20\">beta</word>"
                   "</page></doc>";
   kb_pdf_doc_t doc;
   assert(kb_pdf_parse_bbox_layout(x, &doc) == 0);
   assert(doc.n_pages == 1 && doc.pages[0].n_lines == 2);
   assert(strcmp(doc.pages[0].lines[0].text, "alpha") == 0);
   assert(strcmp(doc.pages[0].lines[1].text, "beta") == 0);
   kb_pdf_free_doc(&doc);
   PASS("degraded_no_line_tags");
}

/* ---- shim-backed ingest ---- */

static int count_rows(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      return -1;
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = (int)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

static void test_ingest_shim(void)
{
   db2_test_shim_open();

   kb_pdf_ingest_stats_t stats;
   int rc =
       kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "hash1", FIXTURE_2PAGE, "internal", &stats);
   assert(rc == 2); /* two chunks (one per page) */
   assert(stats.chunks == 2);
   assert(stats.regions == 3); /* 2 lines + 1 line */

   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE doc_kind='pdf'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE chunk_strategy='page'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE page_start=1 AND page_end=1") == 1);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE page_start=2 AND page_end=2") == 1);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions") == 3);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions WHERE content_type='text'") == 3);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions WHERE document_key='report.pdf'") == 3);
   /* per-chunk line_index starts at 0; the 2-line chunk has indices 0 and 1 */
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions WHERE line_index=1") == 1);
   /* §6: sensitivity stamped on chunks AND regions; non-restricted -> no quarantine */
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE sensitivity_class='internal'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions WHERE sensitivity_class='internal'") ==
          3);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE quarantine_state=''") == 2);
   /* Phase 1b: PDFs are NOT embedded (no vectors -> invisible to vector-only search). */
   assert(count_rows("SELECT COUNT(*) FROM kb_async_jobs WHERE kind='embed_raw'") == 0);
   /* neighbour threading: exactly one row has a prev pointer (the 2nd chunk) */
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE prev_chunk_id IS NOT NULL") == 1);

   /* Re-ingest the same file: delete-then-insert replaces, regions cascade — no growth. */
   rc = kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "hash2", FIXTURE_2PAGE, "internal", &stats);
   assert(rc == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE doc_kind='pdf'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions") == 3);

   /* Empty extraction for the same file must NOT wipe the prior rows (0-chunk guard). */
   rc = kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "hash3", "<doc></doc>", "internal", &stats);
   assert(rc == 0);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE doc_kind='pdf'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions") == 3);

   /* A `restricted` document is quarantined (pending) and still not embedded. */
   rc = kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h4", FIXTURE_2PAGE, "restricted", &stats);
   assert(rc == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='secret.pdf' AND "
                     "quarantine_state='pending'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='secret.pdf' AND "
                     "sensitivity_class='restricted'") == 2);
   assert(count_rows("SELECT COUNT(*) FROM kb_async_jobs WHERE kind='embed_raw'") == 0);

   /* An invalid/empty sensitivity class is refused at ingest — no rows written. */
   rc = kb_doc_pdf_ingest_xhtml("proj", "bad.pdf", "h5", FIXTURE_2PAGE, "secret", &stats);
   assert(rc < 0);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='bad.pdf'") == 0);
   rc = kb_doc_pdf_ingest_xhtml("proj", "bad2.pdf", "h6", FIXTURE_2PAGE, "", &stats);
   assert(rc < 0);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='bad2.pdf'") == 0);

   /* The validator itself. */
   assert(kb_pdf_sensitivity_valid("public") && kb_pdf_sensitivity_valid("internal") &&
          kb_pdf_sensitivity_valid("restricted"));
   assert(!kb_pdf_sensitivity_valid("") && !kb_pdf_sensitivity_valid("secret") &&
          !kb_pdf_sensitivity_valid(NULL));

   db2_test_shim_close();
   PASS("ingest_shim");
}

/* Phase 2: search_chunks retrieves PDF chunks with line-level citations, withholds
 * restricted/quarantined docs, and the general document fetch reports doc_kind='pdf'
 * (the chokepoint kb_fetch_doc_row uses to keep PDFs out of plain search). */
static void test_search_chunks_shim(void)
{
   db2_test_shim_open();

   kb_pdf_ingest_stats_t stats;
   assert(kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "h1", FIXTURE_2PAGE, "internal", &stats) ==
          2);
   assert(kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h2", FIXTURE_2PAGE, "restricted",
                                  &stats) == 2);

   db2_kb_pdf_chunk_t chunks[16];
   /* "world" appears on page 1 of both docs, but the restricted one is withheld. */
   int n = db2_kb_pdf_search_chunks("proj", "world", 16, chunks, NULL);
   assert(n == 1);
   assert(strcmp(chunks[0].document_key, "report.pdf") == 0);
   assert(strcmp(chunks[0].sensitivity_class, "internal") == 0);
   assert(chunks[0].page_start == 1 && chunks[0].page_end == 1);
   assert(strstr(chunks[0].content, "Hello world") != NULL);

   /* Case-insensitive + line-level citations from kb_doc_regions. */
   db2_kb_pdf_region_t regs[16];
   int rn = db2_kb_doc_regions_for_chunk(chunks[0].chunk_id, regs, 16);
   assert(rn == 2); /* page-1 chunk has 2 lines */
   assert(regs[0].line_index == 0 && regs[0].page_no == 1);
   assert(strcmp(regs[0].quote, "Hello world") == 0);
   assert(regs[0].x0 >= 0 && regs[0].x0 <= 1); /* normalized bbox */

   /* Case-insensitive match. */
   assert(db2_kb_pdf_search_chunks("proj", "HELLO", 16, chunks, NULL) == 1);
   /* A term only on page 2 matches the page-2 chunk, not page 1. */
   n = db2_kb_pdf_search_chunks("proj", "Second page", 16, chunks, NULL);
   assert(n == 1 && chunks[0].page_start == 2);
   /* No match → no results. */
   assert(db2_kb_pdf_search_chunks("proj", "nonexistent-zzz", 16, chunks, NULL) == 0);
   /* LIKE metacharacters in the query are literal, not wildcards. */
   assert(db2_kb_pdf_search_chunks("proj", "%", 16, chunks, NULL) == 0);

   /* The general document fetch now reports doc_kind='pdf' — the value kb_fetch_doc_row
    * uses to exclude PDF chunks from plain /v1/search. */
   db2_kb_pdf_chunk_t one[4];
   assert(db2_kb_pdf_search_chunks("proj", "world", 4, one, NULL) == 1);
   db2_kb_document_row_t row;
   assert(db2_kb_document_fetch(one[0].chunk_id, "proj", &row) == 1);
   assert(strcmp(row.doc_kind, "pdf") == 0);

   /* End-to-end through the /v1/pdf/search route handler (param parse → SQL → JSON). */
   char buf[16384];
   int st = handle_get_pdf_search_route("GET", "query=world&project=proj", buf, sizeof(buf));
   assert(st == 200);
   assert(strstr(buf, "report.pdf") != NULL);
   assert(strstr(buf, "secret.pdf") == NULL); /* restricted withheld */
   assert(strstr(buf, "\"citations\"") != NULL);
   assert(strstr(buf, "\"page_no\":1") != NULL);
   assert(strstr(buf, "\"bbox\"") != NULL);
   assert(handle_get_pdf_search_route("GET", "project=proj", buf, sizeof(buf)) ==
          400);                                                                     /* no query */
   assert(handle_get_pdf_search_route("POST", "query=x", buf, sizeof(buf)) == 405); /* method */

   /* doc_kind exclusion also covers content-reading sweeps outside search: a PDF named like
    * a convention source must NOT be pulled into agent-facing conventions (the same filter
    * the curator extraction queue uses to keep PDF content out of derived artifacts). */
   assert(kb_doc_pdf_ingest_xhtml("proj", "docs/adr/0001.pdf", "ha", FIXTURE_2PAGE, "internal",
                                  &stats) == 2);
   assert(db2_kb_documents_insert_chunk("proj", "docs/adr/0002.md", "hb", 0, "", 0, 0,
                                        "decision content", 2) > 0);
   db2_kb_convention_row_t conv[16];
   int cn = db2_kb_documents_list_convention_candidates(conv, 16);
   int saw_md = 0, saw_pdf = 0;
   for (int i = 0; i < cn; i++)
   {
      if (strstr(conv[i].file_path, ".md"))
         saw_md = 1;
      if (strstr(conv[i].file_path, ".pdf"))
         saw_pdf = 1;
   }
   assert(saw_md && !saw_pdf); /* the .md candidate is surfaced; the .pdf is excluded */

   db2_test_shim_close();
   PASS("search_chunks_shim");
}

/* §6 quarantine admin: a restricted PDF is withheld until confirmed; reject purges it. */
static void test_pdf_quarantine_admin(void)
{
   db2_test_shim_open();
   kb_pdf_ingest_stats_t stats;
   char buf[1024];
   db2_kb_pdf_chunk_t chunks[8];

   /* Restricted -> pending -> withheld from search_chunks. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h1", FIXTURE_2PAGE, "restricted",
                                  &stats) == 2);
   assert(db2_kb_pdf_search_chunks("proj", "world", 8, chunks, NULL) == 0);

   /* confirm -> 200; the doc becomes retrievable. */
   const char *cbody =
       "{\"project\":\"proj\",\"document_key\":\"secret.pdf\",\"action\":\"confirm\"}";
   int st = handle_post_pdf_quarantine_route("POST", cbody, (int)strlen(cbody), buf, sizeof(buf));
   assert(st == 200);
   assert(strstr(buf, "\"chunks\":2") != NULL);
   assert(db2_kb_pdf_search_chunks("proj", "world", 8, chunks, NULL) == 1);
   assert(strcmp(chunks[0].document_key, "secret.pdf") == 0);
   /* confirm again -> 404 (no longer pending). */
   assert(handle_post_pdf_quarantine_route("POST", cbody, (int)strlen(cbody), buf, sizeof(buf)) ==
          404);

   /* reject a fresh restricted doc -> 200; chunks + regions purged (FK cascade). */
   assert(kb_doc_pdf_ingest_xhtml("proj", "bad.pdf", "h2", FIXTURE_2PAGE, "restricted", &stats) ==
          2);
   const char *rbody = "{\"project\":\"proj\",\"document_key\":\"bad.pdf\",\"action\":\"reject\"}";
   assert(handle_post_pdf_quarantine_route("POST", rbody, (int)strlen(rbody), buf, sizeof(buf)) ==
          200);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='bad.pdf'") == 0);
   assert(count_rows("SELECT COUNT(*) FROM kb_doc_regions WHERE document_key='bad.pdf'") == 0);

   /* errors: method, missing fields, unknown action, not-found. */
   assert(handle_post_pdf_quarantine_route("GET", cbody, (int)strlen(cbody), buf, sizeof(buf)) ==
          405);
   assert(handle_post_pdf_quarantine_route("POST", "{}", 2, buf, sizeof(buf)) == 400);
   const char *ubody = "{\"project\":\"proj\",\"document_key\":\"x.pdf\",\"action\":\"foo\"}";
   assert(handle_post_pdf_quarantine_route("POST", ubody, (int)strlen(ubody), buf, sizeof(buf)) ==
          400);
   const char *nbody =
       "{\"project\":\"proj\",\"document_key\":\"missing.pdf\",\"action\":\"confirm\"}";
   assert(handle_post_pdf_quarantine_route("POST", nbody, (int)strlen(nbody), buf, sizeof(buf)) ==
          404);

   /* reject is SCOPED: a non-PDF row that happens to share the file_path must SURVIVE — only
    * the pending PDF chunks are purged, not everything at that (project, file_path). */
   assert(kb_doc_pdf_ingest_xhtml("proj", "shared", "h3", FIXTURE_2PAGE, "restricted", &stats) ==
          2);
   assert(db2_kb_documents_insert_chunk("proj", "shared", "h4", 0, "", 0, 0, "non-pdf body", 2) >
          0);
   const char *sbody = "{\"project\":\"proj\",\"document_key\":\"shared\",\"action\":\"reject\"}";
   assert(handle_post_pdf_quarantine_route("POST", sbody, (int)strlen(sbody), buf, sizeof(buf)) ==
          200);
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='shared' AND "
                     "doc_kind='pdf'") == 0); /* the pending PDF is gone */
   assert(count_rows("SELECT COUNT(*) FROM kb_documents WHERE file_path='shared' AND "
                     "doc_kind=''") == 1); /* the co-located non-PDF row survived */

   db2_test_shim_close();
   PASS("pdf_quarantine_admin");
}

/* §5 evidence escalation: open_page / open_neighbors / inspect_structure, with quarantine
 * withholding on each. */
static void test_pdf_evidence_tools(void)
{
   db2_test_shim_open();
   kb_pdf_ingest_stats_t stats;
   char buf[8192];
   assert(kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "h1", FIXTURE_2PAGE, "internal", &stats) ==
          2);

   /* open_page: page 1 has 2 line-regions, page 2 has 1. */
   assert(handle_get_pdf_page_route("GET", "project=proj&document_key=report.pdf&page_no=1", buf,
                                    sizeof(buf)) == 200);
   assert(strstr(buf, "Hello world") != NULL);
   assert(strstr(buf, "\"total\":2") != NULL);
   assert(strstr(buf, "\"bbox\"") != NULL);
   assert(handle_get_pdf_page_route("GET", "project=proj&document_key=report.pdf&page_no=2", buf,
                                    sizeof(buf)) == 200);
   assert(strstr(buf, "Second page") != NULL && strstr(buf, "\"total\":1") != NULL);

   /* open_neighbors: the page-1 chunk's next is the page-2 chunk. */
   db2_kb_pdf_chunk_t ch[4];
   assert(db2_kb_pdf_search_chunks("proj", "Hello", 4, ch, NULL) == 1);
   char nq[80];
   snprintf(nq, sizeof(nq), "project=proj&chunk_id=%lld", (long long)ch[0].chunk_id);
   assert(handle_get_pdf_neighbors_route("GET", nq, buf, sizeof(buf)) == 200);
   assert(strstr(buf, "Second page") != NULL && strstr(buf, "\"total\":1") != NULL);
   /* cross-scope: the same chunk_id under a DIFFERENT project returns nothing (no IDOR). */
   snprintf(nq, sizeof(nq), "project=other&chunk_id=%lld", (long long)ch[0].chunk_id);
   assert(handle_get_pdf_neighbors_route("GET", nq, buf, sizeof(buf)) == 200);
   assert(strstr(buf, "\"total\":0") != NULL);

   /* inspect_structure: a 2-chunk outline, one per page. */
   assert(handle_get_pdf_structure_route("GET", "project=proj&document_key=report.pdf", buf,
                                         sizeof(buf)) == 200);
   assert(strstr(buf, "\"total\":2") != NULL);
   assert(strstr(buf, "\"page_start\":1") != NULL && strstr(buf, "\"page_start\":2") != NULL);

   /* Quarantine withholding on every tool. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h2", FIXTURE_2PAGE, "restricted",
                                  &stats) == 2);
   assert(handle_get_pdf_page_route("GET", "project=proj&document_key=secret.pdf&page_no=1", buf,
                                    sizeof(buf)) == 200);
   assert(strstr(buf, "\"total\":0") != NULL);
   assert(handle_get_pdf_structure_route("GET", "project=proj&document_key=secret.pdf", buf,
                                         sizeof(buf)) == 200);
   assert(strstr(buf, "\"total\":0") != NULL);
   /* open_neighbors of a restricted chunk withholds its (also-restricted) neighbour. */
   int64_t sid =
       count_rows("SELECT id FROM kb_documents WHERE file_path='secret.pdf' AND chunk_index=0");
   assert(sid > 0);
   snprintf(nq, sizeof(nq), "project=proj&chunk_id=%lld", (long long)sid);
   assert(handle_get_pdf_neighbors_route("GET", nq, buf, sizeof(buf)) == 200);
   assert(strstr(buf, "\"total\":0") != NULL);

   /* error paths. */
   assert(handle_get_pdf_page_route("POST", "project=proj&document_key=x&page_no=1", buf,
                                    sizeof(buf)) == 405);
   assert(handle_get_pdf_page_route("GET", "project=proj", buf, sizeof(buf)) == 400);
   assert(handle_get_pdf_neighbors_route("GET", "", buf, sizeof(buf)) == 400);
   assert(handle_get_pdf_structure_route("GET", "document_key=x", buf, sizeof(buf)) == 400);

   db2_test_shim_close();
   PASS("pdf_evidence_tools");
}

/* Phase A: embed_pdf enqueue gating (the access-control core — pending docs are NOT
 * vector-embedded) + the per-query answerability judgment. Hermetic config via a temp
 * AIMEE_HOME/aimee.yaml that flips kb_pdf_vector_enabled on; the sqlite shim has no
 * halfvec so the vector leg yields no rows (search degrades to lexical) — exactly the
 * embedder-absent degradation path. */
static void write_vector_config(const char *home)
{
   mkdir(home, 0700);
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);
   FILE *fp = fopen(path, "w");
   assert(fp);
   fputs("kb_pdf_vector_enabled: true\n", fp);
   fclose(fp);
}

static void test_pdf_vector_enqueue_and_answerability(void)
{
   char home[] = "/tmp/aimee_pdf_vec_test_XXXXXX";
   assert(mkdtemp(home));
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1); /* bypass the config mtime cache so the yaml is re-read */
   write_vector_config(home);

   db2_test_shim_open();

   kb_pdf_ingest_stats_t stats;
   /* Internal (non-pending) doc → both chunks enqueue an embed_pdf job. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "h1", FIXTURE_2PAGE, "internal", &stats) ==
          2);
   assert(db2_kb_async_count_kind("embed_pdf") == 2);

   /* Restricted (quarantine_state='pending') doc → NO embed_pdf jobs: a withheld document
    * is never vector-embedded (the access-control-relevant A1 invariant). */
   assert(kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h2", FIXTURE_2PAGE, "restricted",
                                  &stats) == 2);
   assert(db2_kb_async_count_kind("embed_pdf") == 2); /* unchanged — pending not enqueued */

   /* Confirming the restricted doc clears quarantine AND enqueues its embed_pdf jobs, so a
    * confirmed doc becomes vector-retrievable. */
   assert(db2_kb_pdf_quarantine_confirm("proj", "secret.pdf") == 2);
   assert(db2_kb_async_count_kind("embed_pdf") == 4);

   /* Answerability (A3): a strong full-coverage query scores HIGH (the reference pin:
    * score >= 0.7); a no-match query scores NONE. Vector leg is inert under the shim, so
    * these assert the deterministic lexical+coverage combiner. */
   db2_kb_pdf_chunk_t chunks[16];
   db2_kb_answerability_t ans;
   /* secret.pdf is confirmed now, so "Hello world" matches BOTH docs' page-1 chunks. */
   int n = db2_kb_pdf_search_chunks("proj", "Hello world", 16, chunks, &ans);
   assert(n == 2);
   assert(ans.score >= 0.7);
   assert(strcmp(ans.label, "HIGH") == 0);
   assert(ans.coverage > 0.99); /* both query terms present */

   db2_kb_pdf_chunk_t none_chunks[4];
   db2_kb_answerability_t ans_none;
   assert(db2_kb_pdf_search_chunks("proj", "nonexistent-zzz", 4, none_chunks, &ans_none) == 0);
   assert(ans_none.score < 0.15);
   assert(strcmp(ans_none.label, "NONE") == 0);

   /* The route surfaces the answerability object as a field DISTINCT from any confidence
    * tier, plus per-chunk has_citation / score. */
   char buf[16384];
   int st = handle_get_pdf_search_route("GET", "query=Hello+world&project=proj", buf, sizeof(buf));
   assert(st == 200);
   assert(strstr(buf, "\"answerability\"") != NULL);
   assert(strstr(buf, "\"label\":\"HIGH\"") != NULL);
   assert(strstr(buf, "\"has_citation\"") != NULL);
   assert(strstr(buf, "\"matched_via\"") != NULL);

   /* search_chunks now requires a project scope (like the other three PDF reads): an empty
    * project is a 400, never an unscoped all-projects search. */
   assert(handle_get_pdf_search_route("GET", "query=world", buf, sizeof(buf)) == 400);

   db2_test_shim_close();
   unsetenv("AIMEE_NO_CACHE");
   unsetenv("AIMEE_HOME");
   PASS("pdf_vector_enqueue_and_answerability");
}

/* ---- Phase B: TSR sidecar client + lookup_table ---- */

static const char *g_tsr_resp = NULL;
static int g_tsr_status = 200;

static int tsr_mock_post(const char *url, const char *auth_header, const char *body,
                         char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf && g_tsr_resp)
      *response_buf = strdup(g_tsr_resp);
   return g_tsr_status;
}

static void test_tsr_sidecar_client(void)
{
   kb_tsr_cell_t *cells = NULL;
   int n = 0;

   /* No endpoint configured → unavailable, agent_http_post is never invoked. */
   assert(kb_tsr_recognize("", 1, "[]", &cells, &n) == -1 && cells == NULL && n == 0);

   mock_agent_http_set_post_handler(tsr_mock_post);

   /* Recognised table → cells parsed (row/col/text/confidence/line_index). */
   g_tsr_status = 200;
   g_tsr_resp = "{\"is_table\":true,\"cells\":["
                "{\"row\":0,\"col\":0,\"text\":\"Q1\",\"confidence\":91,\"line_index\":0},"
                "{\"row\":0,\"col\":1,\"text\":\"42\",\"confidence\":88,\"line_index\":1}]}";
   assert(kb_tsr_recognize("http://tsr.local/recognize", 2, "[]", &cells, &n) == 1);
   assert(n == 2);
   assert(cells[0].row == 0 && cells[0].col == 0 && strcmp(cells[0].text, "Q1") == 0);
   assert(cells[0].confidence == 91 && cells[0].line_index == 0);
   assert(cells[1].col == 1 && strcmp(cells[1].text, "42") == 0);
   kb_tsr_free_cells(cells, n);

   /* Sidecar ran but found no table → 0, no cells. */
   cells = NULL;
   n = 0;
   g_tsr_resp = "{\"is_table\":false}";
   assert(kb_tsr_recognize("http://tsr.local/recognize", 1, "[]", &cells, &n) == 0 && n == 0);

   /* Non-2xx → unavailable. */
   g_tsr_status = 503;
   g_tsr_resp = "upstream down";
   assert(kb_tsr_recognize("http://tsr.local/recognize", 1, "[]", &cells, &n) == -1);

   mock_agent_http_reset();
   g_tsr_resp = NULL;
   g_tsr_status = 200;
   PASS("tsr_sidecar_client");
}

/* First region id for a document_key (cells link to a real region). */
static int64_t first_region_id(const char *document_key)
{
   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT id FROM kb_doc_regions WHERE document_key='%s' ORDER BY id LIMIT 1",
            document_key);
   return count_rows(sql);
}

static void test_pdf_table_cells(void)
{
   db2_test_shim_open();

   kb_pdf_ingest_stats_t stats;
   assert(kb_doc_pdf_ingest_xhtml("proj", "report.pdf", "h1", FIXTURE_2PAGE, "internal", &stats) ==
          2);
   int64_t rid = first_region_id("report.pdf");
   assert(rid > 0);

   /* Simulate a TSR run: insert cells linked to a real region + mark tsr_state='ran'. */
   assert(db2_kb_table_cell_insert(rid, "report.pdf", 1, 0, 0, "Metric", "", "", "", 90,
                                   "internal") > 0);
   assert(db2_kb_table_cell_insert(rid, "report.pdf", 1, 0, 1, "Value", "", "", "", 90,
                                   "internal") > 0);
   assert(db2_kb_table_cell_insert(rid, "report.pdf", 1, 1, 0, "Revenue", "", "", "", 85,
                                   "internal") > 0);
   db2_kb_documents_set_tsr_state("proj", "report.pdf", "ran");

   db2_kb_table_cell_t cells[16];
   int n = db2_kb_table_cells_lookup("proj", "report.pdf", -1, cells, 16);
   assert(n == 3);
   /* Ordered by page, row, col. */
   assert(cells[0].cell_row == 0 && cells[0].cell_col == 0 &&
          strcmp(cells[0].cell_text, "Metric") == 0);
   assert(cells[2].cell_row == 1 && strcmp(cells[2].cell_text, "Revenue") == 0);

   char state[32] = "";
   assert(db2_kb_pdf_tsr_state("proj", "report.pdf", state, sizeof(state)) == 1);
   assert(strcmp(state, "ran") == 0);

   /* Page scope. */
   assert(db2_kb_table_cells_lookup("proj", "report.pdf", 1, cells, 16) == 3);
   assert(db2_kb_table_cells_lookup("proj", "report.pdf", 2, cells, 16) == 0);

   /* ACL: a guessed/foreign document_key returns empty + no readable state. */
   assert(db2_kb_table_cells_lookup("proj", "ghost.pdf", -1, cells, 16) == 0);
   state[0] = '\0';
   assert(db2_kb_pdf_tsr_state("proj", "ghost.pdf", state, sizeof(state)) == 0 && state[0] == '\0');

   /* ACL: a RESTRICTED (pending) doc's cells are withheld even though they exist. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "secret.pdf", "h2", FIXTURE_2PAGE, "restricted",
                                  &stats) == 2);
   int64_t srid = first_region_id("secret.pdf");
   assert(srid > 0);
   assert(db2_kb_table_cell_insert(srid, "secret.pdf", 1, 0, 0, "TopSecret", "", "", "", 90,
                                   "restricted") > 0);
   db2_kb_documents_set_tsr_state("proj", "secret.pdf", "ran");
   assert(db2_kb_table_cells_lookup("proj", "secret.pdf", -1, cells, 16) == 0); /* withheld */
   state[0] = '\0';
   assert(db2_kb_pdf_tsr_state("proj", "secret.pdf", state, sizeof(state)) == 0); /* withheld */

   /* tsr_status='not_a_table' when TSR ran but produced no cells. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "plain.pdf", "h3", FIXTURE_2PAGE, "internal", &stats) ==
          2);
   db2_kb_documents_set_tsr_state("proj", "plain.pdf", "no_table");

   /* Route round-trip: cells + tsr_status surfaced; missing params 400; pending withheld. */
   char buf[16384];
   assert(handle_get_pdf_lookup_table_route("GET", "project=proj&document_key=report.pdf", buf,
                                            sizeof(buf)) == 200);
   assert(strstr(buf, "\"tsr_status\":\"ran\"") != NULL);
   assert(strstr(buf, "\"text\":\"Revenue\"") != NULL);
   assert(handle_get_pdf_lookup_table_route("GET", "project=proj&document_key=plain.pdf", buf,
                                            sizeof(buf)) == 200);
   assert(strstr(buf, "\"tsr_status\":\"not_a_table\"") != NULL);
   assert(handle_get_pdf_lookup_table_route("GET", "project=proj&document_key=secret.pdf", buf,
                                            sizeof(buf)) == 200);
   assert(strstr(buf, "\"tsr_status\":\"unavailable\"") != NULL); /* withheld → no state */
   assert(strstr(buf, "TopSecret") == NULL);
   assert(handle_get_pdf_lookup_table_route("GET", "document_key=report.pdf", buf, sizeof(buf)) ==
          400); /* missing project */
   assert(handle_get_pdf_lookup_table_route("GET", "project=proj", buf, sizeof(buf)) ==
          400); /* missing document_key */

   db2_test_shim_close();
   PASS("pdf_table_cells");
}

/* Mock TSR handler: every page is a 1-cell table at line_index 0. */
static int tsr_ingest_mock(const char *url, const char *auth_header, const char *body,
                           char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = strdup(
          "{\"is_table\":true,\"cells\":[{\"row\":0,\"col\":0,\"text\":\"Cell\",\"confidence\":80,"
          "\"line_index\":0}]}");
   return 200;
}

static void write_tsr_config(const char *home)
{
   mkdir(home, 0700);
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);
   FILE *fp = fopen(path, "w");
   assert(fp);
   fputs("kb_pdf_tsr_enabled: true\ntsr_command: \"http://tsr.local/recognize\"\n", fp);
   fclose(fp);
}

/* End-to-end: ingest with TSR enabled runs the sidecar AFTER commit, persists cells, and
 * a restricted doc's cells are stored-but-withheld until confirm (no re-ingest needed). */
static void test_pdf_tsr_ingest(void)
{
   char home[] = "/tmp/aimee_pdf_tsr_test_XXXXXX";
   assert(mkdtemp(home));
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
   write_tsr_config(home);
   mock_agent_http_set_post_handler(tsr_ingest_mock);

   db2_test_shim_open();

   kb_pdf_ingest_stats_t stats;
   /* 2-page fixture → TSR called per page; each page yields 1 cell at line_index 0. */
   assert(kb_doc_pdf_ingest_xhtml("proj", "rep.pdf", "h1", FIXTURE_2PAGE, "internal", &stats) == 2);
   db2_kb_table_cell_t cells[16];
   int n = db2_kb_table_cells_lookup("proj", "rep.pdf", -1, cells, 16);
   assert(n == 2); /* one cell per page */
   assert(strcmp(cells[0].cell_text, "Cell") == 0 && cells[0].tsr_confidence == 80);
   char state[32] = "";
   assert(db2_kb_pdf_tsr_state("proj", "rep.pdf", state, sizeof(state)) == 1 &&
          strcmp(state, "ran") == 0);

   /* Restricted doc: TSR still runs at ingest (cells stored), but they are WITHHELD until an
    * owner confirms — no re-ingest required (the fix for the post-confirm tsr_status gap). */
   assert(kb_doc_pdf_ingest_xhtml("proj", "sec.pdf", "h2", FIXTURE_2PAGE, "restricted", &stats) ==
          2);
   assert(db2_kb_table_cells_lookup("proj", "sec.pdf", -1, cells, 16) == 0); /* withheld */
   assert(db2_kb_pdf_quarantine_confirm("proj", "sec.pdf") == 2);
   assert(db2_kb_table_cells_lookup("proj", "sec.pdf", -1, cells, 16) == 2); /* now visible */
   state[0] = '\0';
   assert(db2_kb_pdf_tsr_state("proj", "sec.pdf", state, sizeof(state)) == 1 &&
          strcmp(state, "ran") == 0);

   db2_test_shim_close();
   mock_agent_http_reset();
   unsetenv("AIMEE_NO_CACHE");
   unsetenv("AIMEE_HOME");
   PASS("pdf_tsr_ingest");
}

int main(void)
{
   printf("structured-pdf (kb_doc_pdf) tests:\n");
   test_parse();
   test_normalize();
   test_chunk_page_boundary();
   test_chunk_line_cap();
   test_degraded_no_line_tags();
   test_ingest_shim();
   test_search_chunks_shim();
   test_pdf_quarantine_admin();
   test_pdf_evidence_tools();
   test_pdf_vector_enqueue_and_answerability();
   test_tsr_sidecar_client();
   test_pdf_table_cells();
   test_pdf_tsr_ingest();
   printf("ALL PASS\n");
   return 0;
}
