#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_client.h"
#include "kb_client_internal.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_post_seen = 0;
static int g_get_seen = 0;
static int g_route_case = 0;
static int g_search_expect_all = 0;
static char g_push_root[512];

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

   if (g_route_case == 1)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/maintenance/repair") == 0);
      cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      cJSON *embedding = cJSON_GetObjectItemCaseSensitive(json, "embedding_command");
      assert(cJSON_IsString(path) && strcmp(path->valuestring, "/repo") == 0);
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      assert(cJSON_IsString(embedding) && strcmp(embedding->valuestring, "embed") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"project\":\"aimee\"}");
   }
   else if (g_route_case == 2)
   {
      assert(strcmp(url, "http://127.0.0.1:4010/v1/maintenance/clear") == 0);
      cJSON *project = cJSON_GetObjectItemCaseSensitive(json, "project");
      assert(cJSON_IsString(project) && strcmp(project->valuestring, "aimee") == 0);
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\",\"chunks_deleted\":3}");
   }
   else if (g_route_case == 3)
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
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "test-token", 1) == 0);

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
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_search_v1_reports_http_status(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(rejecting_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");

   char *resp = kb_client_search_json_ex("aimee", "split kb", NULL, 7, "json", NULL);
   assert(resp);
   assert(strstr(resp, "\"status\":\"error\"") != NULL);
   assert(strstr(resp, "HTTP 401") != NULL);
   free(resp);

   assert(g_post_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
}

static void test_health_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(health_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "test-token", 1) == 0);

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

   assert(g_get_seen == 2);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(status_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "test-token", 1) == 0);

   char *status = kb_client_project_status_json("aimee core/kb?");
   assert(status);
   assert(strstr(status, "\"summary_status\":\"ok\"") != NULL);
   assert(strstr(status, "\"project\":\"aimee core/kb?\"") != NULL);
   assert(strstr(status, "\"vector\"") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_maintenance_uses_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");

   g_route_case = 1;
   char *repair = kb_client_repair_json("/repo", "aimee", "embed");
   assert(repair);
   assert(strstr(repair, "\"status\":\"ok\"") != NULL);
   free(repair);

   g_route_case = 2;
   char *cleared = kb_client_clear_json("aimee");
   assert(cleared);
   assert(strstr(cleared, "\"chunks_deleted\":3") != NULL);
   free(cleared);

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

   assert(g_post_seen == 5);
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
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "queue-token", 1) == 0);

   char *status = kb_client_queue_status_json();
   assert(status);
   assert(strstr(status, "\"queue_depth\":4") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_job_status_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(job_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "queue-token", 1) == 0);

   char *status = kb_client_job_status_json(42);
   assert(status);
   assert(strstr(status, "\"status\":\"done\"") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_build_update_ingest_use_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
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
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "queue-token", 1) == 0);

   char *status = kb_client_ingest_status_json();
   assert(status);
   assert(strstr(status, "\"done_last_24h\":5") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_workers_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1);
   setenv("AIMEE_KB_API_BEARER_TOKEN", "queue-token", 1);
   mock_agent_http_set_get_handler(workers_get_handler);

   char *status = kb_client_workers_json();
   assert(status);
   assert(strstr(status, "\"configured\":2") != NULL);
   free(status);

   assert(g_get_seen == 1);
   unsetenv("AIMEE_KB_API_URL");
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
}

static void test_intelligence_readiness_uses_v1_api_when_configured(void)
{
   g_get_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_get_handler(intelligence_get_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   assert(setenv("AIMEE_KB_API_BEARER_TOKEN", "intel-token", 1) == 0);

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
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_action_wrappers_use_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");

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
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");

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

   assert(g_get_seen == 16);
   unsetenv("AIMEE_KB_API_URL");
   mock_agent_http_reset();
   g_route_case = 0;
}

static void test_index_scan_uses_v1_api_when_configured(void)
{
   g_post_seen = 0;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(maintenance_post_handler);
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010", 1) == 0);
   unsetenv("AIMEE_KB_API_BEARER_TOKEN");

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

int main(void)
{
   test_health_uses_v1_api_when_configured();
   test_status_uses_v1_api_when_configured();
   test_search_uses_v1_api_when_configured();
   test_search_v1_reports_http_status();
   test_maintenance_uses_v1_api_when_configured();
   test_build_update_ingest_use_v1_api_when_configured();
   test_queue_status_uses_v1_api_when_configured();
   test_job_status_uses_v1_api_when_configured();
   test_ingest_status_uses_v1_api_when_configured();
   test_workers_uses_v1_api_when_configured();
   test_intelligence_readiness_uses_v1_api_when_configured();
   test_action_wrappers_use_v1_api_when_configured();
   test_index_reads_use_v1_api_when_configured();
   test_index_scan_uses_v1_api_when_configured();
   printf("test_kb_client_search: ok\n");
   return 0;
}
