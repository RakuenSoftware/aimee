#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "config_client.h"
#include "mcp_osv_cache.h"
#include "aimee/protocols/mcp/mcp_client_registry.h"
#include "aimee/protocols/mcp/mcp_tools.h"
#include "agent_code_capabilities.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static const char *g_http_response;
static int g_http_status = -1;
static int g_http_calls;
static int g_cache_hit;
static db1_mcp_osv_cache_row_t g_cache_row;
static char g_last_audit_action[32];
static char g_last_audit_verdict[32];

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version,
                          int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)ttl_hours;
   if (!g_cache_hit)
      return 0;
   *out = g_cache_row;
   return 1;
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version,
                             const char *verdict, const char *advisory_ids)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   snprintf(g_cache_row.verdict, sizeof(g_cache_row.verdict), "%s", verdict);
   snprintf(g_cache_row.advisory_ids, sizeof(g_cache_row.advisory_ids), "%s",
            advisory_ids ? advisory_ids : "");
   return 0;
}

int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name,
                      const char *version, const char *verdict, const char *action,
                      const char *advisory_ids)
{
   (void)client_name;
   (void)ecosystem;
   (void)name;
   (void)version;
   snprintf(g_last_audit_verdict, sizeof(g_last_audit_verdict), "%s", verdict);
   snprintf(g_last_audit_action, sizeof(g_last_audit_action), "%s", action);
   (void)advisory_ids;
   return 0;
}

int http_retry_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers, int max_attempts, int base_ms,
                    int max_ms)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   (void)max_attempts;
   (void)base_ms;
   (void)max_ms;
   g_http_calls++;
   if (g_http_response)
   {
      size_t len = strlen(g_http_response);
      *response_buf = malloc(len + 1);
      assert(*response_buf != NULL);
      memcpy(*response_buf, g_http_response, len + 1);
   }
   else
      *response_buf = NULL;
   return g_http_status;
}

static void reset_osv_stub(void)
{
   g_http_response = NULL;
   g_http_status = -1;
   g_http_calls = 0;
   g_cache_hit = 0;
   memset(&g_cache_row, 0, sizeof(g_cache_row));
   g_last_audit_action[0] = '\0';
   g_last_audit_verdict[0] = '\0';
}

static int path_exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

static const char *mock_server_path(void)
{
#ifdef MCP_MOCK_SERVER_PATH
   if (path_exists(MCP_MOCK_SERVER_PATH))
      return MCP_MOCK_SERVER_PATH;
   static char root_path[256];
   if (MCP_MOCK_SERVER_PATH[0] != '/')
   {
      snprintf(root_path, sizeof(root_path), "src/%s", MCP_MOCK_SERVER_PATH);
      if (path_exists(root_path))
         return root_path;
   }
   return MCP_MOCK_SERVER_PATH;
#else
   if (path_exists("./build/obj/tests/mock-mcp-server"))
      return "./build/obj/tests/mock-mcp-server";
   return "./src/build/obj/tests/mock-mcp-server";
#endif
}

static void make_package_manager_link(char *dir, size_t dir_len, char *path, size_t path_len)
{
   snprintf(dir, dir_len, "%s/aimee-mcp-registry-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir) != NULL);
   snprintf(path, path_len, "%s/npx", dir);
   char target[512];
   const char *mock = mock_server_path();
   if (mock[0] == '/')
      snprintf(target, sizeof(target), "%s", mock);
   else
   {
      char cwd[256];
      assert(getcwd(cwd, sizeof(cwd)) != NULL);
      snprintf(target, sizeof(target), "%s/%s", cwd, mock);
   }
   assert(symlink(target, path) == 0);
}

static void remove_package_manager_link(const char *dir, const char *path)
{
   if (path && path[0])
      unlink(path);
   if (dir && dir[0])
      rmdir(dir);
}

typedef struct
{
   int mcp_client_count;
   config_mcp_client_t mcp_clients[CONFIG_MCP_MAX_CLIENTS];
   int mcp_osv_enabled;
   int mcp_osv_enforce;
   int mcp_osv_offline;
   int mcp_osv_cache_ttl_hours;
   char mcp_osv_endpoint[512];
   int mcp_osv_allow_count;
   char mcp_osv_allow[CONFIG_MCP_OSV_MAX_ALLOW][256];
} mcp_registry_fixture_t;

static mcp_registry_fixture_t cfg;

/* Publish what this case just filled in. Call after the last cfg.* write and
 * before booting the registry. */
static void publish_cfg(void)
{
   cJSON *clients = cJSON_CreateArray();
   for (int i = 0; i < cfg.mcp_client_count; i++)
   {
      cJSON *client = cJSON_CreateObject();
      cJSON_AddStringToObject(client, "name", cfg.mcp_clients[i].name);
      cJSON_AddStringToObject(client, "transport",
                              cfg.mcp_clients[i].transport == CONFIG_MCP_TRANSPORT_STDIO ? "stdio"
                              : cfg.mcp_clients[i].transport == CONFIG_MCP_TRANSPORT_SSE ? "sse"
                                                                                         : "");
      cJSON_AddStringToObject(
          client, "install", cfg.mcp_clients[i].install == CONFIG_MCP_INSTALL_KB ? "kb" : "server");
      cJSON *command = cJSON_AddArrayToObject(client, "command");
      for (int j = 0; j < cfg.mcp_clients[i].command_count; j++)
         cJSON_AddItemToArray(command, cJSON_CreateString(cfg.mcp_clients[i].command[j]));
      cJSON_AddStringToObject(client, "cwd", cfg.mcp_clients[i].cwd);
      cJSON_AddStringToObject(client, "url", cfg.mcp_clients[i].url);
      cJSON_AddStringToObject(client, "bearer_token_env", cfg.mcp_clients[i].bearer_token_env);
      cJSON_AddItemToArray(clients, client);
   }
   assert(config_client_set_value("mcp_clients", clients) == 0);
   assert(config_client_set_number("mcp_client_count", cfg.mcp_client_count) == 0);
   assert(config_client_set_number("mcp_osv_enabled", cfg.mcp_osv_enabled) == 0);
   assert(config_client_set_number("mcp_osv_enforce", cfg.mcp_osv_enforce) == 0);
   assert(config_client_set_number("mcp_osv_offline", cfg.mcp_osv_offline) == 0);
   assert(config_client_set_number("mcp_osv_cache_ttl_hours", cfg.mcp_osv_cache_ttl_hours) == 0);
   assert(config_client_set_string("mcp_osv_endpoint", cfg.mcp_osv_endpoint) == 0);
   cJSON *allow = cJSON_CreateArray();
   for (int i = 0; i < cfg.mcp_osv_allow_count; i++)
      cJSON_AddItemToArray(allow, cJSON_CreateString(cfg.mcp_osv_allow[i]));
   assert(config_client_set_value("mcp_osv_allow", allow) == 0);
   assert(config_client_set_number("mcp_osv_allow_count", cfg.mcp_osv_allow_count) == 0);
}

static void test_boot_and_lazy_tools(void)
{
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mock");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");

   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(mcp_client_registry_count() == 1);
   assert(strcmp(mcp_client_registry_name_at(0), "mock") == 0);
   assert(mcp_client_registry_get("mock") != NULL);

   cJSON *tools = NULL;
   char err[128] = "";
   assert(mcp_client_registry_list_tools("mock", 1000, &tools, err, sizeof(err)) == 0);
   assert(tools != NULL);
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(tools, "tools");
   assert(cJSON_IsArray(arr));
   cJSON *first = cJSON_GetArrayItem(arr, 0);
   cJSON *name = cJSON_GetObjectItemCaseSensitive(first, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "echo") == 0);
   cJSON_Delete(tools);

   mcp_client_registry_shutdown();
   assert(mcp_client_registry_count() == 0);
   assert(mcp_client_registry_get("mock") == NULL);
}

/* A daemon boots ONLY the plugins whose install target it hosts: a server boots
 * install:server clients, a kb boots install:kb clients, never the other's. */
static void test_install_target_filtering(void)
{
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 2;
   /* client 0: server-hosted */
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "srv");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].install = CONFIG_MCP_INSTALL_SERVER;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");
   /* client 1: kb-hosted (same mock binary) */
   snprintf(cfg.mcp_clients[1].name, sizeof(cfg.mcp_clients[1].name), "%s", "shared");
   cfg.mcp_clients[1].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[1].install = CONFIG_MCP_INSTALL_KB;
   cfg.mcp_clients[1].command_count = 2;
   snprintf(cfg.mcp_clients[1].command[0], sizeof(cfg.mcp_clients[1].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[1].command[1], sizeof(cfg.mcp_clients[1].command[1]), "%s", "happy");

   /* Boot as the server: only the install:server plugin starts. */
   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(mcp_client_registry_count() == 1);
   assert(mcp_client_registry_get("srv") != NULL);
   assert(mcp_client_registry_get("shared") == NULL);
   mcp_client_registry_shutdown();

   /* Boot as the kb: only the install:kb plugin starts. */
   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_KB) == 1);
   assert(mcp_client_registry_count() == 1);
   assert(mcp_client_registry_get("shared") != NULL);
   assert(mcp_client_registry_get("srv") == NULL);
   mcp_client_registry_shutdown();
}

static void test_namespaced_tools_and_dispatch(void)
{
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mock");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");

   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);

   cJSON *remote_tools = mcp_client_registry_build_namespaced_tools(1000);
   assert(cJSON_IsArray(remote_tools));
   assert(cJSON_GetArraySize(remote_tools) == 3);
   cJSON *first = cJSON_GetArrayItem(remote_tools, 0);
   cJSON *name = cJSON_GetObjectItemCaseSensitive(first, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "mock:echo") == 0);
   cJSON_Delete(remote_tools);

   cJSON *schema = NULL;
   char err[128] = "";
   assert(mcp_client_registry_get_tool_schema("mock:bash", 1000, &schema, err, sizeof(err)) == 0);
   assert(schema != NULL);
   name = cJSON_GetObjectItemCaseSensitive(schema, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "mock:bash") == 0);
   cJSON_Delete(schema);

   cJSON *result = NULL;
   assert(mcp_client_registry_call_tool("mock:echo", NULL, 1000, &result, err, sizeof(err)) == 0);
   assert(result != NULL);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   cJSON *text = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text");
   assert(cJSON_IsString(text));
   assert(strcmp(text->valuestring, "hello from mock") == 0);
   cJSON_Delete(result);

   assert(mcp_client_registry_call_tool("mock:fail", NULL, 1000, &result, err, sizeof(err)) == -1);
   assert(strstr(err, "mock failure") != NULL);

   cJSON *public_tools = mcp_build_tools_list();
   assert(cJSON_IsArray(public_tools));
   int saw_namespaced = 0;
   int saw_delegate_background_absent = 0;
   int saw_delegate_cwd = 0;
   int saw_delegate_status = 0;
   int saw_roundtable_review = 0;
   int saw_roundtable_status_absent = 1;
   int saw_job_family = 0;
   int saw_skill_manage = 0;
   int saw_skill_manage_cwd = 0;
   int saw_git_path_is_universal = 0;
   int saw_git_fetch_is_remote_tracking_only = 0;
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, public_tools)
   {
      cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "mock:echo") == 0)
         saw_namespaced = 1;
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "delegate_status") == 0)
         saw_delegate_status = 1;
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "roundtable_review") == 0)
         saw_roundtable_review = 1;
      /* roundtable_review blocks and returns the verdict, so there is no poller
       * to advertise -- and advertising one is what taught agents to spend a
       * model turn per poll on a job that runs for minutes. */
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "roundtable_status") == 0)
         saw_roundtable_status_absent = 0;
      /* job_start/job_status were collapsed into the `job` family (P4b); the
       * member description no longer appears standalone. Verify the family tool. */
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "job") == 0)
         saw_job_family = 1;
      /* The git tool resolves WHICH repository it acts on from args["path"]
       * (mcp_chdir_git_root's priority-1 candidate). That param was described as
       * "clone: local path; verify: repo path", so a schema-driven caller never
       * passed it for status/commit/push and silently got whatever session state
       * resolved to — in a worktree-isolated session, the SHARED checkout on another
       * branch, where a commit stages someone else's work. The description has to
       * say the param selects the repository for EVERY command. */
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "git") == 0)
      {
         cJSON *tool_desc = cJSON_GetObjectItemCaseSensitive(tool, "description");
         cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
         cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
         cJSON *path = cJSON_GetObjectItemCaseSensitive(props, "path");
         cJSON *desc = cJSON_GetObjectItemCaseSensitive(path, "description");
         if (cJSON_IsString(desc) && strstr(desc->valuestring, "every command") &&
             strstr(desc->valuestring, "SHARED checkout"))
            saw_git_path_is_universal = 1;
         cJSON *prune = cJSON_GetObjectItemCaseSensitive(props, "prune");
         cJSON *prune_desc = cJSON_GetObjectItemCaseSensitive(prune, "description");
         if (cJSON_IsString(tool_desc) &&
             strstr(tool_desc->valuestring, "writes/prunes only refs/remotes/<remote>/*") &&
             cJSON_IsString(prune_desc) &&
             strstr(prune_desc->valuestring, "local branches are never"))
            saw_git_fetch_is_remote_tracking_only = 1;
      }
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "skill_manage") == 0)
      {
         saw_skill_manage = 1;
         cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
         cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
         cJSON *cwd = cJSON_GetObjectItemCaseSensitive(props, "cwd");
         cJSON *cwd_type = cJSON_GetObjectItemCaseSensitive(cwd, "type");
         if (cJSON_IsString(cwd_type) && strcmp(cwd_type->valuestring, "string") == 0)
            saw_skill_manage_cwd = 1;
      }
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "delegate") == 0)
      {
         cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
         cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
         /* (WP-B) delegates are always async; the tool no longer exposes a
          * `background` param. */
         if (!cJSON_GetObjectItemCaseSensitive(props, "background"))
            saw_delegate_background_absent = 1;
         cJSON *cwd = cJSON_GetObjectItemCaseSensitive(props, "cwd");
         cJSON *cwd_type = cJSON_GetObjectItemCaseSensitive(cwd, "type");
         if (cJSON_IsString(cwd_type) && strcmp(cwd_type->valuestring, "string") == 0)
            saw_delegate_cwd = 1;
      }
   }
   assert(saw_namespaced);
   assert(saw_delegate_background_absent);
   assert(saw_delegate_cwd);
   assert(saw_delegate_status);
   assert(saw_roundtable_review);
   assert(saw_roundtable_status_absent);
   assert(saw_job_family);
   assert(saw_skill_manage);
   assert(saw_skill_manage_cwd);
   assert(saw_git_path_is_universal);
   assert(saw_git_fetch_is_remote_tracking_only);
   cJSON_Delete(public_tools);

   mcp_client_registry_shutdown();
}

static void test_failed_client_does_not_abort_boot(void)
{
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 2;

   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "missing");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 1;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            "/definitely/not/a/real/mcp-server");

   snprintf(cfg.mcp_clients[1].name, sizeof(cfg.mcp_clients[1].name), "%s", "mock");
   cfg.mcp_clients[1].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[1].command_count = 2;
   snprintf(cfg.mcp_clients[1].command[0], sizeof(cfg.mcp_clients[1].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[1].command[1], sizeof(cfg.mcp_clients[1].command[1]), "%s", "happy");

   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(mcp_client_registry_count() == 1);
   assert(mcp_client_registry_get("missing") == NULL);
   assert(mcp_client_registry_get("mock") != NULL);

   mcp_client_registry_shutdown();
}

static void test_osv_gate_blocks_malware(void)
{
   reset_osv_stub();
   g_http_status = 200;
   g_http_response = "{\"vulns\":[{\"id\":\"MAL-2026-1\"}]}";
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_osv_enabled = 1;
   cfg.mcp_osv_enforce = 1;
   cfg.mcp_osv_cache_ttl_hours = 24;
   snprintf(cfg.mcp_osv_endpoint, sizeof(cfg.mcp_osv_endpoint), "%s", "https://example.invalid");
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mal");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s", "npx");
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "bad-pkg");

   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 0);
   assert(mcp_client_registry_count() == 0);
   assert(g_http_calls == 1);
   assert(strcmp(g_last_audit_verdict, "malware") == 0);
   assert(strcmp(g_last_audit_action, "block") == 0);
   mcp_client_registry_shutdown();
}

static void test_osv_gate_shadow_and_allowlist_allow(void)
{
   char dir[128], npx_path[160];
   make_package_manager_link(dir, sizeof(dir), npx_path, sizeof(npx_path));

   reset_osv_stub();
   g_http_status = 200;
   g_http_response = "{\"vulns\":[{\"id\":\"MAL-2026-2\"}]}";
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_osv_enabled = 1;
   cfg.mcp_osv_enforce = 0;
   cfg.mcp_osv_cache_ttl_hours = 24;
   snprintf(cfg.mcp_osv_endpoint, sizeof(cfg.mcp_osv_endpoint), "%s", "https://example.invalid");
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "shadow");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s", npx_path);
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "bad-pkg");
   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(strcmp(g_last_audit_action, "shadow_block") == 0);
   mcp_client_registry_shutdown();

   reset_osv_stub();
   g_cache_hit = 1;
   snprintf(g_cache_row.verdict, sizeof(g_cache_row.verdict), "%s", "malware");
   snprintf(g_cache_row.advisory_ids, sizeof(g_cache_row.advisory_ids), "%s", "MAL-2026-2");
   cfg.mcp_osv_enforce = 1;
   cfg.mcp_osv_allow_count = 1;
   snprintf(cfg.mcp_osv_allow[0], sizeof(cfg.mcp_osv_allow[0]), "%s", "npm:bad-pkg");
   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(g_http_calls == 0);
   assert(strcmp(g_last_audit_action, "allow_allowlisted") == 0);
   mcp_client_registry_shutdown();

   remove_package_manager_link(dir, npx_path);
}

static void test_osv_offline_cache_miss_allows(void)
{
   char dir[128], npx_path[160];
   make_package_manager_link(dir, sizeof(dir), npx_path, sizeof(npx_path));
   reset_osv_stub();
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_osv_enabled = 1;
   cfg.mcp_osv_enforce = 1;
   cfg.mcp_osv_offline = 1;
   cfg.mcp_osv_cache_ttl_hours = 24;
   snprintf(cfg.mcp_osv_endpoint, sizeof(cfg.mcp_osv_endpoint), "%s", "https://example.invalid");
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "offline");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s", npx_path);
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "pkg");

   publish_cfg();
   assert(mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER) == 1);
   assert(g_http_calls == 0);
   assert(strcmp(g_last_audit_verdict, "unknown") == 0);
   assert(strcmp(g_last_audit_action, "allow") == 0);
   mcp_client_registry_shutdown();
   remove_package_manager_link(dir, npx_path);
}

/* test_mcp_tools_golden.inc: AUTO-GENERATED golden for mcp_build_tools_list()'s
 * built-in tool surface (name + sorted schema property keys + required), captured
 * via the DUMP_TOOLS path in test_mcp_client_registry.c. Regenerate after an
 * intentional tool change: DUMP_TOOLS=1 ./unit-test-mcp-client-registry 2>&1. */
#define MCP_TOOLS_GOLDEN_COUNT 54
#define MCP_TOOLS_GOLDEN                                                                                                                                                                                                                                                           \
   "ask_user {choices,question} req:question\n"                                                                                                                                                                                                                                    \
   "ast_grep_search {lang,path,pattern} req:lang,pattern\n"                                                                                                                                                                                                                        \
   "attempt {approach,command,filter,lesson,outcome,task_context} req:command\n"                                                                                                                                                                                                   \
   "autopilot {action,job_id,pipeline_id,plan_depth,plan_id,task} req:action\n"                                                                                                                                                                                                    \
   "background {action,command,cwd,id,tail_lines} req:action\n"                                                                                                                                                                                                                    \
   "call_tool {arguments,name} req:arguments,name\n"                                                                                                                                                                                                                               \
   "clarify {answer,command,description,session_id} req:command\n"                                                                                                                                                                                                                 \
   "dashboard_metrics {} req:\n"                                                                                                                                                                                                                                                   \
   "delegate {branch,cwd,handoff_json,persona,prompt,role,scope,tools,via} req:persona,prompt,role\n"                                                                                                                                                                              \
   "delegate_reply {content,delegation_id} req:content,delegation_id\n"                                                                                                                                                                                                            \
   "delegate_status {job_id} req:job_id\n"                                                                                                                                                                                                                                         \
   "describe_tool {name} req:name\n"                                                                                                                                                                                                                                               \
   "diagnose {command,content,diagnosis_id,hypothesis_id,rank,source,stance,symptom} req:command\n"                                                                                                                                                                                \
   "ensemble {assignments,channel,command,id,limit,message,reason,speaker,template} req:command\n"                                                                                                                                                                                 \
   "epistemic_directive {anchor_entity,anchor_file,cause,command,id,limit,note,priority,question,resolution_memory_id,state,suppress,topic,valid_until} req:command\n"                                                                                                             \
   "find_symbol {identifier,identifiers,project,scope} req:\n"                                                                                                                                                                                                                     \
   "find_tools {limit,query} req:\n"                                                                                                                                                                                                                                               \
   "get_help {topic} req:\n"                                                                                                                                                                                                                                                       \
   "get_identity {} req:\n"                                                                                                                                                                                                                                                        \
   "git {abort_on_conflict,action,all,async,auto,base,body,branch,command,count,depth,diff_stat,expected_head_sha,files,force,index,job_id,merge_method,message,mirror,mode,name,number,path,prune,rebase,ref,remote,source,staged,stat_only,state,title,url,wait} req:command\n"  \
   "graph {command,cwd,entity,episode_key,limit,project,query,scope,workspace} req:command\n"                                                                                                                                                                                      \
   "host {command,name} req:command\n"                                                                                                                                                                                                                                             \
   "index {command,fallback,file_path,file_paths,include_code,judge,line_end,line_start,max_results,node,paths,project,queries,query,scope,spans,symbol,symbols} req:command\n"                                                                                                    \
   "job {command,job_id,max_concurrent,plan_id} req:command\n"                                                                                                                                                                                                                     \
   "learning {command,correction_text,description,evidence_refs,limit,polarity,signal_type,sink,state,target_key,target_memory_id,title,workflow_project,workflow_signal_type} req:command\n"                                                                                      \
   "list_curiosity_items {limit,state} req:\n"                                                                                                                                                                                                                                     \
   "lsp {col,command,file,line,workspace} req:command\n"                                                                                                                                                                                                                           \
   "memory {as_of,command,confidence,content,cwd,dry_run,force,handle,id,key,kind,memory_id,modes,project,query,reason,scope,tier,verb,workspace} req:command\n"                                                                                                                   \
   "memory_recall {cwd,limit_tokens,project,scope,session_start,task_hint,workspace} req:\n"                                                                                                                                                                                       \
   "note {command,content,limit,query,tag,tags,title} req:command\n"                                                                                                                                                                                                               \
   "payload_rewrite_status {} req:\n"                                                                                                                                                                                                                                              \
   "pdf_inspect_structure {document_key,project} req:document_key,project\n"                                                                                                                                                                                                       \
   "pdf_list_assets {document_key,project} req:document_key,project\n"                                                                                                                                                                                                             \
   "pdf_lookup_table {document_key,page_no,project} req:document_key,project\n"                                                                                                                                                                                                    \
   "pdf_open_asset {asset_id,project} req:asset_id,project\n"                                                                                                                                                                                                                      \
   "pdf_open_neighbors {chunk_id,project} req:chunk_id,project\n"                                                                                                                                                                                                                  \
   "pdf_open_page {document_key,page_no,project} req:document_key,page_no,project\n"                                                                                                                                                                                               \
   "pdf_search_chunks {max_results,project,query} req:project,query\n"                                                                                                                                                                                                             \
   "peer {command,conversation_id,expect_reply,max,text,to} req:command\n"                                                                                                                                                                                                         \
   "pipeline {artifact,base_branch,brief,command,done_bar,head_branch,idea,operator_principal,pipeline_id,questions,reason,remote,repo_root,state,verdict,worktree_path} req:command\n"                                                                                            \
   "preview_blast_radius {paths,project,scope} req:paths\n"                                                                                                                                                                                                                        \
   "prospective_memory {action_text,anchor_entity,anchor_file,command,id,limit,recurrence,state,trigger_text,valid_until} req:command\n"                                                                                                                                           \
   "recall {block_type,command,cwd,limit,limit_tokens,project,query,scope,since,workspace} req:command\n"                                                                                                                                                                          \
   "roadmap {command,roadmap_id} req:command\n"                                                                                                                                                                                                                                    \
   "roundtable_review {artifact_stage,brief,diff,original_request,roundtable,workdir} req:diff,original_request\n"                                                                                                                                                                 \
   "rules {command,reason,text} req:command\n"                                                                                                                                                                                                                                     \
   "search_docs {cwd,max_results,project,query,scope} req:query\n"                                                                                                                                                                                                                 \
   "search_memory {cwd,filter,project,query,scope,workspace} req:query\n"                                                                                                                                                                                                          \
   "send_message {target,text} req:target,text\n"                                                                                                                                                                                                                                  \
   "session {around_message_id,chain_id,command,include_sources,limit,query,session_id,window} req:command\n"                                                                                                                                                                      \
   "skill_manage {absorbed_into,action,content,cwd,file_path,name,new_string,old_string,replace_all} req:action,name\n"                                                                                                                                                            \
   "store_workflow {project,rule,signal_type} req:rule,signal_type\n"                                                                                                                                                                                                              \
   "task_list {limit,session_id,state} req:\n"                                                                                                                                                                                                                                     \
   "workflow_run {proposal_md,repo,workflow} req:proposal_md\n"

/* --- mcp_build_tools_list surface net ---
 * Pins the full set of built-in tools and each tool's schema SHAPE (sorted
 * property keys + required) so mcp_build_tools_list() can be decomposed into
 * per-tool builders without silently dropping/renaming a tool or altering a
 * schema. Robust to description-text edits. Run before any registry boot so
 * the list is the clean built-in set (no namespaced remote tools). */
static int tools_cmp_ptr(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int tools_cmp_row(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}
static void tool_signature(cJSON *tool, char *out, size_t outsz)
{
   cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
   const char *keys[96];
   int nk = 0;
   cJSON *c = NULL;
   if (cJSON_IsObject(props))
      cJSON_ArrayForEach(c, props) if (c->string && nk < 96) keys[nk++] = c->string;
   qsort(keys, (size_t)nk, sizeof(keys[0]), tools_cmp_ptr);
   const char *rq[48];
   int nr = 0;
   if (cJSON_IsArray(req))
      cJSON_ArrayForEach(c, req) if (cJSON_IsString(c) && nr < 48) rq[nr++] = c->valuestring;
   qsort(rq, (size_t)nr, sizeof(rq[0]), tools_cmp_ptr);
   /* Clamped appends: snprintf returns what it WOULD have written, so a signature
    * wider than the buffer would otherwise walk past it (and, before the clamp,
    * silently truncate away the `req:` half — a tool could lose a required
    * parameter and this golden would not notice). */
   int o = snprintf(out, outsz, "%s {", cJSON_IsString(name) ? name->valuestring : "?");
   if (o > (int)outsz - 1)
      o = (int)outsz - 1;
#define SIG_APPEND(...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      int n_ = snprintf(out + o, outsz - (size_t)o, __VA_ARGS__);                                  \
      o = (n_ < 0 || o + n_ > (int)outsz - 1) ? (int)outsz - 1 : o + n_;                           \
   } while (0)
   for (int i = 0; i < nk; i++)
      SIG_APPEND("%s%s", i ? "," : "", keys[i]);
   SIG_APPEND("} req:");
   for (int i = 0; i < nr; i++)
      SIG_APPEND("%s%s", i ? "," : "", rq[i]);
#undef SIG_APPEND
   assert(o < (int)outsz - 1); /* a truncated signature is not a check */
}
static void test_tools_list_surface(void)
{
   cJSON *tools = mcp_build_tools_list();
   assert(cJSON_IsArray(tools));
   static char sigs[256][512];
   int ns = 0;
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      assert(ns < 256);
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
      cJSON *ds = cJSON_GetObjectItemCaseSensitive(tool, "description");
      cJSON *sc = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
      assert(cJSON_IsString(nm) && nm->valuestring[0]);
      assert(cJSON_IsString(ds) && ds->valuestring[0]);
      assert(cJSON_IsObject(sc));
      tool_signature(tool, sigs[ns], sizeof(sigs[ns]));
      ns++;
   }
   qsort(sigs, (size_t)ns, sizeof(sigs[0]), tools_cmp_row);
   static char joined[256 * 512];
   int o = 0;
   for (int i = 0; i < ns; i++)
      o += snprintf(joined + o, sizeof(joined) - (size_t)o, "%s\n", sigs[i]);
   if (getenv("DUMP_TOOLS"))
      fprintf(stderr, "---GOLDEN(%d)---\n%s---END---\n", ns, joined);
   else
   {
      assert(ns == MCP_TOOLS_GOLDEN_COUNT);
      assert(strcmp(joined, MCP_TOOLS_GOLDEN) == 0);
   }
   cJSON_Delete(tools);
}

/* P1/P2 presentation profile: "core"/"lean" (the P2 default) shrinks the served
 * tools/list to the Tier-0 set; explicit "full"/unknown fail open. */
static int profile_core_has(const char *n, const char *const *set)
{
   for (int i = 0; set[i]; i++)
      if (strcmp(n, set[i]) == 0)
         return 1;
   return 0;
}
static void test_tool_profile_filter(void)
{
   /* Mirror of MCP_CORE_TOOLS in mcp_tool_profile.c — kept in sync intentionally.
    * Includes the P2 discovery meta-tools and schema-bound dispatch bridge. */
   static const char *const core[] = {"get_help",
                                      "find_tools",
                                      "describe_tool",
                                      "call_tool",
                                      "search_docs",
                                      "search_memory",
                                      "memory_recall",
                                      "get_identity",
                                      "find_symbol",
                                      "ast_grep_search",
                                      "preview_blast_radius",
                                      "index",
                                      "git",
                                      "delegate",
                                      "delegate_status",
                                      "roundtable_review",
                                      "ask_user",
                                      "send_message",
                                      "note",
                                      "peer",
                                      NULL};
   int expect = 0;
   for (int i = 0; core[i]; i++)
      expect++;
   assert(profile_core_has("find_tools", core) && profile_core_has("describe_tool", core));
   assert(profile_core_has("call_tool", core));

   /* THE MIRROR ABOVE LISTED THESE BEFORE THE REAL FLOOR DID.
    *
    * roundtable_review was in this array but NOT in
    * MCP_CORE_TOOLS, and nothing compared the two, so the drift went unnoticed.
    * Measured consequence on am_b84c9294aa: 74 tool calls, the skill telling the
    * agent to get a review before reporting done, and roundtable never invoked --
    * reaching it cost find_tools -> describe_tool -> call_tool. The agent shipped
    * a one-file caller-side fix against a reference that changes four files
    * elsewhere. Assert both are actually served, not merely mirrored. */
   {
      cJSON *served = mcp_build_tools_list();
      int have_review = 0, have_status = 0;
      cJSON *t = NULL;
      cJSON_ArrayForEach(t, served)
      {
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
         if (!cJSON_IsString(nm))
            continue;
         if (strcmp(nm->valuestring, "roundtable_review") == 0)
            have_review = 1;
         if (strcmp(nm->valuestring, "roundtable_status") == 0)
            have_status = 1;
      }
      cJSON_Delete(served);
      assert(have_review);
      /* roundtable_review blocks and returns the verdict; a status tool on the
       * surface is an invitation to poll a call that has already finished. */
      assert(!have_status);
   }

   /* THE SAME CHECK FOR peer, FOR THE SAME REASON THE COMMENT ABOVE GIVES.
    *
    * Peer messaging shipped served-but-unreachable once already: four bus stages
    * nothing called, then two MCP tools that worked and were not on the floor. A
    * name in the mirror above proves only that this array was edited. What has
    * to be true is that a client asking for the default profile is SHOWN it, and
    * that both operations are reachable through the one entry -- a family whose
    * enum lost a member is a capability silently withdrawn.
    *
    * There is no fallback here. An agent not shown this does not reach another
    * session by some slower route; it does not reach one at all. */
   {
      cJSON *served = mcp_build_tools_list();
      cJSON *peer = NULL, *t = NULL;
      cJSON_ArrayForEach(t, served)
      {
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
         if (cJSON_IsString(nm) && strcmp(nm->valuestring, "peer") == 0)
            peer = t;
      }
      assert(peer && "the peer family is not on the served surface at all");

      cJSON *schema = cJSON_GetObjectItemCaseSensitive(peer, "inputSchema");
      cJSON *props = schema ? cJSON_GetObjectItemCaseSensitive(schema, "properties") : NULL;
      cJSON *cmd = props ? cJSON_GetObjectItemCaseSensitive(props, "command") : NULL;
      cJSON *en = cmd ? cJSON_GetObjectItemCaseSensitive(cmd, "enum") : NULL;
      int has_send = 0, has_inbox = 0;
      cJSON *e = NULL;
      cJSON_ArrayForEach(e, en)
      {
         if (!cJSON_IsString(e))
            continue;
         if (strcmp(e->valuestring, "send") == 0)
            has_send = 1;
         if (strcmp(e->valuestring, "inbox") == 0)
            has_inbox = 1;
      }
      assert(has_send && "peer command=send is not offered");
      assert(has_inbox && "peer command=inbox is not offered");

      /* The parameters each operation needs must survive the fold. The family
         unions its members' properties, so a member that failed to attach takes
         its arguments with it and leaves a command that cannot be called. */
      assert(cJSON_GetObjectItemCaseSensitive(props, "to"));
      assert(cJSON_GetObjectItemCaseSensitive(props, "text"));
      assert(cJSON_GetObjectItemCaseSensitive(props, "conversation_id"));
      assert(cJSON_GetObjectItemCaseSensitive(props, "expect_reply"));
      assert(cJSON_GetObjectItemCaseSensitive(props, "max"));

      /* And the demux resolves both commands to real handlers. A family entry
         whose command maps to a name mcp_tool_lookup does not know is an advert
         for a tool that cannot run. */
      char resolved[96];
      cJSON *args = cJSON_CreateObject();
      cJSON_AddStringToObject(args, "command", "send");
      assert(mcp_family_demux("peer", args, resolved, sizeof resolved) == 1);
      assert(strcmp(resolved, "peer_send") == 0);
      cJSON_ReplaceItemInObjectCaseSensitive(args, "command", cJSON_CreateString("inbox"));
      assert(mcp_family_demux("peer", args, resolved, sizeof resolved) == 1);
      assert(strcmp(resolved, "peer_inbox") == 0);
      /* An unknown command must RESOLVE TO NOTHING rather than fall through to a
         default. -1 is "this family does not serve that", which the caller turns
         into a refusal; a 0 here would mean "not a family at all" and send the
         call somewhere else entirely. */
      cJSON_ReplaceItemInObjectCaseSensitive(args, "command", cJSON_CreateString("reply"));
      assert(mcp_family_demux("peer", args, resolved, sizeof resolved) == -1);
      cJSON_Delete(args);
      cJSON_Delete(served);
   }

   /* An asynchronous tool whose poller is NOT core costs the agent a
    * find_tools -> describe_tool -> call_tool detour before it can read the
    * result of a call it was told to make. Measured on a real cell: five of
    * fourteen tool calls went on reaching delegate_status. delegate must ship
    * delegate_status on those grounds for as long as delegate stays
    * asynchronous. roundtable took the other road: the review itself now
    * blocks, so it needs no poller in the floor at all. */
   assert(profile_core_has("delegate_status", core));

   /* The retrieval an agent reaches for when the question is NOT a symbol name.
    * Withholding `index` did not reduce retrieval; it moved it to a recursive
    * shell search, because that was one visible call while index_hybrid cost a
    * find_tools -> describe_tool -> call_tool detour. Measured across the
    * benchmark's aimee cells: 87 shell searches emitting 2.4 MB, one large
    * enough to hit the client's 1 MB output truncation.
    *
    * THIS ASSERTION WAS INVERTED ONCE, ON 2026-08-12, and inverted back the same
    * night. The argument was that the fallback had changed -- `aimee index ...`
    * now exists as a chainable command named in the standing guidance -- so the
    * detour was no longer the alternative. Re-tested with the commands live and
    * the guidance naming them, n=3 each side on a healthy box: CLI invocations
    * went DOWN 2.3 -> 1.3 per cell, MCP calls 4.3 -> 6.3, searches 3 -> 4.3,
    * search output 4.2 KB -> 6.1 KB, credits 15.52 -> 19.15 mean (+23%). The
    * calls it did make were find_tools x4, describe_tool x2, call_tool x1: the
    * detour, paid in full.
    *
    * So the original finding survives a change that looked like it should have
    * retired it. A schema present in every request outcompetes a sentence
    * recommending a command, and removing the schema sends the agent to
    * discovery and to grep rather than to the command. */
   assert(profile_core_has("index", core));
   assert(profile_core_has("delegate_status", core));
   {
      cJSON *listed = mcp_build_tools_list();
      int found_delegate = 0, found_status = 0;
      cJSON *entry = NULL;
      cJSON_ArrayForEach(entry, listed)
      {
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(entry, "name");
         if (!cJSON_IsString(nm))
            continue;
         if (strcmp(nm->valuestring, "delegate") == 0)
            found_delegate = 1;
         if (strcmp(nm->valuestring, "delegate_status") == 0)
            found_status = 1;
      }
      assert(found_delegate);
      assert(found_status);
      cJSON_Delete(listed);
   }

   /* Control the env so the default is deterministic across CI runners. */
   unsetenv("AIMEE_MCP_TOOL_PROFILE");

   /* Explicit "full" and an unknown profile are no-ops (fail open). */
   cJSON *t = mcp_build_tools_list();
   int full = cJSON_GetArraySize(t);
   assert(full > expect);
   assert(mcp_filter_tools_for_profile(t, "full", 1) == 0 && cJSON_GetArraySize(t) == full);
   assert(mcp_filter_tools_for_profile(t, "nonsense", 1) == 0 && cJSON_GetArraySize(t) == full);
   cJSON_Delete(t);

   /* P2 default: env unset + NULL profile resolves to "core" and filters. */
   assert(strcmp(mcp_tool_profile_effective(NULL), "core") == 0);
   t = mcp_build_tools_list();
   assert(mcp_filter_tools_for_profile(t, NULL, 1) == full - expect);
   assert(cJSON_GetArraySize(t) == expect);
   cJSON_Delete(t);

   /* "core" keeps exactly the Tier-0 set — and every named core tool exists in
    * the built list (kept count == expected count proves none was dropped). */
   t = mcp_build_tools_list();
   int removed = mcp_filter_tools_for_profile(t, "core", 1);
   assert(removed == full - expect);
   assert(cJSON_GetArraySize(t) == expect);
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, t)
   {
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
      assert(cJSON_IsString(nm) && profile_core_has(nm->valuestring, core));
   }
   cJSON_Delete(t);

   /* "lean" is an alias for "core". */
   t = mcp_build_tools_list();
   assert(mcp_filter_tools_for_profile(t, "lean", 1) == full - expect);
   cJSON_Delete(t);
}

static void test_call_tool_demux(void)
{
   const char *target = NULL;
   cJSON *target_args = NULL;
   cJSON *wrapper = cJSON_CreateObject();
   cJSON_AddStringToObject(wrapper, "name", "index");
   cJSON *nested = cJSON_AddObjectToObject(wrapper, "arguments");
   cJSON_AddStringToObject(nested, "command", "find_callers");
   cJSON_AddStringToObject(nested, "symbol", "end_of_month");

   assert(mcp_call_tool_demux("find_tools", wrapper, &target, &target_args) == 0);
   assert(mcp_call_tool_demux("call_tool", wrapper, &target, &target_args) == 1);
   assert(strcmp(target, "index") == 0);
   assert(target_args == nested);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(target_args, "symbol")->valuestring,
                 "end_of_month") == 0);
   cJSON_Delete(wrapper);

   wrapper = cJSON_CreateObject();
   cJSON_AddStringToObject(wrapper, "name", "call_tool");
   cJSON_AddObjectToObject(wrapper, "arguments");
   assert(mcp_call_tool_demux("call_tool", wrapper, &target, &target_args) == -1);
   cJSON_Delete(wrapper);

   wrapper = cJSON_CreateObject();
   cJSON_AddStringToObject(wrapper, "name", "index");
   assert(mcp_call_tool_demux("call_tool", wrapper, &target, &target_args) == -1);
   cJSON_Delete(wrapper);
}

static int tools_have(cJSON *tools, const char *name)
{
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         return 1;
   }
   return 0;
}

static cJSON *tools_get(cJSON *tools, const char *name)
{
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         return t;
   }
   return NULL;
}

static int schema_has_property(cJSON *tool, const char *name)
{
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   return cJSON_GetObjectItemCaseSensitive(properties, name) != NULL;
}

static cJSON *schema_property(cJSON *tool, const char *name)
{
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   return cJSON_GetObjectItemCaseSensitive(properties, name);
}

static int schema_requires(cJSON *tool, const char *name)
{
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, required) if (cJSON_IsString(item) &&
                                          strcmp(item->valuestring, name) == 0) return 1;
   return 0;
}

/* E1 contract: the words installed guidance gives an agent must map to a direct
 * lean tool, and every code-navigation schema must admit active-project defaults
 * plus an explicit cross-project escape hatch. */
/* get_help's description names its topics so the model reads them from
 * tools/list instead of spending a call on the index. That is only safe while
 * every name is a real section, so check them against the document itself. */
static void test_get_help_topics_exist(void)
{
   static const char *const topics[] = {
       "MCP Tools",    "Delegate",    "Memory CLI",  "Code Index",  "Verification",
       "Build & Test", "PR Workflow", "Conventions", "Diagnostics", NULL};
   cJSON *tools = mcp_build_tools_list_flat();
   cJSON *help = tools_get(tools, "get_help");
   assert(help != NULL);
   cJSON *desc = cJSON_GetObjectItemCaseSensitive(help, "description");
   assert(cJSON_IsString(desc));

   /* Every advertised topic must appear in the description ... */
   for (int i = 0; topics[i]; i++)
      assert(strstr(desc->valuestring, topics[i]) != NULL);

   /* ... and the description must not send the agent to fetch the index first,
    * which is the round trip this change removes. */
   assert(strstr(desc->valuestring, "CALL THIS TOOL FIRST") == NULL);

   /* ... and every topic must be a real "## " section of the document the help
    * data is generated from (src/Makefile: agent_help_data.h <- ../docs/agent.md). */
   FILE *fh = fopen("docs/agent.md", "re");
   if (!fh)
      fh = fopen("../docs/agent.md", "re");
   assert(fh != NULL);
   {
      char buf[16384];
      size_t got = fread(buf, 1, sizeof(buf) - 1, fh);
      buf[got] = '\0';
      fclose(fh);
      for (int i = 0; topics[i]; i++)
      {
         char heading[128];
         snprintf(heading, sizeof(heading), "## %s", topics[i]);
         assert(strstr(buf, heading) != NULL);
      }
   }
   cJSON_Delete(tools);
}

/* THE "solo" PROFILE IS GONE, AND MUST NOT COME BACK.
 *
 * It withheld delegate/delegate_status/roundtable_review/roundtable_status so a
 * run could be measured without a second agent's tokens landing outside the
 * caller's transcript. That makes the measured thing a configuration nobody
 * deploys -- the benchmark stops describing aimee and starts describing a
 * variant built for the benchmark. If delegates should not run, do not configure
 * them; that is a real deployment state. Hiding shipped tools to flatter a
 * measurement is not.
 *
 * An unknown profile fails OPEN to the full set, so "solo" now presents
 * everything rather than silently withholding. */
static void test_solo_profile_is_gone(void)
{
   cJSON *tools = mcp_build_tools_list_flat();
   assert(tools_get(tools, "delegate") != NULL);
   assert(tools_get(tools, "roundtable_review") != NULL);
   /* Unknown profile -> fail open: nothing is removed. */
   assert(mcp_filter_tools_for_profile(tools, "solo", 1) == 0);
   assert(tools_get(tools, "delegate") != NULL);
   assert(tools_get(tools, "roundtable_review") != NULL);
   cJSON_Delete(tools);

   /* "core" ships delegation, as it always did. */
   cJSON *core_tools = mcp_build_tools_list_flat();
   mcp_filter_tools_for_profile(core_tools, "core", 1);
   assert(tools_get(core_tools, "delegate") != NULL);
   assert(tools_get(core_tools, "delegate_status") != NULL);
   cJSON_Delete(core_tools);
}

/* Delegation off removes the delegate tools from EVERY profile, including the
 * ones that otherwise remove nothing.
 *
 * "full" and unknown profiles are the cases worth asserting: they fail open and
 * return early, so a delegation filter written after that early-return would
 * silently do nothing on exactly the surface that presents the most tools.
 *
 * roundtable_review must SURVIVE. Withholding it would be hiding a working tool
 * to flatter a measurement, which is what the "no solo profile" decision above
 * exists to prevent; the review levers change what the prompt demands, not what
 * the server can do. */
static void test_delegates_disabled_withholds_delegate_tools(void)
{
   const char *profiles[] = {"full", "nonsense", "core", "lean", NULL};
   for (int i = 0; profiles[i]; i++)
   {
      cJSON *tools = mcp_build_tools_list_flat();
      assert(tools_get(tools, "delegate") != NULL); /* present before */
      int removed = mcp_filter_tools_for_profile(tools, profiles[i], 0);
      assert(removed >= 2);
      assert(tools_get(tools, "delegate") == NULL);
      assert(tools_get(tools, "delegate_status") == NULL);
      assert(tools_get(tools, "roundtable_review") != NULL);
      cJSON_Delete(tools);
   }
}

static void test_agent_code_intelligence_contracts(void)
{
   cJSON *collapsed = mcp_build_tools_list();
   cJSON *flat = mcp_build_tools_list_flat();

   cJSON *preview = tools_get(collapsed, AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS);
   assert(preview != NULL);
   cJSON *description = cJSON_GetObjectItemCaseSensitive(preview, "description");
   assert(cJSON_IsString(description));
   assert(strstr(description->valuestring, "blast radius") != NULL);
   assert(strstr(description->valuestring, "Preview") != NULL);
   assert(mcp_tool_matches_query(preview, AIMEE_CODE_DISCOVERY_BLAST_RADIUS));
   assert(mcp_tool_matches_query(preview, "BLAST RADIUS"));
   assert(mcp_tool_matches_query(preview, AIMEE_CODE_DISCOVERY_PREVIEW));
   assert(!mcp_tool_matches_query(preview, "unrelated memory query"));

   /* An agent searching for a tool types the words, not the identifier. A whole
    * -string substring test cannot match "delegate status" against a tool named
    * delegate_status, so the search returned nothing for a tool that exists and
    * the agent fell back to dumping the entire catalogue. Match on words, with
    * separators treated alike. */
   {
      cJSON *flat_list = mcp_build_tools_list_flat();
      cJSON *status = tools_get(flat_list, "delegate_status");
      assert(status != NULL);
      assert(mcp_tool_matches_query(status, "delegate status"));
      assert(mcp_tool_matches_query(status, "delegate_status"));
      assert(mcp_tool_matches_query(status, "STATUS delegate"));
      /* Every word still has to appear: this must not become a fuzzy OR. */
      assert(!mcp_tool_matches_query(status, "delegate vault rotation"));
      cJSON_Delete(flat_list);
   }
   assert(schema_has_property(preview, "project"));
   assert(schema_has_property(preview, "scope"));
   assert(schema_has_property(preview, "paths"));
   assert(!schema_requires(preview, "project"));
   assert(schema_requires(preview, "paths"));

   cJSON *symbol = tools_get(collapsed, AIMEE_CODE_TOOL_FIND_SYMBOL);
   assert(symbol != NULL);
   assert(schema_has_property(symbol, "identifier"));
   assert(schema_has_property(symbol, "project"));
   assert(schema_has_property(symbol, "scope"));

   cJSON *callers = tools_get(flat, "index_find_callers");
   assert(callers != NULL);
   assert(schema_has_property(callers, "project"));
   assert(schema_has_property(callers, "scope"));

   /* Batch span reads used to advertise only `spans: array`, leaving clients to
    * guess that its elements were strings. The handler expects structured
    * ranges, so the plausible guess returned [] and forced a duplicate shell
    * read. Pin the item schema on both the flat tool and collapsed index family. */
   cJSON *span = tools_get(flat, "code_span_get");
   cJSON *index = tools_get(collapsed, "index");
   assert(span != NULL && index != NULL);
   cJSON *span_arrays[] = {schema_property(span, "spans"), schema_property(index, "spans")};
   for (size_t i = 0; i < sizeof(span_arrays) / sizeof(span_arrays[0]); i++)
   {
      cJSON *items = cJSON_GetObjectItemCaseSensitive(span_arrays[i], "items");
      cJSON *item_type = cJSON_GetObjectItemCaseSensitive(items, "type");
      cJSON *properties = cJSON_GetObjectItemCaseSensitive(items, "properties");
      cJSON *required = cJSON_GetObjectItemCaseSensitive(items, "required");
      assert(cJSON_IsString(item_type) && strcmp(item_type->valuestring, "object") == 0);
      assert(cJSON_GetObjectItemCaseSensitive(properties, "file_path") != NULL);
      assert(cJSON_GetObjectItemCaseSensitive(properties, "line_start") != NULL);
      assert(cJSON_GetObjectItemCaseSensitive(properties, "line_end") != NULL);
      cJSON *first_required = cJSON_GetArrayItem(required, 0);
      assert(cJSON_IsString(first_required));
      assert(strcmp(first_required->valuestring, "file_path") == 0);
   }

   cJSON *args = cJSON_Parse("{\"cwd\":\"/work/aimee\"}");
   /* cwd is resolved to a stable identity at the server dispatch boundary;
    * this helper deliberately refuses the old basename fallback. */
   assert(mcp_code_project_from_args(args) == NULL);
   assert(mcp_code_scope_all(args) == 0);
   cJSON_AddStringToObject(args, "project", "explicit-project");
   assert(strcmp(mcp_code_project_from_args(args), "explicit-project") == 0);
   cJSON_AddStringToObject(args, "scope", "all");
   assert(mcp_code_scope_all(args) == 1);
   cJSON_ReplaceItemInObject(args, "scope", cJSON_CreateString(""));
   assert(mcp_code_scope_all(args) == 0);
   cJSON_ReplaceItemInObject(args, "scope", cJSON_CreateString("invalid"));
   assert(mcp_code_scope_all(args) == -1);
   cJSON_Delete(args);

   cJSON_Delete(flat);
   cJSON_Delete(collapsed);
   printf("  PASS: agent_code_intelligence_contracts\n");
}

/* The flat list keeps family members; the collapsed one folds them away.
 *
 * mcp_collapse_families presents coherent families as ONE multiplexed tool
 * (index{command:"find_callers"}) to keep an external client's tool count down.
 * aimee's own agents need the individual names, because a toolset grants tools one
 * at a time -- review_indexed gets index_find_callers but not index_hybrid, which a
 * single `index` tool cannot express.
 *
 * The native advert first looked the flat names up in the COLLAPSED list, found
 * nothing, and silently dropped every code-intelligence tool ("has no usable
 * advert; not offered natively") -- while the toolset still resolved them. So a
 * review panel was granted index_find_callers and never offered it. Dispatch was
 * fine throughout (the flat names stay callable via mcp_family_demux); only the
 * advert lied. Four green CI runs and a passing unit test with a faked provider all
 * missed it; a real server on real hardware caught it in one line of log. */
static void test_flat_list_keeps_family_members(void)
{
   cJSON *flat = mcp_build_tools_list_flat();
   cJSON *collapsed = mcp_build_tools_list();

   /* A family member: flat has the individual tool, collapsed folds it into `index`. */
   assert(tools_have(flat, "index_find_callers"));
   assert(!tools_have(collapsed, "index_find_callers"));
   assert(tools_have(collapsed, "index"));
   assert(!tools_have(flat, "index"));

   /* Every tool the native surface declares must survive in the flat list, or it is
    * registered, resolvable in a toolset, and never offered to the model. */
   const char *native[] = {"index_find_callers", "index_blast_radius", "index_structure",
                           "get_context_block",  "search_graph",       "ast_grep_search"};
   for (size_t i = 0; i < sizeof(native) / sizeof(native[0]); i++)
      assert(tools_have(flat, native[i]));

   /* A non-family tool is in both: collapsing changes nothing for it. */
   assert(tools_have(flat, "ast_grep_search") && tools_have(collapsed, "ast_grep_search"));

   assert(cJSON_GetArraySize(flat) > cJSON_GetArraySize(collapsed));
   cJSON_Delete(flat);
   cJSON_Delete(collapsed);
   printf("  PASS: flat_list_keeps_family_members\n");
}

/* Trimming prose must cost tokens and nothing else.
 *
 * tools/list is re-sent as context on every turn, and 59% of its 20 KB is guidance
 * prose rather than the shape a client needs to build a call. Shortening it is only
 * safe if every tool survives and every schema stays exactly as callable, so that is
 * what this asserts -- not the byte count, which will drift, but the invariants. */
static void test_prose_trim_keeps_tools_callable(void)
{
   cJSON *full = mcp_build_tools_list();
   cJSON *lean = mcp_build_tools_list();
   int n = cJSON_GetArraySize(full);
   assert(n > 0);

   char *before = cJSON_PrintUnformatted(lean);
   assert(mcp_compact_tool_prose(lean) == n);
   char *after = cJSON_PrintUnformatted(lean);

   /* It actually saves something, and no tool was dropped. */
   assert(strlen(after) < strlen(before));
   assert(cJSON_GetArraySize(lean) == n);

   for (int i = 0; i < n; i++)
   {
      cJSON *f = cJSON_GetArrayItem(full, i);
      cJSON *l = cJSON_GetArrayItem(lean, i);
      const char *fname = cJSON_GetObjectItemCaseSensitive(f, "name")->valuestring;
      const char *lname = cJSON_GetObjectItemCaseSensitive(l, "name")->valuestring;
      assert(strcmp(fname, lname) == 0); /* same tools, same order */

      /* A description may shrink but must never vanish: a nameless tool is worse
       * than a terse one. */
      cJSON *ld = cJSON_GetObjectItemCaseSensitive(l, "description");
      if (cJSON_GetObjectItemCaseSensitive(f, "description"))
         assert(cJSON_IsString(ld) && ld->valuestring[0]);

      /* The callable shape is untouched: same properties, same types, same required. */
      cJSON *fs = cJSON_GetObjectItemCaseSensitive(f, "inputSchema");
      cJSON *ls = cJSON_GetObjectItemCaseSensitive(l, "inputSchema");
      if (!fs)
         continue;
      cJSON *fp = cJSON_GetObjectItemCaseSensitive(fs, "properties");
      cJSON *lp = cJSON_GetObjectItemCaseSensitive(ls, "properties");
      assert(cJSON_GetArraySize(fp) == cJSON_GetArraySize(lp));
      cJSON *prop = NULL;
      cJSON_ArrayForEach(prop, fp)
      {
         cJSON *mirror = cJSON_GetObjectItemCaseSensitive(lp, prop->string);
         assert(mirror != NULL);
         cJSON *ft = cJSON_GetObjectItemCaseSensitive(prop, "type");
         cJSON *lt = cJSON_GetObjectItemCaseSensitive(mirror, "type");
         if (cJSON_IsString(ft))
            assert(cJSON_IsString(lt) && strcmp(ft->valuestring, lt->valuestring) == 0);
         cJSON *fe = cJSON_GetObjectItemCaseSensitive(prop, "enum");
         cJSON *le = cJSON_GetObjectItemCaseSensitive(mirror, "enum");
         if (fe)
            assert(le && cJSON_GetArraySize(fe) == cJSON_GetArraySize(le));
      }
      cJSON *fr = cJSON_GetObjectItemCaseSensitive(fs, "required");
      cJSON *lr = cJSON_GetObjectItemCaseSensitive(ls, "required");
      if (fr)
         assert(lr && cJSON_GetArraySize(fr) == cJSON_GetArraySize(lr));
   }

   free(before);
   free(after);
   cJSON_Delete(full);
   cJSON_Delete(lean);
}

int main(void)
{
   test_prose_trim_keeps_tools_callable();
   printf("test_mcp_client_registry\n");
   test_tools_list_surface();
   test_tool_profile_filter();
   test_call_tool_demux();
   test_get_help_topics_exist();
   test_solo_profile_is_gone();
   test_delegates_disabled_withholds_delegate_tools();
   test_agent_code_intelligence_contracts();
   test_flat_list_keeps_family_members();
   test_boot_and_lazy_tools();
   test_install_target_filtering();
   test_namespaced_tools_and_dispatch();
   test_failed_client_does_not_abort_boot();
   test_osv_gate_blocks_malware();
   test_osv_gate_shadow_and_allowlist_allow();
   test_osv_offline_cache_miss_allows();
   printf("  all tests passed\n");
   return 0;
}
