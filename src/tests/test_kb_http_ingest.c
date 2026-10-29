/* test_kb_http_ingest.c: unit tests for kb_http_ingest handlers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kb_doc_hash.h"
#include "kb_document_inspector.h"
#include "kb_http_ingest.h"
#include "config.h"
#include "kb_doc_pdf.h"
#include "support/config_test_setters.h"

/* ── Stubs for kb_store/kb_docs.h symbols ───────────────────────────────────────
 * The real kb_store_kb_doc_t layout must match what handle_get_doc reads,
 * so we reproduce it here verbatim rather than using void *. */

typedef struct
{
   int64_t id;
   char content_hash[65];
   char filename[256];
   char scope[64];
   char converter[64];
   char converter_version[128];
   char state[32];
   int review_needed;
   char review_reason[256];
   char created_at[32];
   char updated_at[32];
} stub_doc_t;

static char g_last_content_hash[KB_DOC_HASH_HEX_LEN + 1];
static kb_document_disposition_t g_inspection_disposition = KB_DOCUMENT_CLEAN;

int kb_document_inspect(const char *filename, const char *bytes, int nbytes,
                        kb_document_channel_report_t *report)
{
   memset(report, 0, sizeof(*report));
   snprintf(report->format, sizeof(report->format), "%s",
            strstr(filename, ".html") ? "html" : "plain");
   kb_doc_content_hash(bytes, nbytes, report->raw_digest);
   report->disposition = g_inspection_disposition;
   if (g_inspection_disposition == KB_DOCUMENT_REVIEW ||
       g_inspection_disposition == KB_DOCUMENT_REJECT)
   {
      report->hidden_spans = 1;
      snprintf(report->first_hidden_digest, sizeof(report->first_hidden_digest), "%064d", 1);
      snprintf(report->lexical_verdict, sizeof(report->lexical_verdict), "%s",
               g_inspection_disposition == KB_DOCUMENT_REJECT ? "reject" : "accept");
   }
   return 0;
}

const char *kb_document_disposition_name(kb_document_disposition_t disposition)
{
   switch (disposition)
   {
   case KB_DOCUMENT_REVIEW:
      return "review";
   case KB_DOCUMENT_REJECT:
      return "reject";
   case KB_DOCUMENT_UNSUPPORTED:
      return "unsupported";
   case KB_DOCUMENT_RESOURCE_LIMIT:
      return "resource_limit";
   case KB_DOCUMENT_INVALID:
      return "invalid";
   default:
      return "clean";
   }
}

int64_t kb_store_kb_doc_write(const char *content_hash, const char *filename, const char *scope,
                              const char *converter, const char *converter_version,
                              const char *normalized_text, int *was_existing)
{
   (void)filename;
   (void)scope;
   (void)converter;
   (void)converter_version;
   (void)normalized_text;
   snprintf(g_last_content_hash, sizeof(g_last_content_hash), "%s",
            content_hash ? content_hash : "");
   if (was_existing)
      *was_existing = 0;
   return 42; /* fake doc id */
}

int kb_store_kb_doc_exists_by_hash_scope(const char *content_hash, const char *scope)
{
   if (!content_hash || !scope)
      return -1;
   if (strcmp(content_hash, "present-hash") == 0 && strcmp(scope, "project") == 0)
      return 1;
   if (strcmp(content_hash, "db-error") == 0)
      return -1;
   return 0;
}

int kb_store_kb_doc_read(int64_t id, stub_doc_t *out)
{
   if (id != 42)
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = 42;
   snprintf(out->filename, sizeof(out->filename), "test.md");
   snprintf(out->content_hash, sizeof(out->content_hash), "abc123");
   snprintf(out->scope, sizeof(out->scope), "global");
   snprintf(out->converter, sizeof(out->converter), "passthrough");
   snprintf(out->state, sizeof(out->state), "staged");
   snprintf(out->created_at, sizeof(out->created_at), "2026-01-01 00:00:00");
   return 0;
}

int kb_store_kb_doc_set_state(int64_t id, const char *state, int clear, const char *reason)
{
   (void)id;
   (void)state;
   (void)clear;
   (void)reason;
   return 0;
}

int kb_store_kb_doc_list_review(int limit, int64_t cursor_id, stub_doc_t *out, int max_out)
{
   (void)limit;
   (void)cursor_id;
   (void)out;
   (void)max_out;
   return 0;
}

/* ── Stubs for kb_ingest_normalize.h symbols ───────────────────────────── */

int kb_ingest_normalize(const char *filename, const char *bytes, int nbytes, char *out_buf,
                        int out_cap, char *converter_out, int conv_cap, char *converter_version_out,
                        int ver_cap)
{
   (void)filename;
   (void)bytes;
   (void)nbytes;
   if (out_buf && out_cap > 0)
      snprintf(out_buf, (size_t)out_cap, "# Normalized\n");
   if (converter_out && conv_cap > 0)
      snprintf(converter_out, (size_t)conv_cap, "passthrough");
   if (converter_version_out && ver_cap > 0)
      converter_version_out[0] = '\0';
   return 0;
}

const char *kb_ingest_detect_format(const char *filename)
{
   (void)filename;
   return "passthrough";
}

/* ── Stubs for the structured-PDF path (config + kb_doc_pdf) ─────────────── */

int kb_doc_pdf_render_assets(const char *project, const char *file_path,
                             const char *sensitivity_class, const unsigned char *pdf_bytes, int n)
{
   (void)project;
   (void)file_path;
   (void)sensitivity_class;
   (void)pdf_bytes;
   (void)n;
   return 0;
}
int kb_pdf_sensitivity_valid(const char *s)
{
   return s &&
          (strcmp(s, "public") == 0 || strcmp(s, "internal") == 0 || strcmp(s, "restricted") == 0);
}
static int g_exec_called = 0;
int kb_pdf_exec_bbox_layout(const unsigned char *bytes, int n, char *out, int out_cap,
                            int timeout_ms)
{
   (void)bytes;
   (void)n;
   (void)timeout_ms;
   g_exec_called++;
   snprintf(out, (size_t)out_cap,
            "<doc><page width=\"600\" height=\"800\"><line xMin=\"1\" yMin=\"1\" xMax=\"2\" "
            "yMax=\"2\"><word xMin=\"1\" yMin=\"1\" xMax=\"2\" yMax=\"2\">hi</word></line>"
            "</page></doc>");
   return 0;
}
/* OCR is disabled in this handler suite. Keep the new production dependency at
 * the same explicit test-double boundary as XHTML ingestion, and fail if routing
 * ever calls it instead of silently reporting a fabricated OCR success. */
int kb_doc_pdf_ingest_ocr(const char *project, const char *file_path, const char *file_hash,
                          const char *sensitivity_class, const unsigned char *pdf_bytes, int n,
                          const char *ocr_endpoint, kb_pdf_ingest_stats_t *stats)
{
   (void)project;
   (void)file_path;
   (void)file_hash;
   (void)sensitivity_class;
   (void)pdf_bytes;
   (void)n;
   (void)ocr_endpoint;
   (void)stats;
   assert(!"OCR must not be invoked while disabled in the HTTP ingestion suite");
   return -1;
}
static int g_ingest_called = 0;
static char g_ingest_project[64], g_ingest_fp[256], g_ingest_hash[80], g_ingest_class[32];
int kb_doc_pdf_ingest_xhtml(const char *project, const char *file_path, const char *file_hash,
                            const char *xhtml, const char *sensitivity_class,
                            kb_pdf_ingest_stats_t *stats)
{
   (void)xhtml;
   g_ingest_called++;
   snprintf(g_ingest_project, sizeof(g_ingest_project), "%s", project ? project : "");
   snprintf(g_ingest_fp, sizeof(g_ingest_fp), "%s", file_path ? file_path : "");
   snprintf(g_ingest_hash, sizeof(g_ingest_hash), "%s", file_hash ? file_hash : "");
   snprintf(g_ingest_class, sizeof(g_ingest_class), "%s",
            sensitivity_class ? sensitivity_class : "");
   if (stats)
   {
      stats->chunks = 2;
      stats->regions = 3;
   }
   return 2;
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

static void test_post_docs_ok(void)
{
   const char *boundary = "----TestBoundary";
   const char *file_content = "# Hello World\r\n";
   char expected_hash[KB_DOC_HASH_HEX_LEN + 1];
   char body[512];
   kb_doc_content_hash(file_content, (int)strlen(file_content), expected_hash);
   g_last_content_hash[0] = '\0';
   int body_len = snprintf(body, sizeof(body),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"test.md\"\r\n"
                           "Content-Type: text/markdown\r\n"
                           "\r\n"
                           "# Hello World\r\n"
                           "\r\n--%s--\r\n",
                           boundary, boundary);
   char buf[256];
   int status = handle_post_docs(body, body_len, buf, sizeof(buf));
   assert(status == 201);
   assert(strstr(buf, "doc_id") != NULL);
   assert(strcmp(g_last_content_hash, expected_hash) == 0);
}

static void test_structural_disposition_stops_before_write(void)
{
   const char *boundary = "----InspectBoundary";
   char body[512], buf[1024];
   int body_len =
       snprintf(body, sizeof(body),
                "--%s\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"test.html\"\r\n"
                "Content-Type: text/html\r\n\r\n"
                "<div hidden>draft</div>\r\n"
                "--%s--\r\n",
                boundary, boundary);
   g_last_content_hash[0] = '\0';
   setenv("AIMEE_KB_DOCUMENT_INSPECTION", "1", 1);
   g_inspection_disposition = KB_DOCUMENT_REVIEW;
   int status = handle_post_docs(body, body_len, buf, sizeof(buf));
   assert(status == 422);
   assert(strstr(buf, "document_review") != NULL);
   assert(g_last_content_hash[0] == '\0');

   g_inspection_disposition = KB_DOCUMENT_UNSUPPORTED;
   status = handle_post_docs(body, body_len, buf, sizeof(buf));
   assert(status == 415);
   assert(g_last_content_hash[0] == '\0');
   g_inspection_disposition = KB_DOCUMENT_CLEAN;
   unsetenv("AIMEE_KB_DOCUMENT_INSPECTION");
}

static void test_post_docs_missing_file(void)
{
   char buf[256];
   int status = handle_post_docs(NULL, 0, buf, sizeof(buf));
   assert(status == 400);
}

static void test_post_docs_manifest_filters_present_hashes(void)
{
   const char *body = "{\"scope\":\"project\",\"docs\":["
                      "{\"doc_key\":\"a.md\",\"content_hash\":\"present-hash\"},"
                      "{\"doc_key\":\"b.md\",\"content_hash\":\"missing-hash\"}]}";
   char buf[512];
   int status = handle_post_docs_manifest(body, (int)strlen(body), buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"present\":1") != NULL);
   assert(strstr(buf, "\"missing_count\":1") != NULL);
   assert(strstr(buf, "\"doc_key\":\"b.md\"") != NULL);
   assert(strstr(buf, "a.md") == NULL);
}

static void test_post_docs_manifest_requires_docs_array(void)
{
   char buf[256];
   const char *body = "{\"docs\":{}}";
   int status = handle_post_docs_manifest(body, (int)strlen(body), buf, sizeof(buf));
   assert(status == 400);
}

static void test_post_docs_manifest_db_error(void)
{
   const char *body = "{\"docs\":[{\"doc_key\":\"bad.md\",\"content_hash\":\"db-error\"}]}";
   char buf[256];
   int status = handle_post_docs_manifest(body, (int)strlen(body), buf, sizeof(buf));
   assert(status == 503);
}

static void test_get_doc_ok(void)
{
   char buf[512];
   int status = handle_get_doc("42", buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "filename") != NULL);
}

static void test_get_doc_not_found(void)
{
   char buf[256];
   int status = handle_get_doc("999", buf, sizeof(buf));
   assert(status == 404);
}

static void test_delete_doc_requires_lifecycle_preview(void)
{
   char buf[256];
   int status = handle_delete_doc("42", buf, sizeof(buf));
   assert(status == 409);
   assert(strstr(buf, "document.preview_lifecycle") != NULL);
   assert(strstr(buf, "purge") != NULL);
}

static void test_delete_doc_not_found(void)
{
   char buf[256];
   int status = handle_delete_doc("999", buf, sizeof(buf));
   assert(status == 404);
}

static void test_get_review_ok(void)
{
   char buf[512];
   int status = handle_get_review(NULL, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "docs") != NULL);
}

int main(void)
{
   printf("kb_http_ingest: ");
   assert(config_set_kb_pdf_ocr_enabled(0) == 0);
   assert(config_set_kb_pdf_assets_enabled(0) == 0);
   unsetenv("AIMEE_KB_DOCUMENT_INSPECTION");

   test_post_docs_ok();
   test_structural_disposition_stops_before_write();
   test_post_docs_missing_file();
   test_post_docs_manifest_filters_present_hashes();
   test_post_docs_manifest_requires_docs_array();
   test_post_docs_manifest_db_error();
   test_get_doc_ok();
   test_get_doc_not_found();
   test_delete_doc_requires_lifecycle_preview();
   test_delete_doc_not_found();
   test_get_review_ok();

   printf("ok\n");
   return 0;
}
