/* test_cli_index_bootstrap.c: a remote MCP initialize only uploads the
 * canonical repository when the remote index does not already contain it. */
#include "cJSON.h"
#include "platform_test_util.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_remote;
static int g_list_mode; /* 0 = present, 1 = absent, 2 = unavailable */
static int g_list_calls;
static int g_scan_calls;
static char g_scan_root[4096];
static char g_root[4096];
static int g_quiet, g_quiet_binds, g_scan_rc;

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
   if (g_list_mode == 0 || g_list_mode == 3)
   {
      cJSON *project = cJSON_CreateObject();
      cJSON_AddStringToObject(project, "name",
                              g_list_mode == 0 ? "bootstrap-owner" : "foreign-label");
      cJSON_AddStringToObject(project, "root", g_root);
      cJSON_AddItemToArray(projects, project);
   }
   return resp;
}

int cli_ws_output_quiet_bind(int quiet)
{
   int previous = g_quiet;
   g_quiet = quiet != 0;
   g_quiet_binds++;
   return previous;
}

int cli_index_scan_remote(int argc, char **argv)
{
   assert(argc == 1);
   assert(argv && argv[0]);
   assert(g_quiet == 1);
   g_scan_calls++;
   snprintf(g_scan_root, sizeof(g_scan_root), "%s", argv[0]);
   return g_scan_rc;
}

#include "../cli_index_bootstrap.c"

static void reset(int mode)
{
   g_remote = 1;
   g_list_mode = mode;
   g_list_calls = 0;
   g_scan_calls = 0;
   g_scan_root[0] = '\0';
   g_quiet = g_quiet_binds = g_scan_rc = 0;
}

static void test_present_project_skips_upload(void)
{
   reset(0);
   assert(cli_index_ensure_remote(g_root) == 0);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 0);
   assert(g_quiet == 0 && g_quiet_binds == 0);
}

static void test_missing_project_uploads_canonical_root(void)
{
   reset(1);
   assert(cli_index_ensure_remote(g_root) == 0);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 1);
   assert(g_quiet == 0 && g_quiet_binds == 2);
   assert(strcmp(g_scan_root, g_root) == 0);
}

static void test_unavailable_list_does_not_start_doomed_upload(void)
{
   reset(2);
   assert(cli_index_ensure_remote(g_root) == 1);
   assert(g_list_calls == 1);
   assert(g_scan_calls == 0);
   assert(g_quiet == 0 && g_quiet_binds == 0);
}

static void test_local_transport_is_untouched(void)
{
   reset(1);
   g_remote = 0;
   assert(cli_index_ensure_remote("/does/not/exist") == 0);
   assert(g_list_calls == 0);
   assert(g_scan_calls == 0);
   assert(g_quiet == 0 && g_quiet_binds == 0);
}

static void write_manifest(const char *identity)
{
   char path[8192];
   snprintf(path, sizeof path, "%s/aimee.workspace.yaml", g_root);
   FILE *f = fopen(path, "w");
   assert(f && fprintf(f, "id: %s\n", identity) > 0 && fclose(f) == 0);
}

static void run_owned(const char *const *argv)
{
   char *out = NULL;
   int rc = safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &out, 16384, 10000);
   free(out);
   assert(rc == 0);
}

static void test_wrong_index_label_does_not_replace_owner(void)
{
   reset(3);
   assert(cli_index_ensure_remote(g_root) == 0);
   assert(g_list_calls == 1 && g_scan_calls == 1);
   assert(!strcmp(g_scan_root, g_root));
}

static void test_invalid_manifest_refuses_before_index(void)
{
   write_manifest("invalid identity with spaces");
   reset(0);
   assert(cli_index_ensure_remote(g_root) == 1);
   assert(g_list_calls == 0 && g_scan_calls == 0);
   write_manifest("bootstrap-owner");
}

static void test_scan_failure_preserves_rc_and_render_scope(void)
{
   reset(1);
   g_scan_rc = 7;
   assert(cli_index_ensure_remote(g_root) == 7);
   assert(g_scan_calls == 1 && g_quiet_binds == 2 && g_quiet == 0);
   reset(1);
   g_quiet = 1;
   assert(cli_index_ensure_remote(g_root) == 0);
   assert(g_scan_calls == 1 && g_quiet_binds == 2 && g_quiet == 1);
}

int main(void)
{
   assert(snprintf(g_root, sizeof g_root, "%s/index-bootstrap.XXXXXX", platform_tmpdir()) <
          (int)sizeof g_root);
   assert(mkdtemp(g_root));
   run_owned((const char *const[]){"git", "init", "-q", g_root, NULL});
   write_manifest("bootstrap-owner");
   test_present_project_skips_upload();
   test_missing_project_uploads_canonical_root();
   test_unavailable_list_does_not_start_doomed_upload();
   test_local_transport_is_untouched();
   test_wrong_index_label_does_not_replace_owner();
   test_invalid_manifest_refuses_before_index();
   test_scan_failure_preserves_rc_and_render_scope();
   run_owned((const char *const[]){"rm", "-rf", "--", g_root, NULL});
   puts("cli_index_bootstrap: all tests passed");
   return 0;
}
