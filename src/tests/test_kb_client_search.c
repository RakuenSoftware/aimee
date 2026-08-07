#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_client.h"
#include "kb_client_internal.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"
#include "runtime_secret.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_post_seen = 0;
static int g_get_seen = 0;
static int g_route_case = 0;
static int g_search_expect_all = 0;
static char g_push_root[512];
static int64_t g_dependency_now_ms = 100000;
static int g_mtls_enabled;
static int g_mtls_status;
static const char *g_mtls_response;
static const char *g_mtls_expected_content_type;
static int g_mtls_calls;

int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)content_type;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   assert(!"mTLS raw POST must not fall through to HTTP");
   return -1;
}

int kb_client_mtls_configured(void)
{
   return g_mtls_enabled;
}

char *kb_client_mtls_request_timeout_with_type(const char *method, const char *path,
                                               const char *body, const char *content_type,
                                               int timeout_ms, int *status_out)
{
   assert(g_mtls_enabled);
   assert(method && path && timeout_ms > 0);
   if (strcmp(method, "GET") == 0)
      assert(body == NULL);
   if (g_mtls_expected_content_type)
      assert(content_type && strcmp(content_type, g_mtls_expected_content_type) == 0);
   else
      assert(content_type == NULL);
   g_mtls_calls++;
   if (status_out)
      *status_out = g_mtls_status;
   return g_mtls_response ? strdup(g_mtls_response) : NULL;
}

char *kb_client_mtls_request_timeout(const char *method, const char *path, const char *body,
                                     int timeout_ms, int *status_out)
{
   return kb_client_mtls_request_timeout_with_type(method, path, body, NULL, timeout_ms,
                                                   status_out);
}

static int64_t dependency_test_clock(void)
{
   return g_dependency_now_ms;
}

static int health_get_handler(const char *url, const char *extra_headers, char **response_buf,
                              int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer test-token") == 0);
   g_get_seen++;
   if (strcmp(url, "http://127.0.0.1:4010/v1/version") == 0)
   {
      if (response_buf)
         *response_buf = strdup("{\"version\":\"v0.3.0-test\",\"service\":\"aimee-kb\"}");
      return 200;
   }
   assert(strcmp(url, "http://127.0.0.1:4010/v1/health") == 0);
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"db2_ok\":true,"
                             "\"db2_kb_tables_ok\":true,\"pgvec_ok\":true,"
                             "\"pgvec_collection_ok\":true,\"pgvec_vectors\":42,"
                             "\"pgvec_indexed_vectors\":41,\"embed_ok\":true,"
                             "\"embed_command\":\"embed --json\",\"freshness_days\":3,"
                             "\"last_ingest_at\":\"2026-05-24 00:00:00\","
                             "\"chunk_count\":7,\"embedding_count\":6,"
                             "\"last_maintenance_at\":\"2026-05-24 01:00:00\","
                             "\"last_maintenance_rows_decayed\":5,"
                             "\"last_maintenance_orphans_pruned\":4,"
                             "\"maintenance_enabled\":true,"
                             "\"warnings\":[\"warn-a\",\"warn-b\"]}");
   return 200;
}

static int status_get_handler(const char *url, const char *extra_headers, char **response_buf,
                              int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/health?status=1&project=aimee%20core%2Fkb%3F") ==
          0);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer test-token") == 0);
   g_get_seen++;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"summary_status\":\"ok\","
                             "\"owner\":\"knowledge-service\",\"available\":true,"
                             "\"project\":\"aimee core/kb?\",\"files\":2,\"chunks\":7,"
                             "\"vector\":{\"status\":\"ok\"}}");
   return 200;
}

static int search_post_handler(const char *url, const char *auth_header, const char *body,
                               char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/search") == 0);
   assert(auth_header);
   assert(strcmp(auth_header, "Authorization: Bearer test-token") == 0);
   assert(extra_headers == NULL);
   assert(body);

   cJSON *json = cJSON_Parse(body);
   assert(json);
   cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
   cJSON *query = cJSON_GetObjectItemCaseSensitive(json, "query");
   cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
   cJSON *max_results = cJSON_GetObjectItemCaseSensitive(json, "max_results");
   cJSON *format = cJSON_GetObjectItemCaseSensitive(json, "format");
   cJSON *fusion = cJSON_GetObjectItemCaseSensitive(json, "fusion_mode");

   if (g_search_expect_all)
   {
      cJSON *scope = cJSON_GetObjectItemCaseSensitive(json, "scope");
      if (g_search_expect_all == 1)
         assert(project == NULL);
      else
         assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(scope) && strcmp(scope->valuestring, "all") == 0);
   }
   else
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
   assert(cJSON_IsString(query) && strcmp(query->valuestring, "split kb") == 0);
   assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed --json") == 0);
   assert(cJSON_IsNumber(max_results) && max_results->valueint == 7);
   assert(cJSON_IsString(format) && strcmp(format->valuestring, "json") == 0);
   assert(cJSON_IsString(fusion) && strcmp(fusion->valuestring, "rrf") == 0);
   cJSON_Delete(json);

   g_post_seen++;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"result\":[{\"title\":\"hit\"}]}");
   return 200;
}

static int rejecting_post_handler(const char *url, const char *auth_header, const char *body,
                                  char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/search") == 0);
   assert(auth_header == NULL);
   g_post_seen++;
   if (response_buf)
      *response_buf = strdup("{\"error\":\"unauthorized\"}");
   return 401;
}

static int outage_post_handler(const char *url, const char *auth_header, const char *body,
                               char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   g_post_seen++;
   return -1;
}

static int typed_result_get_handler(const char *url, const char *extra_headers, char **response_buf,
                                    int timeout_ms)
{
   (void)extra_headers;
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/typed-result") == 0);
   g_get_seen++;
   if (g_route_case == 40)
   {
      *response_buf = strdup("{\"status\":\"ok\",\"facts\":[{\"id\":1}]}");
      return 200;
   }
   if (g_route_case == 41)
   {
      *response_buf = strdup("{\"status\":\"ok\",\"facts\":[]}");
      return 200;
   }
   if (g_route_case == 42)
   {
      *response_buf = strdup("{\"status\":\"ok\",\"no_answer\":true,\"citation_ids\":[]}");
      return 200;
   }
   if (g_route_case == 43)
   {
      *response_buf =
          strdup("{\"status\":\"stale\",\"observed_generation\":6,\"current_generation\":7,"
                 "\"observed_dimension\":2560,\"current_dimension\":1024}");
      return 409;
   }
   if (g_route_case == 44)
   {
      *response_buf = strdup("{\"status\":\"unavailable\",\"dependency\":\"embedder\"}");
      return 503;
   }
   assert(g_route_case == 45);
   *response_buf = strdup("{\"status\":\"unauthorized\"}");
   return 401;
}

static int queue_get_handler(const char *url, const char *extra_headers, char **response_buf,
                             int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/pipeline/status") == 0);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer queue-token") == 0);
   g_get_seen++;
   if (response_buf)
      *response_buf = strdup("{\"state\":\"running\",\"queue_depth\":4}");
   return 200;
}

static int job_get_handler(const char *url, const char *extra_headers, char **response_buf,
                           int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/jobs/42") == 0);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer queue-token") == 0);
   g_get_seen++;
   if (response_buf)
      *response_buf = strdup("{\"id\":42,\"status\":\"done\",\"project\":\"proj-alpha\"}");
   return 200;
}

static int ingest_status_get_handler(const char *url, const char *extra_headers,
                                     char **response_buf, int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/ingest/status") == 0);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer queue-token") == 0);
   g_get_seen++;
   if (response_buf)
      *response_buf =
          strdup("{\"status\":\"ok\",\"queue\":{\"pending\":2,\"running\":1,"
                 "\"done_last_24h\":5,\"failed_last_24h\":0},\"workers\":{\"configured\":2},"
                 "\"recent\":[]}");
   return 200;
}

static int workers_get_handler(const char *url, const char *extra_headers, char **response_buf,
                               int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(strcmp(url, "http://127.0.0.1:4010/v1/workers") == 0);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer queue-token") == 0);
   g_get_seen++;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"configured\":2,\"slots\":[]}");
   return 200;
}

static int intelligence_get_handler(const char *url, const char *extra_headers, char **response_buf,
                                    int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(extra_headers);
   assert(strcmp(extra_headers, "Authorization: Bearer intel-token") == 0);
   g_get_seen++;
   if (g_route_case == 30)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/intelligence/calibration/readiness") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"ready\":true,"
                                "\"surfaces_with_data\":2,\"min_rows_required\":200}");
   }
   else if (g_route_case == 31)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/intelligence/demotion/check") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"candidates\":3,\"scored\":2,"
                                "\"would_demote\":1,\"by_kind\":[]}");
   }
   else if (g_route_case == 32)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/intelligence/bandit/export") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"decision_point\":\"kb_fusion_mode\","
                                "\"decisions\":[],\"arm_stats\":[]}");
   }
   else
   {
      assert(!"unexpected intelligence route case");
   }
   return 200;
}

static char *blast_hot_response(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "file", "src/hot.c");
   cJSON_AddStringToObject(root, "project", "aimee");
   cJSON_AddNumberToObject(root, "generation", 7);
   cJSON_AddStringToObject(root, "freshness", "current");
   cJSON_AddBoolToObject(root, "resolved", 1);
   cJSON *dependents = cJSON_AddArrayToObject(root, "dependents");
   cJSON *edges = cJSON_AddArrayToObject(root, "dependent_edges");
   for (char name = 'a'; name <= 'k'; name++)
   {
      char path[2] = {name, '\0'};
      cJSON_AddItemToArray(dependents, cJSON_CreateString(path));
      cJSON *edge = cJSON_CreateObject();
      cJSON_AddStringToObject(edge, "path", path);
      cJSON_AddStringToObject(edge, "provenance", "import");
      cJSON_AddStringToObject(edge, "confidence", "high");
      cJSON_AddStringToObject(edge, "project", "aimee");
      cJSON_AddNumberToObject(edge, "generation", 7);
      cJSON_AddStringToObject(edge, "freshness", "current");
      cJSON_AddItemToArray(edges, edge);
   }
   cJSON_AddNumberToObject(root, "dependent_count", 11);
   cJSON_AddArrayToObject(root, "dependencies");
   cJSON_AddNumberToObject(root, "dependency_count", 0);
   cJSON_AddArrayToObject(root, "dependency_edges");
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json;
}

static int index_get_handler(const char *url, const char *extra_headers, char **response_buf,
                             int timeout_ms)
{
   (void)timeout_ms;
   assert(url);
   assert(extra_headers == NULL);
   g_get_seen++;
   if (g_route_case == 12)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/find?identifier=app_start&max_results=3&"
                         "scope=all") == 0);
      if (response_buf)
         *response_buf = strdup("{\"hits\":[{\"project\":\"aimee\",\"file_path\":\"src/main.c\","
                                "\"line\":12,\"kind\":\"function\"}],\"next_cursor\":null}");
   }
   else if (g_route_case == 34)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/find?identifier=target%2Ffn%3F&"
                         "max_results=2&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"hits\":[{\"project\":\"aimee core/kb?\","
                                "\"file_path\":\"src/main.c\",\"line\":12,"
                                "\"kind\":\"function\"}],\"next_cursor\":null}");
   }
   else if (g_route_case == 35)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/find?identifier=target%2Ffn%3F&"
                         "max_results=2&scope=all&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"hits\":[{\"project\":\"aimee core/kb?\","
                                "\"file_path\":\"src/main.c\",\"line\":12,"
                                "\"kind\":\"function\"}],\"next_cursor\":null}");
   }
   else if (g_route_case == 22)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/projects?max_results=2") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"projects\":[{\"name\":\"aimee\","
                                "\"root\":\"/repo/aimee\",\"scanned_at\":\"2026-05-26 00:00:00\"}],"
                                "\"next_cursor\":null}");
   }
   else if (g_route_case == 40)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/projects?max_results=2") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\"}");
   }
   else if (g_route_case == 13 || g_route_case == 14 || g_route_case == 38)
   {
      assert(strstr(url, "http://127.0.0.1:4010/v1/code/blast-radius?project=aimee&file_path=") ==
             url);
      if (g_route_case == 38)
      {
         assert(strstr(url, "file_path=src%2Flegacy.c") != NULL);
         if (response_buf)
            *response_buf = strdup("{\"file\":\"src/legacy.c\",\"dependents\":[\"src/app.c\"],"
                                   "\"dependencies\":[]}");
      }
      else if (strstr(url, "file_path=src%2Fhot.c"))
      {
         if (response_buf)
            *response_buf = blast_hot_response();
      }
      else
      {
         assert(strstr(url, "file_path=src%2Fmain.c") != NULL);
         if (response_buf)
            *response_buf = strdup(
                "{\"file\":\"src/main.c\",\"project\":\"aimee\",\"generation\":7,"
                "\"freshness\":\"current\",\"resolved\":true,"
                "\"dependents\":[\"src/app.c\"],\"dependent_count\":1,"
                "\"dependent_edges\":[{\"path\":\"src/app.c\",\"provenance\":\"import,call\","
                "\"confidence\":\"high\",\"project\":\"aimee\",\"generation\":7,"
                "\"freshness\":\"current\"}],\"dependencies\":[\"src/lib.c\"],"
                "\"dependency_count\":1,\"dependency_edges\":[{\"identity\":\"src/lib.c\","
                "\"provenance\":\"import\",\"confidence\":\"high\",\"project\":\"aimee\","
                "\"generation\":7,\"freshness\":\"current\"}]}");
      }
   }
   else if (g_route_case == 18)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/structure?project=aimee&"
                         "file_path=src%2Fmain.c&max_results=4") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"definitions\":[{\"name\":\"app_start\","
                                "\"kind\":\"function\",\"line\":12}]}");
   }
   else if (g_route_case == 19)
   {
      assert(
          strcmp(url, "http://127.0.0.1:4010/v1/code/project-stats?project=aimee%20core%2Fkb%3F") ==
          0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"project\":\"aimee core/kb?\","
                                "\"files\":11,\"definitions\":7,"
                                "\"langs\":[{\"lang\":\"c\",\"count\":8},"
                                "{\"lang\":\"h\",\"count\":3}]}");
   }
   else if (g_route_case == 20)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/search?query=split%20kb%2Findex%3F&"
                         "max_results=2&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"hits\":[{\"project\":\"aimee core/kb?\","
                                "\"file_path\":\"src/search.c\",\"snippet\":\"split kb index\","
                                "\"rank\":0.75}],\"next_cursor\":null}");
   }
   else if (g_route_case == 21)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/callers?symbol=target%2Ffn%3F&"
                         "max_results=2&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"hits\":[{\"project\":\"aimee core/kb?\","
                                "\"file_path\":\"src/caller.c\",\"caller\":\"caller_fn\","
                                "\"line\":44}],\"next_cursor\":null}");
   }
   else if (g_route_case == 36)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/search?query=split%20kb%2Findex%3F&"
                         "max_results=2&scope=all&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"hits\":[]}");
   }
   else if (g_route_case == 37)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/callers?symbol=target%2Ffn%3F&"
                         "max_results=2&scope=all&project=aimee%20core%2Fkb%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"hits\":[]}");
   }
   else if (g_route_case == 39)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/context?query=split%20kb%2Findex%3F&"
                         "max_results=4&project=aimee%20core%2Fkb%3F&symbol=target%2Ffn%3F") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"project\":\"aimee core/kb?\","
                                "\"generation\":7,\"freshness\":\"current\","
                                "\"resolved\":true,\"results\":[]}");
   }
   else
   {
      assert(!"unexpected index route case");
   }
   return 200;
}

static int maintenance_post_handler(const char *url, const char *auth_header, const char *body,
                                    char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   (void)extra_headers;
   assert(url);
   assert(auth_header == NULL);
   assert(body);

   cJSON *json = cJSON_Parse(body);
   assert(json);

   /* Cases 1 (maintenance/repair) and 2 (maintenance/clear) were removed with
    * their client wrappers — aimee-server no longer addresses those routes. */
   if (g_route_case == 3)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/maintenance/reconcile") == 0);
      cJSON *dry_run = cJSON_GetObjectItemCaseSensitive(json, "dry_run");
      assert(cJSON_IsBool(dry_run) && cJSON_IsTrue(dry_run));
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"dry_run\":true}");
   }
   else if (g_route_case == 4)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/scan") == 0);
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      cJSON *root_path = cJSON_GetObjectItemCaseSensitive(json, "root_path");
      cJSON *force = cJSON_GetObjectItemCaseSensitive(json, "force");
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(root_path) && strcmp(root_path->valuestring, "/repo") == 0);
      assert(cJSON_IsBool(force) && cJSON_IsTrue(force));
      if (response_buf)
         *response_buf = strdup("{\"state\":\"accepted\"}");
   }
   else if (g_route_case == 17)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/scan") == 0);
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      cJSON *root_path = cJSON_GetObjectItemCaseSensitive(json, "root_path");
      cJSON *force = cJSON_GetObjectItemCaseSensitive(json, "force");
      cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(root_path) && strcmp(root_path->valuestring, "/repo") == 0);
      assert(cJSON_IsBool(force) && cJSON_IsTrue(force));
      assert(files == NULL || (cJSON_IsArray(files) && cJSON_GetArraySize(files) == 0));
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"skipped\":false,\"project\":\"aimee\","
                                "\"files\":2,\"inspected\":3}");
   }
   else if (g_route_case == 5)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/drain") == 0);
      cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
      cJSON *timeout = cJSON_GetObjectItemCaseSensitive(json, "timeout");
      assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed") == 0);
      assert(cJSON_IsNumber(timeout) && timeout->valueint == 9);
      if (response_buf)
         *response_buf = strdup("{\"state\":\"idle\",\"processed\":2}");
   }
   else if (g_route_case == 6)
   {
      /* Synchronous build now does a single POST to /v1/code/build; aimee-kb
       * owns the compute + store + canonical scan. */
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/build") == 0);
      cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
      cJSON *force = cJSON_GetObjectItemCaseSensitive(json, "force");
      assert(cJSON_IsString(path) && strcmp(path->valuestring, g_push_root) == 0);
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed") == 0);
      assert(cJSON_IsBool(force) && cJSON_IsTrue(force));
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"project\":\"aimee\",\"files_indexed\":1}");
   }
   else if (g_route_case == 7)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/code/update") == 0);
      cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
      assert(cJSON_IsString(path) && strcmp(path->valuestring, g_push_root) == 0);
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"project\":\"aimee\",\"files_indexed\":1}");
   }
   else if (g_route_case == 8)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/ingest") == 0);
      cJSON *workspace = cJSON_GetObjectItemCaseSensitive(json, "workspace");
      cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
      cJSON *force = cJSON_GetObjectItemCaseSensitive(json, "force");
      assert(cJSON_IsString(workspace) && strcmp(workspace->valuestring, "default") == 0);
      assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed") == 0);
      assert(cJSON_IsBool(force) && cJSON_IsTrue(force));
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"projects_queued\":2}");
   }
   else if (g_route_case == 33)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/actions/memory.directive_sweep_expired") == 0);
      assert(cJSON_GetArraySize(json) == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"expired\":1}");
   }
   else
   {
      assert(!"unexpected route case");
   }

   cJSON_Delete(json);
   g_post_seen++;
   return g_route_case == 4 ? 202 : 200;
}

static void test_search_uses_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(search_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   char *resp = kb_client_search_json_ex("aimee", "split kb", "embed --json", 7, "json", "rrf");
   assert(resp);
   assert(strstr(resp, "\"status\":\"ok\"") != NULL);
   assert(strstr(resp, "\"title\":\"hit\"") != NULL);
   free(resp);

   g_search_expect_all = 1;
   resp = kb_client_search_json_ex(NULL, "split kb", "embed --json", 7, "json", "rrf");
   assert(resp && strstr(resp, "\"status\":\"ok\"") != NULL);
   free(resp);
   g_search_expect_all = 2;
   resp = kb_client_search_json_scoped_ex("aimee", 1, "split kb", "embed --json", 7, "json", "rrf");
   assert(resp && strstr(resp, "\"status\":\"ok\"") != NULL);
   free(resp);
   g_search_expect_all = 0;

   assert(g_post_seen == 3);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_search_v1_reports_http_status(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(rejecting_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   char *resp = kb_client_search_json_ex("aimee", "split kb", NULL, 7, "json", NULL);
   assert(resp);
   assert(strstr(resp, "\"status\":\"unauthorized\"") != NULL);
   assert(strstr(resp, "\"retryable\":false") != NULL);
   assert(strstr(resp, "HTTP 401") != NULL);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAUTHORIZED);
   free(resp);

   assert(g_post_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
}

static void test_outage_is_typed_bounded_and_recovers(void)
{
   g_post_seen = 0;
   g_dependency_now_ms = 100000;
   kb_client_dependency_reset_for_tests();
   kb_client_dependency_set_clock_for_tests(dependency_test_clock);
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(outage_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   for (int i = 0; i < 4; i++)
   {
      char *resp = kb_client_search_json_ex("aimee", "split kb", "embed --json", 7, "json", "rrf");
      assert(resp && strstr(resp, "\"status\":\"unavailable\"") != NULL);
      assert(strstr(resp, "\"retryable\":true") != NULL);
      cJSON *typed = cJSON_Parse(resp);
      cJSON *retry_after = typed ? cJSON_GetObjectItemCaseSensitive(typed, "retry_after_ms") : NULL;
      assert(cJSON_IsNumber(retry_after));
      assert(retry_after->valuedouble >= 1000 && retry_after->valuedouble <= 30000);
      cJSON_Delete(typed);
      free(resp);
   }
   /* The fourth client call is breaker-suppressed; only three reached HTTP. */
   assert(g_post_seen == 3);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE);
   kb_client_dependency_health_t health;
   kb_client_dependency_health(&health);
   assert(strcmp(health.state, "open") == 0);
   assert(health.retry_after_ms >= 1000 && health.retry_after_ms <= 1250);
   assert(health.suppressed_calls == 1);

   g_dependency_now_ms += health.retry_after_ms;
   mock_agent_http_set_post_handler(search_post_handler);
   char *recovered =
       kb_client_search_json_ex("aimee", "split kb", "embed --json", 7, "json", "rrf");
   assert(recovered && strstr(recovered, "\"status\":\"ok\"") != NULL);
   free(recovered);
   assert(g_post_seen == 4);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_OK);
   kb_client_dependency_health(&health);
   assert(strcmp(health.state, "closed") == 0);

   kb_client_dependency_set_clock_for_tests(NULL);
   kb_client_dependency_reset_for_tests();
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_exact_result_statuses(void)
{
   g_get_seen = 0;
   kb_client_dependency_reset_for_tests();
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(typed_result_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   const kb_client_result_status_t expected[] = {
       KB_CLIENT_RESULT_OK,    KB_CLIENT_RESULT_EMPTY,       KB_CLIENT_RESULT_ABSTAINED,
       KB_CLIENT_RESULT_STALE, KB_CLIENT_RESULT_UNAVAILABLE, KB_CLIENT_RESULT_UNAUTHORIZED,
   };
   for (int i = 0; i < 6; i++)
   {
      g_route_case = 40 + i;
      int http_status = 0;
      char *json = kb_client_v1_get_json("/v1/typed-result", 100, &http_status);
      if (i < 3)
         assert(json != NULL && http_status == 200);
      else
         assert(json == NULL);
      free(json);
      assert(kb_client_last_result_status() == expected[i]);
      assert(strcmp(kb_client_result_status_name(expected[i]),
                    (const char *[]){"ok", "empty", "abstained", "stale", "unavailable",
                                     "unauthorized"}[i]) == 0);
      if (i == 3 || i == 4)
      {
         char *typed = kb_client_last_result_json("typed result");
         assert(typed);
         if (i == 3)
         {
            assert(strstr(typed, "\"observed_generation\":6") != NULL);
            assert(strstr(typed, "\"current_generation\":7") != NULL);
            assert(strstr(typed, "\"observed_dimension\":2560") != NULL);
            assert(strstr(typed, "\"current_dimension\":1024") != NULL);
         }
         else
            assert(strstr(typed, "\"dependency\":\"embedder\"") != NULL);
         free(typed);
      }
   }
   assert(g_get_seen == 6);

   /* An embedder 503 is a typed internal dependency outage, not evidence that
    * the reachable KB transport itself should open. */
   kb_client_dependency_health_t health;
   kb_client_dependency_health(&health);
   assert(strcmp(health.state, "closed") == 0 && health.failure_streak == 0);

   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   kb_client_dependency_reset_for_tests();
   g_route_case = 0;
}

static void test_health_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(health_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   kb_health_t health;
   assert(kb_client_health(&health) == 0);
   assert(health.process_ok == 1);
   assert(strcmp(health.version, "v0.3.0-test") == 0);
   assert(health.db2_ok == 1);
   assert(health.db2_kb_tables_ok == 1);
   assert(health.pgvec_ok == 1);
   assert(health.pgvec_collection_ok == 1);
   assert(health.pgvec_vectors == 42);
   assert(health.pgvec_indexed == 41);
   assert(health.embed_ok == 1);
   assert(strcmp(health.embed_command, "embed --json") == 0);
   assert(health.freshness_days == 3);
   assert(strcmp(health.last_ingest_at, "2026-05-24 00:00:00") == 0);
   assert(health.chunk_count == 7);
   assert(health.embedding_count == 6);
   assert(strcmp(health.last_maintenance_at, "2026-05-24 01:00:00") == 0);
   assert(health.last_maintenance_rows_decayed == 5);
   assert(health.last_maintenance_orphans_pruned == 4);
   assert(health.maintenance_enabled == 1);
   assert(strstr(health.warnings, "warn-a") != NULL);
   assert(strstr(health.warnings, "warn-b") != NULL);

   assert(strcmp(health.status, "ok") == 0);
   assert(health.blockers[0] == '\0');

   assert(g_get_seen == 2);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

/* A kb that answers "degraded" is UP and is telling us exactly what is wrong.
 *
 * kb_client_health used to require status == "ok" and return -1 for anything
 * else, which predates the kb having any other verdict to send. The moment it
 * gained one, that check would have converted every degraded kb into a transport
 * failure: `aimee status` would print "the knowledge base did not answer" about a
 * running kb, and the blockers explaining the fault would be discarded unread —
 * the original defect inverted, and strictly worse, because "unreachable" sends
 * the operator to look at the network.
 *
 * Reachability and capability are separate answers. Assert they stay separate. */
static int degraded_health_get_handler(const char *url, const char *extra_headers,
                                       char **response_buf, int timeout_ms)
{
   (void)timeout_ms;
   (void)extra_headers;
   assert(url);
   g_get_seen++;
   if (strstr(url, "/v1/version"))
   {
      if (response_buf)
         *response_buf = strdup("{\"version\":\"v0.3.0-test\",\"service\":\"aimee-kb\"}");
      return 200;
   }
   assert(strcmp(url, "http://127.0.0.1:4010/v1/health") == 0);
   if (response_buf)
      *response_buf = strdup("{\"status\":\"degraded\",\"db2_ok\":true,"
                             "\"db2_kb_tables_ok\":true,\"pgvec_ok\":true,"
                             "\"pgvec_collection_ok\":true,\"pgvec_vectors\":0,"
                             "\"embed_ok\":false,\"embed_command\":\"\","
                             "\"chunk_count\":0,\"embedding_count\":0,"
                             "\"warnings\":[],"
                             "\"blockers\":[\"no embedder configured: set embedder_model\","
                             "\"embedder width mismatch: 3 vector(s) refused\"]}");
   return 200;
}

static void test_health_degraded_is_reachable_and_carries_blockers(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(degraded_health_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   kb_health_t health;
   /* Not -1: the kb answered. */
   assert(kb_client_health(&health) == 0);
   assert(health.process_ok == 1);
   assert(strcmp(health.status, "degraded") == 0);
   /* Every blocker survives the boundary, newline-joined and in order. */
   assert(strstr(health.blockers, "no embedder configured") != NULL);
   assert(strstr(health.blockers, "width mismatch") != NULL);
   assert(strchr(health.blockers, '\n') != NULL);
   /* The siblings still parse — a degraded response is a full response. */
   assert(health.embed_ok == 0);
   assert(health.db2_ok == 1);

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

/* An older kb sends no `status` field shape we recognise beyond the string, and
 * no blockers at all. It must still read as reachable with an empty verdict —
 * callers distinguish "said ok" from "said nothing" and must not read the latter
 * as the former. */
static int legacy_health_get_handler(const char *url, const char *extra_headers,
                                     char **response_buf, int timeout_ms)
{
   (void)timeout_ms;
   (void)extra_headers;
   assert(url);
   g_get_seen++;
   if (strstr(url, "/v1/version"))
      return 404;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"db2_ok\":true,\"pgvec_ok\":true}");
   return 200;
}

static void test_health_legacy_kb_without_blockers(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(legacy_health_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   kb_health_t health;
   assert(kb_client_health(&health) == 0);
   assert(health.process_ok == 1);
   assert(strcmp(health.status, "ok") == 0);
   assert(health.blockers[0] == '\0');

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(status_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   char *status = kb_client_project_status_json("aimee core/kb?");
   assert(status);
   assert(strstr(status, "\"summary_status\":\"ok\"") != NULL);
   assert(strstr(status, "\"project\":\"aimee core/kb?\"") != NULL);
   assert(strstr(status, "\"vector\"") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

/* Which credential aimee-server PRESENTS to aimee-kb.
 *
 * AIMEE_KB_CLIENT_BEARER_TOKEN is the server's own outbound credential and wins,
 * so it can be a scoped `service` token that holds only the data plane. It falls
 * back to AIMEE_KB_API_BEARER_TOKEN — aimee-kb's inbound token — because that is
 * what existing deployments set; without the fallback an upgrade would silently
 * stop reaching the kb. The fallback means presenting the OWNER credential, which
 * the client warns about; this pins the SELECTION, which is what decides
 * authority. */
static char g_seen_auth[512];
static int auth_capture_handler(const char *url, const char *auth_header, const char *body,
                                char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   snprintf(g_seen_auth, sizeof(g_seen_auth), "%s", auth_header ? auth_header : "");
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\"}");
   return 0;
}

static void test_client_bearer_selection(void)
{
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(auth_capture_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);

   /* Only the inbound token set: fall back to it, so an upgrade keeps working. */
   runtime_secret_remove("AIMEE_KB_CLIENT_BEARER_TOKEN");
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "owner-secret") == 0);
   g_seen_auth[0] = '\0';
   free(kb_client_reconcile_json(1));
   assert(strstr(g_seen_auth, "owner-secret") != NULL);

   /* The server's own credential wins when set, so a scoped service token is
    * what actually reaches the wire. */
   assert(runtime_secret_store("AIMEE_KB_CLIENT_BEARER_TOKEN", "scope:service:aimee-server:svc") ==
          0);
   g_seen_auth[0] = '\0';
   free(kb_client_reconcile_json(1));
   assert(strstr(g_seen_auth, "scope:service:aimee-server:svc") != NULL);
   assert(strstr(g_seen_auth, "owner-secret") == NULL);

   /* Neither set: no Authorization header at all (the kb may run auth-off). */
   runtime_secret_remove("AIMEE_KB_CLIENT_BEARER_TOKEN");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   g_seen_auth[0] = '\0';
   free(kb_client_reconcile_json(1));
   assert(g_seen_auth[0] == '\0');

   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
}

static void test_maintenance_uses_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   /* kb_client_repair_json / kb_client_clear_json are gone: their only callers
    * were `aimee kb repair` / `aimee kb clear`, which the live dispatcher never
    * exposed, so both were unreachable and the linker discarded them. Maintenance
    * on aimee-kb is an administrative action there, not a client capability
    * here. Cases 1 and 2 went with them. */
   g_route_case = 3;
   char *reconcile = kb_client_reconcile_json(1);
   assert(reconcile);
   assert(strstr(reconcile, "\"dry_run\":true") != NULL);
   free(reconcile);

   g_route_case = 4;
   assert(kb_client_canonical_index_scan("aimee", "/repo", 1) == 0);

   g_route_case = 5;
   char *drain = kb_client_queue_drain_json("embed", 9);
   assert(drain);
   assert(strstr(drain, "\"processed\":2") != NULL);
   free(drain);

   assert(g_post_seen == 3); /* was 5; repair + clear were removed */
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_queue_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(queue_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "queue-token") == 0);

   char *status = kb_client_queue_status_json();
   assert(status);
   assert(strstr(status, "\"queue_depth\":4") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_job_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(job_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "queue-token") == 0);

   char *status = kb_client_job_status_json(42);
   assert(status);
   assert(strstr(status, "\"status\":\"done\"") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_build_update_ingest_use_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   snprintf(g_push_root, sizeof(g_push_root), "/repo");

   g_route_case = 6;
   char *build = kb_client_build_json(g_push_root, "aimee", "embed", 1);
   assert(build);
   assert(strstr(build, "\"files_indexed\":1") != NULL);
   free(build);

   g_route_case = 7;
   char *update = kb_client_update_json(g_push_root, "aimee", "embed");
   assert(update);
   assert(strstr(update, "\"files_indexed\":1") != NULL);
   free(update);

   g_route_case = 8;
   char *ingest = kb_client_ingest_json("default", "embed", 1);
   assert(ingest);
   assert(strstr(ingest, "\"projects_queued\":2") != NULL);
   free(ingest);

   /* build (/v1/code/build) + update (/v1/code/update) + ingest (/v1/ingest) = 3 POSTs. */
   assert(g_post_seen == 3);
   g_push_root[0] = '\0';
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_ingest_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(ingest_status_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "queue-token") == 0);

   char *status = kb_client_ingest_status_json();
   assert(status);
   assert(strstr(status, "\"done_last_24h\":5") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_workers_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "queue-token") == 0);
   mock_agent_http_set_get_handler(workers_get_handler);

   char *status = kb_client_workers_json();
   assert(status);
   assert(strstr(status, "\"configured\":2") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_intelligence_readiness_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(intelligence_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "intel-token") == 0);

   g_route_case = 30;
   char *calibrate = kb_client_calibrate_readiness_json();
   assert(calibrate);
   assert(strstr(calibrate, "\"surfaces_with_data\":2") != NULL);
   free(calibrate);

   g_route_case = 31;
   char *demote = kb_client_demote_check_json();
   assert(demote);
   assert(strstr(demote, "\"would_demote\":1") != NULL);
   free(demote);

   g_route_case = 32;
   char *bandit = kb_client_bandit_export_json();
   assert(bandit);
   assert(strstr(bandit, "\"decision_point\":\"kb_fusion_mode\"") != NULL);
   free(bandit);

   assert(g_get_seen == 3);
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_action_wrappers_use_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   g_route_case = 33;
   char *resp = kb_client_memory_directive_sweep_expired_json();
   assert(resp);
   assert(strstr(resp, "\"expired\":1") != NULL);
   free(resp);

   assert(g_post_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_index_reads_use_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(index_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   g_route_case = 12;
   term_hit_t hits[3];
   assert(kb_client_index_find("app_start", hits, 3) == 1);
   assert(strcmp(hits[0].project, "aimee") == 0);
   assert(strcmp(hits[0].file_path, "src/main.c") == 0);
   assert(hits[0].line == 12);
   assert(strcmp(hits[0].kind, "function") == 0);

   g_route_case = 34;
   assert(kb_client_index_find_project("aimee core/kb?", "target/fn?", hits, 2) == 1);
   assert(strcmp(hits[0].project, "aimee core/kb?") == 0);

   g_route_case = 35;
   assert(kb_client_index_find_scoped("aimee core/kb?", 1, "target/fn?", hits, 2) == 1);

   g_route_case = 22;
   project_info_t projects[2];
   assert(kb_client_index_list(projects, 2) == 1);
   assert(strcmp(projects[0].name, "aimee") == 0);
   assert(strcmp(projects[0].root, "/repo/aimee") == 0);
   assert(strcmp(projects[0].scanned_at, "2026-05-26 00:00:00") == 0);

   g_route_case = 40;
   assert(kb_client_index_list(projects, 2) == -1);

   g_route_case = 13;
   blast_radius_t br;
   assert(kb_client_index_blast_radius("aimee", "src/main.c", &br) == 0);
   assert(strcmp(br.file, "src/main.c") == 0);
   assert(br.dependent_count == 1);
   assert(strcmp(br.dependents[0], "src/app.c") == 0);
   assert(br.dependency_count == 1);
   assert(strcmp(br.dependencies[0], "src/lib.c") == 0);
   assert(br.resolved == 1);
   assert(strcmp(br.project, "aimee") == 0);
   assert(br.generation == 7);
   assert(strcmp(br.dependent_meta[0].provenance, "import,call") == 0);
   assert(strcmp(br.dependency_meta[0].freshness, "current") == 0);

   g_route_case = 38;
   assert(kb_client_index_blast_radius("aimee", "src/legacy.c", &br) == -1);
   assert(br.resolved == 0);

   g_route_case = 14;
   char *paths[] = {"src/main.c", "src/hot.c"};
   char *preview = kb_client_index_blast_radius_preview_json("aimee", paths, 2);
   assert(preview);
   assert(strstr(preview, "\"total_dependents\":12") != NULL);
   assert(strstr(preview, "\"severity\":\"red\"") != NULL);
   assert(strstr(preview, "src/hot.c has 11 dependents") != NULL);
   free(preview);

   g_route_case = 18;
   definition_t defs[4];
   assert(kb_client_index_structure("aimee", "src/main.c", defs, 4) == 1);
   assert(strcmp(defs[0].name, "app_start") == 0);
   assert(strcmp(defs[0].kind, "function") == 0);
   assert(defs[0].line == 12);

   g_route_case = 19;
   int files = 0;
   int definitions = 0;
   assert(kb_client_index_project_stats("aimee core/kb?", &files, &definitions) == 0);
   assert(files == 11);
   assert(definitions == 7);

   char langs[256];
   assert(kb_client_index_project_lang("aimee core/kb?", langs, sizeof(langs)) == 0);
   assert(strstr(langs, "\"lang\":\"c\"") != NULL);
   assert(strstr(langs, "\"count\":8") != NULL);

   g_route_case = 20;
   code_search_hit_t search_hits[2];
   assert(kb_client_index_code_search("split kb/index?", "aimee core/kb?", search_hits, 2) == 1);
   assert(strcmp(search_hits[0].project, "aimee core/kb?") == 0);
   assert(strcmp(search_hits[0].file_path, "src/search.c") == 0);
   assert(strcmp(search_hits[0].snippet, "split kb index") == 0);
   assert(search_hits[0].rank == 0.75);

   g_route_case = 36;
   assert(kb_client_index_code_search_scoped("split kb/index?", "aimee core/kb?", 1, search_hits,
                                             2) == 0);

   g_route_case = 21;
   caller_hit_t caller_hits[2];
   assert(kb_client_index_find_callers("aimee core/kb?", "target/fn?", caller_hits, 2) == 1);
   assert(strcmp(caller_hits[0].project, "aimee core/kb?") == 0);
   assert(strcmp(caller_hits[0].file_path, "src/caller.c") == 0);
   assert(strcmp(caller_hits[0].caller, "caller_fn") == 0);
   assert(caller_hits[0].line == 44);

   g_route_case = 37;
   assert(kb_client_index_find_callers_scoped("aimee core/kb?", 1, "target/fn?", caller_hits, 2) ==
          0);

   g_route_case = 39;
   int context_status = 0;
   char *context =
       kb_client_code_context("split kb/index?", "target/fn?", "aimee core/kb?", &context_status);
   assert(context && context_status == 200);
   assert(strstr(context, "\"project\":\"aimee core/kb?\"") != NULL);
   free(context);

   assert(g_get_seen == 17);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_mtls_non_2xx_is_not_returned_as_valid_json(void)
{
   unsetenv("AIMEE_KB_API_URL");
   kb_client_dependency_reset_for_tests();
   g_mtls_enabled = 1;
   g_mtls_status = 503;
   g_mtls_response = "{\"status\":\"unavailable\",\"dependency\":\"kb\",\"retryable\":true}";

   project_info_t projects[2];
   assert(kb_client_index_list(projects, 2) == -1);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE);

   g_mtls_status = 403;
   g_mtls_response = "{\"status\":\"unauthorized\",\"retryable\":false}";
   cJSON *request = cJSON_CreateObject();
   assert(request != NULL);
   int status = 0;
   assert(kb_client_v1_post_json("/v1/search", request, 5000, &status) == NULL);
   cJSON_Delete(request);
   assert(status == 403);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAUTHORIZED);

   g_mtls_response = NULL;
   g_mtls_status = 0;
   g_mtls_enabled = 0;
   kb_client_dependency_reset_for_tests();
}

static void test_mtls_raw_post_preserves_content_type_and_status(void)
{
   unsetenv("AIMEE_KB_API_URL");
   kb_client_dependency_reset_for_tests();
   g_mtls_enabled = 1;
   g_mtls_calls = 0;
   g_mtls_expected_content_type = "Content-Type: multipart/form-data; boundary=unit";
   g_mtls_status = 200;
   g_mtls_response = "{\"status\":\"ok\"}";

   int status = 0;
   char *response = kb_client_v1_post_body_with_type("/v1/docs", "--unit--",
                                                     g_mtls_expected_content_type, 5000, &status);
   assert(response && status == 200 && g_mtls_calls == 1);
   free(response);

   g_mtls_status = 503;
   g_mtls_response = "{\"status\":\"unavailable\",\"dependency\":\"kb\",\"retryable\":true}";
   response = kb_client_v1_post_body_with_type("/v1/docs", "--unit--", g_mtls_expected_content_type,
                                               5000, &status);
   assert(response == NULL && status == 503 && g_mtls_calls == 2);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE);

   g_mtls_expected_content_type = NULL;
   g_mtls_response = NULL;
   g_mtls_status = 0;
   g_mtls_enabled = 0;
   kb_client_dependency_reset_for_tests();
}

static void test_index_scan_uses_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");

   g_route_case = 17;
   kb_client_index_scan_result_t res;
   assert(kb_client_index_scan("aimee", "/repo", 1, &res) == 0);
   assert(res.skipped == 0);
   assert(res.projects == 1);
   assert(res.files == 2);
   assert(res.inspected == 3);

   assert(g_post_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

/* A call that burned its whole budget and got nothing timed out; one that failed
 * immediately did not. Only the latter is evidence the KB is unreachable, and
 * only the latter may open the shared breaker -- a slow ingest scan opening it
 * suppressed every unrelated KB call in the process. */
static void test_timeout_is_distinguished_from_unreachable(void)
{
   /* Spent the budget with nothing to show: a timeout. */
   assert(kb_transport_call_timed_out(-1, NULL, 300000, 300000) == 1);
   assert(kb_transport_call_timed_out(-1, NULL, 270000, 300000) == 1); /* 90% counts */

   /* Failed fast: nobody answered. */
   assert(kb_transport_call_timed_out(-1, NULL, 5, 300000) == 0);

   /* Anything that actually answered is not a timeout, however long it took. */
   assert(kb_transport_call_timed_out(200, "{}", 300000, 300000) == 0);
   assert(kb_transport_call_timed_out(503, NULL, 300000, 300000) == 0);

   /* No budget declared: cannot claim a timeout. */
   assert(kb_transport_call_timed_out(-1, NULL, 300000, 0) == 0);
}

/* Bulk ingestion and interactive reads keep separate failure budgets. A corpus
 * ingest that is failing says nothing about whether a symbol lookup will work,
 * and while they shared one breaker it could suppress every unrelated KB call
 * in the process. */
static void test_bulk_and_interactive_have_separate_budgets(void)
{
   assert(kb_dependency_class_for_path("/v1/code/scan") == KB_DEP_BULK);
   assert(kb_dependency_class_for_path("/v1/code/build") == KB_DEP_BULK);
   assert(kb_dependency_class_for_path("/v1/code/embed") == KB_DEP_BULK);
   assert(kb_dependency_class_for_path("/v1/ingest") == KB_DEP_BULK);

   /* Reads and lookups are interactive and must stay answerable. */
   assert(kb_dependency_class_for_path("/v1/code/find") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path("/v1/code/callers") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path("/v1/search") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path("/v1/health") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path("/v1/memory/recall") == KB_DEP_INTERACTIVE);

   /* Unknown and empty paths default to interactive: a path we cannot classify
    * must not silently borrow the bulk budget. */
   assert(kb_dependency_class_for_path("/v1/something/new") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path("") == KB_DEP_INTERACTIVE);
   assert(kb_dependency_class_for_path(NULL) == KB_DEP_INTERACTIVE);
}

int main(void)
{
   test_bulk_and_interactive_have_separate_budgets();
   test_timeout_is_distinguished_from_unreachable();
   test_health_uses_v1_api_when_configured();
   test_health_degraded_is_reachable_and_carries_blockers();
   test_health_legacy_kb_without_blockers();
   test_status_uses_v1_api_when_configured();
   test_search_uses_v1_api_when_configured();
   test_search_v1_reports_http_status();
   test_outage_is_typed_bounded_and_recovers();
   test_exact_result_statuses();
   test_maintenance_uses_v1_api_when_configured();
   test_build_update_ingest_use_v1_api_when_configured();
   test_queue_status_uses_v1_api_when_configured();
   test_job_status_uses_v1_api_when_configured();
   test_ingest_status_uses_v1_api_when_configured();
   test_workers_uses_v1_api_when_configured();
   test_intelligence_readiness_uses_v1_api_when_configured();
   test_action_wrappers_use_v1_api_when_configured();
   test_index_reads_use_v1_api_when_configured();
   test_mtls_non_2xx_is_not_returned_as_valid_json();
   test_mtls_raw_post_preserves_content_type_and_status();
   test_index_scan_uses_v1_api_when_configured();
   test_client_bearer_selection();
   printf("test_kb_client_search: ok\n");
   return 0;
}
