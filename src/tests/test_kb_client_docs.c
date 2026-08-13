#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_client.h"
#include "kb_client_internal.h"
#include "kb_doc_hash.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char *g_last_path;
static char *g_last_body;
static char *g_last_content_type;
static int g_post_calls;
static int g_last_timeout_ms;
static int g_next_status = 200;
static const char *g_next_response = "{\"missing\":[],\"present\":1}";
static const char *g_response_queue[8];
static int g_response_queue_count;
static int g_response_queue_pos;

static const char *next_stub_response(void)
{
   if (g_response_queue_pos < g_response_queue_count)
      return g_response_queue[g_response_queue_pos++];
   return g_next_response;
}

char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   g_last_timeout_ms = timeout_ms;
   free(g_last_path);
   free(g_last_body);
   g_last_path = strdup(path ? path : "");
   g_last_body = body ? cJSON_PrintUnformatted(body) : strdup("{}");
   g_post_calls++;
   if (status_out)
      *status_out = g_next_status;
   if (g_next_status < 200 || g_next_status >= 300)
      return NULL;
   return strdup(next_stub_response());
}

char *kb_client_v1_post_body(const char *path, const char *body, int timeout_ms, int *status_out)
{
   return kb_client_v1_post_body_with_type(path, body, "Content-Type: application/json", timeout_ms,
                                           status_out);
}

char *kb_client_v1_post_body_with_type(const char *path, const char *body, const char *content_type,
                                       int timeout_ms, int *status_out)
{
   g_last_timeout_ms = timeout_ms;
   free(g_last_path);
   free(g_last_body);
   free(g_last_content_type);
   g_last_path = strdup(path ? path : "");
   g_last_body = strdup(body ? body : "");
   g_last_content_type = strdup(content_type ? content_type : "");
   g_post_calls++;
   if (status_out)
      *status_out = g_next_status;
   if (g_next_status < 200 || g_next_status >= 300)
      return NULL;
   return strdup(next_stub_response());
}

static void reset_stub(void)
{
   free(g_last_path);
   free(g_last_body);
   free(g_last_content_type);
   g_last_path = NULL;
   g_last_body = NULL;
   g_last_content_type = NULL;
   g_post_calls = 0;
   g_last_timeout_ms = 0;
   g_next_status = 200;
   g_next_response = "{\"missing\":[],\"present\":1}";
   g_response_queue_count = 0;
   g_response_queue_pos = 0;
   memset(g_response_queue, 0, sizeof(g_response_queue));
}

static void queue_response(const char *response)
{
   assert(g_response_queue_count < (int)(sizeof(g_response_queue) / sizeof(g_response_queue[0])));
   g_response_queue[g_response_queue_count++] = response;
}

static char *write_temp_doc(const char *content)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-kb-client-docs-XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmpl);
   assert(fd >= 0);
   size_t len = strlen(content);
   assert(write(fd, content, len) == (ssize_t)len);
   close(fd);
   return strdup(tmpl);
}

static char *write_temp_doc_bytes(const char *content, size_t len)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-kb-client-docs-XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmpl);
   assert(fd >= 0);
   assert(write(fd, content, len) == (ssize_t)len);
   close(fd);
   return strdup(tmpl);
}

static void test_manifest_posts_hashes(void)
{
   reset_stub();
   const char *content = "# Hello World\n";
   char *path = write_temp_doc(content);
   const char *paths[] = {path};

   char *resp = kb_client_docs_manifest_json("project", paths, 1);
   assert(resp != NULL);
   assert(strcmp(resp, g_next_response) == 0);
   assert(g_post_calls == 1);
   assert(g_last_timeout_ms == 60000);
   assert(strcmp(g_last_path, "/v1/docs/manifest") == 0);

   cJSON *body = cJSON_Parse(g_last_body);
   assert(body != NULL);
   cJSON *scope = cJSON_GetObjectItemCaseSensitive(body, "scope");
   assert(cJSON_IsString(scope) && strcmp(scope->valuestring, "project") == 0);
   cJSON *docs = cJSON_GetObjectItemCaseSensitive(body, "docs");
   assert(cJSON_IsArray(docs) && cJSON_GetArraySize(docs) == 1);
   cJSON *doc = cJSON_GetArrayItem(docs, 0);
   cJSON *doc_key = cJSON_GetObjectItemCaseSensitive(doc, "doc_key");
   assert(cJSON_IsString(doc_key) && strcmp(doc_key->valuestring, path) == 0);
   cJSON *hash = cJSON_GetObjectItemCaseSensitive(doc, "content_hash");
   char expected_hash[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash(content, (int)strlen(content), expected_hash);
   assert(strcmp(expected_hash,
                 "3193a37e30746364372ddb1604d91052647d835206efaaeb2f77ab5e2100bcba") == 0);
   assert(cJSON_IsString(hash) && strcmp(hash->valuestring, expected_hash) == 0);

   cJSON_Delete(body);
   free(resp);
   unlink(path);
   free(path);
}

static void test_manifest_read_error_does_not_post(void)
{
   reset_stub();
   const char *paths[] = {"/tmp/aimee-docs-does-not-exist"};
   char *resp = kb_client_docs_manifest_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(strstr(resp, paths[0]) != NULL);
   assert(g_post_calls == 0);
   free(resp);
}

static void test_manifest_http_error_returns_error(void)
{
   reset_stub();
   char *path = write_temp_doc("hello\n");
   const char *paths[] = {path};
   g_next_status = 503;

   char *resp = kb_client_docs_manifest_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(strstr(resp, "HTTP 503") != NULL);
   assert(g_post_calls == 1);
   assert(g_last_timeout_ms == 60000);
   free(resp);
   unlink(path);
   free(path);
}

static void test_upload_posts_multipart(void)
{
   reset_stub();
   g_next_response = "{\"doc_id\":7,\"state\":\"staged\"}";
   char *path = write_temp_doc("# Hello World\n");
   const char *paths[] = {path};

   char *resp = kb_client_docs_upload_json("project", paths, 1);
   assert(resp != NULL);
   assert(g_post_calls == 1);
   assert(g_last_timeout_ms == 60000);
   assert(strcmp(g_last_path, "/v1/docs") == 0);
   assert(strstr(g_last_content_type, "Content-Type: multipart/form-data; boundary=") != NULL);
   assert(strstr(g_last_body, "Content-Disposition: form-data; name=\"scope\"") != NULL);
   assert(strstr(g_last_body, "\r\nproject\r\n") != NULL);
   assert(strstr(g_last_body, "Content-Disposition: form-data; name=\"file\"; filename=\"") !=
          NULL);
   assert(strstr(g_last_body, path) != NULL);
   assert(strstr(g_last_body, "# Hello World\n") != NULL);
   assert(strstr(g_last_body, "\r\n------AimeeKbDocs") != NULL);
   const char *boundary = strstr(g_last_content_type, "boundary=");
   assert(boundary != NULL);
   boundary += strlen("boundary=");
   assert(strstr(g_last_body, boundary) != NULL);

   cJSON *body = cJSON_Parse(resp);
   assert(body != NULL);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(body, "status");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   cJSON *uploaded = cJSON_GetObjectItemCaseSensitive(body, "uploaded");
   assert(cJSON_IsNumber(uploaded) && uploaded->valueint == 1);
   cJSON *docs = cJSON_GetObjectItemCaseSensitive(body, "docs");
   assert(cJSON_IsArray(docs) && cJSON_GetArraySize(docs) == 1);
   cJSON *doc = cJSON_GetArrayItem(docs, 0);
   cJSON *response = cJSON_GetObjectItemCaseSensitive(doc, "response");
   cJSON *doc_id = cJSON_GetObjectItemCaseSensitive(response, "doc_id");
   assert(cJSON_IsNumber(doc_id) && doc_id->valueint == 7);

   cJSON_Delete(body);
   free(resp);
   unlink(path);
   free(path);
}

static void test_upload_read_error_does_not_post(void)
{
   reset_stub();
   const char *paths[] = {"/tmp/aimee-docs-does-not-exist"};
   char *resp = kb_client_docs_upload_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(strstr(resp, "\"failed\":1") != NULL);
   assert(g_post_calls == 0);
   free(resp);
}

static void test_upload_null_scope_defaults_global(void)
{
   reset_stub();
   g_next_response = "{\"doc_id\":8,\"state\":\"staged\"}";
   char *path = write_temp_doc("hello\n");
   const char *paths[] = {path};

   char *resp = kb_client_docs_upload_json(NULL, paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"ok\"") != NULL);
   assert(g_post_calls == 1);
   assert(strstr(g_last_body, "\r\nglobal\r\n") != NULL);
   free(resp);
   unlink(path);
   free(path);
}

static void test_upload_http_error_reports_failure(void)
{
   reset_stub();
   char *path = write_temp_doc("hello\n");
   const char *paths[] = {path};
   g_next_status = 503;

   char *resp = kb_client_docs_upload_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(strstr(resp, "\"http_status\":503") != NULL);
   assert(g_post_calls == 1);
   free(resp);
   unlink(path);
   free(path);
}

static void test_upload_rejects_invalid_path_header_chars(void)
{
   reset_stub();
   const char *paths[] = {"/tmp/aimee-docs-quote\"bad.md"};
   char *resp = kb_client_docs_upload_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(g_post_calls == 0);
   free(resp);
}

static void test_upload_embedded_nul_does_not_post(void)
{
   reset_stub();
   const char bytes[] = {'a', '\0', 'b'};
   char *path = write_temp_doc_bytes(bytes, sizeof(bytes));
   const char *paths[] = {path};

   char *resp = kb_client_docs_upload_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(g_post_calls == 0);
   free(resp);
   unlink(path);
   free(path);
}

static void test_push_skips_when_manifest_present(void)
{
   reset_stub();
   char *path1 = write_temp_doc("one\n");
   char *path2 = write_temp_doc("two\n");
   const char *paths[] = {path1, path2};
   queue_response("{\"missing\":[],\"present\":2,\"total\":2,\"missing_count\":0}");

   char *resp = kb_client_docs_push_json("project", paths, 2);
   assert(resp != NULL);
   assert(g_post_calls == 1);
   assert(strcmp(g_last_path, "/v1/docs/manifest") == 0);

   cJSON *body = cJSON_Parse(resp);
   assert(body != NULL);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(body, "status");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(body, "uploaded")->valueint == 0);
   assert(cJSON_GetObjectItemCaseSensitive(body, "skipped")->valueint == 2);

   cJSON_Delete(body);
   free(resp);
   unlink(path1);
   unlink(path2);
   free(path1);
   free(path2);
}

static void test_push_uploads_only_manifest_missing(void)
{
   reset_stub();
   char *path1 = write_temp_doc("one\n");
   char *path2 = write_temp_doc("two\n");
   const char *paths[] = {path1, path2};

   char manifest[1024];
   snprintf(manifest, sizeof(manifest),
            "{\"missing\":[{\"doc_key\":\"%s\",\"content_hash\":\"abc\",\"scope\":\"project\"}],"
            "\"present\":1,\"total\":2,\"missing_count\":1}",
            path2);
   queue_response(manifest);
   queue_response("{\"doc_id\":9,\"state\":\"staged\"}");

   char *resp = kb_client_docs_push_json("project", paths, 2);
   assert(resp != NULL);
   assert(g_post_calls == 2);
   assert(strcmp(g_last_path, "/v1/docs") == 0);
   assert(strstr(g_last_body, path2) != NULL);
   assert(strstr(g_last_body, path1) == NULL);

   cJSON *body = cJSON_Parse(resp);
   assert(body != NULL);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(body, "status");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(body, "uploaded")->valueint == 1);
   assert(cJSON_GetObjectItemCaseSensitive(body, "skipped")->valueint == 1);

   cJSON_Delete(body);
   free(resp);
   unlink(path1);
   unlink(path2);
   free(path1);
   free(path2);
}

static void test_push_rejects_unknown_manifest_path(void)
{
   reset_stub();
   char *path = write_temp_doc("one\n");
   const char *paths[] = {path};
   queue_response("{\"missing\":[{\"doc_key\":\"/tmp/not-requested.md\","
                  "\"content_hash\":\"abc\",\"scope\":\"project\"}],\"present\":0}");

   char *resp = kb_client_docs_push_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(g_post_calls == 1);
   assert(strcmp(g_last_path, "/v1/docs/manifest") == 0);

   free(resp);
   unlink(path);
   free(path);
}

static void test_push_preserves_manifest_error(void)
{
   reset_stub();
   char *path = write_temp_doc("one\n");
   const char *paths[] = {path};
   queue_response("{\"status\":\"error\",\"message\":\"database queue unavailable\"}");

   char *resp = kb_client_docs_push_json("project", paths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "database queue unavailable") != NULL);
   assert(strstr(resp, "missing array") == NULL);
   free(resp);
   unlink(path);
   free(path);

   reset_stub();
   const char *keys[] = {"/client/guide.md"};
   const char *contents[] = {"remote bytes\n"};
   int lengths[] = {(int)strlen(contents[0])};
   queue_response("{\"error\":{\"message\":\"manifest service overloaded\"}}");
   resp = kb_client_docs_push_content_json("project", keys, contents, lengths, 1);
   assert(resp != NULL);
   assert(strstr(resp, "manifest service overloaded") != NULL);
   assert(strstr(resp, "missing array") == NULL);
   free(resp);
}

static void test_push_content_uploads_client_bytes(void)
{
   reset_stub();
   const char *keys[] = {"/client/guide.md"};
   const char *contents[] = {"# Remote guide\nThe heliotrope code is 7319.\n"};
   int lengths[] = {(int)strlen(contents[0])};
   queue_response("{\"missing\":[{\"doc_key\":\"/client/guide.md\","
                  "\"content_hash\":\"abc\",\"scope\":\"project\"}],"
                  "\"present\":0,\"total\":1,\"missing_count\":1}");
   queue_response("{\"doc_id\":17,\"state\":\"staged\"}");

   char *resp = kb_client_docs_push_content_json("project", keys, contents, lengths, 1);
   assert(resp != NULL);
   assert(g_post_calls == 2);
   assert(strcmp(g_last_path, "/v1/docs") == 0);
   assert(strstr(g_last_body, "filename=\"/client/guide.md\"") != NULL);
   assert(strstr(g_last_body, "heliotrope code is 7319") != NULL);
   assert(strstr(resp, "\"status\":\"ok\"") != NULL);
   assert(strstr(resp, "\"uploaded\":1") != NULL);
   free(resp);
}

int main(void)
{
   test_manifest_posts_hashes();
   test_manifest_read_error_does_not_post();
   test_manifest_http_error_returns_error();
   test_upload_posts_multipart();
   test_upload_read_error_does_not_post();
   test_upload_null_scope_defaults_global();
   test_upload_http_error_reports_failure();
   test_upload_rejects_invalid_path_header_chars();
   test_upload_embedded_nul_does_not_post();
   test_push_skips_when_manifest_present();
   test_push_uploads_only_manifest_missing();
   test_push_rejects_unknown_manifest_path();
   test_push_preserves_manifest_error();
   test_push_content_uploads_client_bytes();
   reset_stub();
   printf("kb_client_docs: ok\n");
   return 0;
}
