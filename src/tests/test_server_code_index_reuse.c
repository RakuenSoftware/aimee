/* test_server_code_index_reuse.c: a staged code-scan batch reuses the vectors
 * the store already holds for unchanged files and embeds only the rest.
 *
 * Embedding is the slow step of a scan (seconds per file on a CPU embedder),
 * and on the live Server every scan embedded every file again although the
 * store held vectors for nearly all of them. This drives the real batch path
 * (local_code -> known_embeddings -> extract_file -> store) against a fake
 * store and embedder, and checks the store's refusal handling and status. */
#include "cJSON.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../server/server_code_index.c"

/* ---- fake store ---------------------------------------------------------- */
static int g_lookups, g_stages, g_known_reply = 1, g_refuse_stage;
static int g_embed_calls, g_serving_available = 1;
static char g_lookup_serving[64];
static int g_lookup_files;
static int g_staged_with_reuse, g_staged_embedded;

char *db1_runtime_code_index(const char *request_json)
{
   cJSON *req = cJSON_Parse(request_json);
   assert(req);
   const char *route = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "route"));
   char *out = NULL;
   if (!strcmp(route, "/v1/code/embeddings"))
   {
      g_lookups++;
      snprintf(g_lookup_serving, sizeof g_lookup_serving, "%s",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "embedding_serving")));
      cJSON *files = cJSON_GetObjectItemCaseSensitive(req, "files");
      g_lookup_files = cJSON_GetArraySize(files);
      const cJSON *f;
      cJSON_ArrayForEach(f, files)
      {
         /* The lookup carries a 64-hex content fingerprint, never the content. */
         assert(strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f, "fingerprint"))) ==
                64);
         assert(!cJSON_GetObjectItemCaseSensitive(f, "content"));
      }
      out = strdup(g_known_reply ? "{\"embeddings\":{\"unchanged.c\":[0.5,0.25]}}"
                                 : "{\"error\":\"store unavailable\"}");
   }
   else
   {
      assert(!strcmp(route, "/v1/code/scan"));
      g_stages++;
      const cJSON *file = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(req, "files"), 0);
      const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, "rel_path"));
      const char *serving =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, "embedding_serving"));
      const cJSON *vector = cJSON_GetObjectItemCaseSensitive(file, "embedding");
      /* Only when the store offered a vector (and an identity was known) is the
       * unchanged file expected to arrive with it; otherwise it is embedded. */
      if (path && !strcmp(path, "unchanged.c") && g_known_reply && g_serving_available)
      {
         /* Reused: the stored vector under the current serving identity and the
          * policy the store requires. */
         assert(serving && !strcmp(serving, "serving-1"));
         assert(cJSON_GetArraySize(vector) == 2 &&
                cJSON_GetArrayItem(vector, 0)->valuedouble == 0.5);
         assert(!strcmp(
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, "embedding_policy")),
             "runtime-code-path-prefix8192-bytes-cosine/v1"));
         g_staged_with_reuse++;
      }
      else
         g_staged_embedded += cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "embedded_here"));
      out = strdup(g_refuse_stage && g_stages == 1
                       ? "{\"status\":\"error\",\"code\":\"scan_superseded\",\"error\":\"taken "
                         "over\"}"
                       : "{\"status\":\"ok\",\"files\":1}");
   }
   cJSON_Delete(req);
   return out;
}

/* ---- fake embedder --------------------------------------------------------- */
char *server_code_index_serving_id(void)
{
   return g_serving_available ? strdup("serving-1") : NULL;
}
void server_code_index_embed_file(const char *path, const char *content, cJSON *file)
{
   (void)path;
   (void)content;
   g_embed_calls++;
   cJSON_AddBoolToObject(file, "embedded_here", 1);
}
void server_code_index_reuse_embedding(cJSON *file, const cJSON *vector, const char *serving)
{
   cJSON_AddItemToObject(file, "embedding", cJSON_Duplicate(vector, 1));
   cJSON_AddStringToObject(file, "embedding_serving", serving);
   cJSON_AddStringToObject(file, "embedding_policy",
                           "runtime-code-path-prefix8192-bytes-cosine/v1");
}
void server_code_index_embed_query(const char *route, cJSON *request)
{
   (void)route;
   (void)request;
}
int server_code_index_embedding_available(void)
{
   return 1;
}

/* ---- everything else the file touches -------------------------------------- */
int aimee_kb_index_observer_required(void)
{
   return 0;
}
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
int aimee_sha256_hex(const void *data, size_t len, char out_hex[65])
{
   (void)data;
   (void)len;
   memset(out_hex, 'a', 64);
   out_hex[64] = '\0';
   return 0;
}
int code_collect_discover_repos(const char *root, code_collect_repo_cb cb, void *ctx)
{
   (void)root;
   (void)cb;
   (void)ctx;
   return 0;
}
int code_import_identity(const char *importer_path, const char *raw_import, char *out,
                         size_t out_cap)
{
   (void)importer_path;
   (void)raw_import;
   (void)out;
   (void)out_cap;
   return -1;
}
int code_index_source_is_worktree(void)
{
   return 0;
}
int code_path_import_identity(const char *path, char *out, size_t out_cap)
{
   (void)path;
   snprintf(out, out_cap, "fixture");
   return 0;
}
void code_span_set_index_reader(code_span_index_reader_fn reader)
{
   (void)reader;
}
int extract_calls(const char *ext, const char *content, call_ref_t *out, int max)
{
   (void)ext;
   (void)content;
   (void)out;
   (void)max;
   return 0;
}
int extract_definitions(const char *ext, const char *content, definition_t *out, int max)
{
   (void)ext;
   (void)content;
   (void)out;
   (void)max;
   return 0;
}
int extract_imports_sys(const char *ext, const char *content, char **out, int *sys, int max)
{
   (void)ext;
   (void)content;
   (void)out;
   (void)sys;
   (void)max;
   return 0;
}
int git_resolve_default_sha(const char *root, char *out, size_t outlen)
{
   (void)root;
   (void)out;
   (void)outlen;
   return -1;
}
int kb_client_index_scan(const char *name, const char *root, int force,
                         kb_client_index_scan_result_t *out)
{
   (void)name;
   (void)root;
   (void)force;
   (void)out;
   return -1;
}
int kb_client_local_code_enabled(void)
{
   return 1;
}
void kb_client_set_local_code_provider(kb_client_local_code_fn provider)
{
   (void)provider;
}
int ws_reg_lock(const char *ref)
{
   (void)ref;
   return -1;
}
int ws_reg_lookup(const char *ref, char *remote, size_t remote_cap)
{
   (void)ref;
   (void)remote;
   (void)remote_cap;
   return 0;
}
int ws_reg_ready(void)
{
   return 0;
}
int ws_scope_environment_root(char *out, size_t cap)
{
   (void)out;
   (void)cap;
   return -1;
}
int ws_scope_project_ref_valid(const char *buf, size_t len)
{
   (void)buf;
   (void)len;
   return 0;
}

/* ---- the batch ------------------------------------------------------------- */
static cJSON *batch(void)
{
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "project", "fixture");
   cJSON_AddStringToObject(body, "root_path", "/owned/fixture");
   cJSON_AddStringToObject(body, "phase", "stage");
   cJSON_AddStringToObject(body, "scan_id", "scan-1");
   cJSON *files = cJSON_AddArrayToObject(body, "files");
   const char *paths[] = {"unchanged.c", "changed.c", "new.c"};
   for (int i = 0; i < 3; i++)
   {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "rel_path", paths[i]);
      cJSON_AddStringToObject(f, "content", "int f(void) { return 0; }");
      cJSON_AddItemToArray(files, f);
   }
   return body;
}

static void reset(void)
{
   g_lookups = g_stages = g_embed_calls = g_staged_with_reuse = g_staged_embedded = 0;
   g_lookup_files = 0;
   g_known_reply = g_serving_available = 1;
   g_refuse_stage = 0;
}

static void test_unchanged_file_reuses_vector_others_embed(void)
{
   reset();
   cJSON *body = batch();
   int status = 0;
   char *reply = local_code("/v1/code/scan", body, &status);
   assert(status == 200);
   assert(g_lookups == 1 && g_lookup_files == 3);
   assert(!strcmp(g_lookup_serving, "serving-1"));
   assert(g_stages == 3);
   assert(g_staged_with_reuse == 1);
   assert(g_embed_calls == 2 && g_staged_embedded == 2);
   cJSON *out = cJSON_Parse(reply);
   assert(cJSON_GetObjectItemCaseSensitive(out, "embeddings_reused")->valueint == 1);
   cJSON_Delete(out);
   free(reply);
   cJSON_Delete(body);
}

static void test_lookup_failure_embeds_everything(void)
{
   reset();
   g_known_reply = 0;
   cJSON *body = batch();
   int status = 0;
   char *reply = local_code("/v1/code/scan", body, &status);
   assert(status == 200 && g_lookups == 1);
   assert(g_staged_with_reuse == 0 && g_embed_calls == 3);
   free(reply);
   cJSON_Delete(body);

   reset();
   g_serving_available = 0; /* no embedder identity: no lookup, embed as before */
   body = batch();
   reply = local_code("/v1/code/scan", body, &status);
   assert(status == 200 && g_lookups == 0 && g_embed_calls == 3);
   free(reply);
   cJSON_Delete(body);
}

static void test_refusal_stops_the_batch_with_409(void)
{
   reset();
   g_refuse_stage = 1;
   cJSON *body = batch();
   int status = 0;
   char *reply = local_code("/v1/code/scan", body, &status);
   assert(status == 409);
   assert(g_stages == 1); /* no file is staged into a scan that was taken over */
   cJSON *out = cJSON_Parse(reply);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "code")),
                  "scan_superseded"));
   cJSON_Delete(out);
   free(reply);
   cJSON_Delete(body);
}

int main(void)
{
   test_unchanged_file_reuses_vector_others_embed();
   test_lookup_failure_embeds_everything();
   test_refusal_stops_the_batch_with_409();
   printf("server code index: batch reuses stored vectors, embeds the rest, stops on refusal\n");
   return 0;
}
