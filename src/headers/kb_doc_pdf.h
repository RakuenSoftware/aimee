/* kb_doc_pdf.h: structured-PDF ingestion, Phase 1 (first increment).
 *
 * Turns poppler `pdftotext -bbox-layout` XHTML into the existing kb_documents chunk
 * spine PLUS per-line coordinate geometry in kb_doc_regions. The whole pipeline below
 * starts from the XHTML STRING (the bbox-layout output), so it is pure and unit-testable
 * with no subprocess: parse -> normalize -> chunk -> ingest. The actual `pdftotext`
 * exec wrapper (with its subprocess timeout + temp-file hardening) and the upload-route
 * wiring land in the next increment; this one has NO live call site and is exercised by
 * its unit tests only — so nothing is exposed regardless of the kb_pdf_ingest_enabled flag.
 *
 * License note: poppler (the producer of the XHTML this parses) is run as a SEPARATE
 * PROCESS by the operator's deploy; aimee never links or bundles it. This module only
 * parses the text output across that process boundary.
 *
 * Chunking (Phase 1): deterministic PAGE-BOUNDARY chunking with a line-count cap — no
 * font/heading heuristics (those are flaky across PDF producers and deferred). Every
 * chunk therefore lies on a single page (page_start == page_end); the two columns exist
 * so heading-spanning chunks can cross pages in a later increment. No text is ever
 * dropped: every extracted line lands in exactly one chunk and one region row.
 */
#ifndef DEC_KB_DOC_PDF_H
#define DEC_KB_DOC_PDF_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* A single extracted text line with its bounding box. After kb_pdf_normalize the bbox
    * is in [0,1], top-left origin, normalized per page; before it, the raw pdftotext
    * point coordinates. `text` is malloc'd (owned by the kb_pdf_doc_t). */
   typedef struct
   {
      int page_no; /* 1-based */
      double x0, y0, x1, y1;
      char *text;
   } kb_pdf_line_t;

   typedef struct
   {
      double width, height; /* page box in points, from the <page> element */
      kb_pdf_line_t *lines;
      int n_lines, cap_lines;
   } kb_pdf_page_t;

   typedef struct
   {
      kb_pdf_page_t *pages;
      int n_pages, cap_pages;
      int normalized; /* 1 once kb_pdf_normalize has run */
   } kb_pdf_doc_t;

   /* One chunk + the lines that compose it (borrowed pointers into the kb_pdf_doc_t, so a
    * chunk array must not outlive its doc). content is malloc'd (owned by the chunk). */
   typedef struct
   {
      char *content;               /* lines joined by '\n' */
      const kb_pdf_line_t **lines; /* malloc'd array of borrowed line pointers */
      int n_lines;
      int line_start, line_end; /* 0-based global line ordinals across the doc */
      int page_start, page_end; /* MIN/MAX page_no over the chunk's lines */
      int token_count;
   } kb_pdf_chunk_t;

   /* Parse `pdftotext -bbox-layout` XHTML into pages/lines (raw point coords). Tolerant of
    * the surrounding <html>/<doc> wrapper and of pages that carry bare <word>s without
    * <line> wrappers (each such word becomes its own line, so no text is lost). Returns 0
    * on success (out fully owned by the caller; free with kb_pdf_free_doc), <0 on a hard
    * error. An input with no pages yields a valid empty doc (0 pages), returning 0. */
   int kb_pdf_parse_bbox_layout(const char *xhtml, kb_pdf_doc_t *out);

   /* Normalize every line's bbox to [0,1] (x/width, y/height), top-left origin, clamped.
    * Idempotent guard via doc->normalized. A page with non-positive width/height leaves its
    * lines clamped to [0,0,0,0] rather than dividing by zero. */
   void kb_pdf_normalize(kb_pdf_doc_t *doc);

   /* Page-boundary chunking with a line cap. Allocates *chunks (free with
    * kb_pdf_free_chunks). Returns the chunk count (>=0) or <0 on error. */
   int kb_pdf_chunk(const kb_pdf_doc_t *doc, kb_pdf_chunk_t **chunks, int *n_chunks);

   void kb_pdf_free_doc(kb_pdf_doc_t *doc);
   void kb_pdf_free_chunks(kb_pdf_chunk_t *chunks, int n_chunks);

   /* Result counters from an ingest. */
   typedef struct
   {
      int chunks;
      int regions;
   } kb_pdf_ingest_stats_t;

   /* True iff `s` is one of the §6 sensitivity classes (public|internal|restricted). The
    * upload surface rejects anything else (incl. empty/NULL) before ingest. */
   int kb_pdf_sensitivity_valid(const char *s);

   /* Run `pdftotext -bbox-layout` over `bytes` in a SEPARATE PROCESS (operator-installed;
    * aimee never links/bundles poppler), capturing its XHTML into `out` (NUL-terminated,
    * capped at `out_cap`). Hardened: bytes go to a 0600 mkstemp temp file (unlinked on every
    * exit path); the child gets RLIMIT_CPU/RLIMIT_AS/RLIMIT_FSIZE caps; stdout is read with a
    * wall-clock deadline (`timeout_ms`) and a byte cap (`out_cap` — output beyond it aborts);
    * on deadline the child is SIGKILLed and reaped. Returns 0 on success, <0 on
    * timeout/over-cap/exec/non-zero-exit (the caller treats any <0 as a parse failure). */
   int kb_pdf_exec_bbox_layout(const unsigned char *bytes, int n, char *out, int out_cap,
                               int timeout_ms);

   /* Ingest an already-parsed-and-normalized doc into kb_documents + kb_doc_regions under
    * (project, file_path, file_hash), stamping `sensitivity_class` on every chunk + region.
    * A `restricted` document is marked `quarantine_state='pending'`. Replaces any existing
    * rows for (project, file_path) first (db2_kb_documents_delete_for_file; regions cascade),
    * all in one transaction; the 0-chunk case writes nothing (never wipes prior rows).
    * Phase 1b does NOT enqueue embeddings for PDF chunks — embedding + access-controlled
    * retrieval land together in Phase 2, so PDF content is never vector-searchable before its
    * access control exists. Returns the chunk count (>=0) or <0 on error. */
   int kb_doc_pdf_ingest(const char *project, const char *file_path, const char *file_hash,
                         const kb_pdf_doc_t *doc, const char *sensitivity_class,
                         kb_pdf_ingest_stats_t *stats);

   /* Convenience: parse -> normalize -> ingest from raw bbox-layout XHTML. */
   int kb_doc_pdf_ingest_xhtml(const char *project, const char *file_path, const char *file_hash,
                               const char *xhtml, const char *sensitivity_class,
                               kb_pdf_ingest_stats_t *stats);

   /* Phase C: render each page of a PDF (raw bytes) to a PNG crop via the hardened pdftoppm
    * harness, store it in the content-addressed blob store, and insert a kb_doc_assets row
    * (kind='page'). Best-effort: a missing pdftoppm or unrenderable page yields fewer/no
    * assets, never an error. Returns the number of asset rows created. Caller gates on
    * kb_pdf_assets_enabled + the doc's sensitivity. */
   int kb_doc_pdf_render_assets(const char *project, const char *file_path,
                                const char *sensitivity_class, const unsigned char *pdf_bytes,
                                int n);

   /* Phase D: OCR a scanned/no-text-layer PDF via the OCR sidecar and ingest the recognised
    * text + per-line geometry through the normal kb_doc_pdf_ingest path (citations work
    * identically). Returns the ingested chunk count (>0), 0 if no text was recognised on any
    * page (caller falls back to asset-only), or -1 on a DB error. */
   int kb_doc_pdf_ingest_ocr(const char *project, const char *file_path, const char *file_hash,
                             const char *sensitivity_class, const unsigned char *pdf_bytes, int n,
                             const char *ocr_endpoint, kb_pdf_ingest_stats_t *stats);

/* The page-boundary chunk line cap (exposed for tests). */
#define KB_PDF_MAX_CHUNK_LINES 100

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_DOC_PDF_H */
