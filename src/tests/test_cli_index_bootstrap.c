/* test_cli_index_bootstrap.c: a remote MCP initialize only uploads the
 * canonical repository when the remote index does not already contain it. */
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_remote;
static int g_list_mode; /* 0 = present, 1 = absent, 2 = unavailable */
static int g_list_calls;
static int g_scan_calls;
static char g_scan_root[4096];

int cli_v1_remote_endpoint_is_network(void)
{
   return g_remote;
}

char *cli_v1_client_endpoint(void)
{
   return strdup("tcp:aimee.test:8743");
}

char *cli_v1_client_bearer(void)
{
   return strdup("test-bearer");
}

const char *cli_v1_route_for_method(const char *method, const char **verb_out)
{
   assert(strcmp(method, "index.list") == 0);
   if (verb_out)
      *verb_out = "POST";
   return "/v1/index/list";
}

cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body_json, const char *bearer, int timeout_ms, int *http_status)
{
   (void)endpoint;
   (void)method;
   (void)path;
   (void)body_json;
   (void)bearer;
   (void)timeout_ms;
   g_list_calls++;
   if (g_list_mode == 2)
   {
      if (http_status)
         *http_status = 503;
      return NULL;
   }

   if (http_status)
      *http_status = 200;
   cJSON *resp = cJSON_CreateObject();
   cJSON *projects = cJSON_AddArrayToObject(resp, "projects");
   if (g_list_mode == 0)
   {
      cJSON *project = cJSON_CreateObject();
      cJSON_AddStringToObject(project, "name", "tmp");
      cJSON_AddStringToObject(project, "root", "/tmp");
      cJSON_AddItemToArray(projects, project);
   }
   return resp;
}

int cli_index_scan_remote(int argc, char **argv)
{
   assert(argc == 1);
   assert(argv && argv[0]);
   g_scan_calls++;
   snprintf(g_scan_root, sizeof(g_scan_root), "%s", argv[0]);
   return 0;
}

#include "../cli_index_bootstrap.c"

static void reset(int mode)
{
   g_remote = 1;
   g_list_mode = mode;
   g_list_calls = 0;
   g_scan_calls = 0;
   g_scan_root[0] = '\0';
}

static void test_present_project_skips_upload(void)
{
   reset(0);
   assert(cli_index_ensure_remote("/tmp") == 0);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 0);
}

static void test_missing_project_uploads_canonical_root(void)
{
   reset(1);
   assert(cli_index_ensure_remote("/tmp") == 0);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 1);
   assert(strcmp(g_scan_root, "/tmp") == 0);
}

static void test_unavailable_list_does_not_start_doomed_upload(void)
{
   reset(2);
   assert(cli_index_ensure_remote("/tmp") == 1);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 0);
}

static void test_local_transport_is_untouched(void)
{
   reset(1);
   g_remote = 0;
   assert(cli_index_ensure_remote("/does/not/exist") == 0);
   assert(g_list_calls == 0);
   assert(g_scan_calls == 0);
}

int main(void)
{
   test_present_project_skips_upload();
   test_missing_project_uploads_canonical_root();
   test_unavailable_list_does_not_start_doomed_upload();
   test_local_transport_is_untouched();
   puts("cli_index_bootstrap: all tests passed");
   return 0;
}
