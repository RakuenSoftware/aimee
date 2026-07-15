#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "mcp_osv_cache.h"
#include "mcp_client_registry.h"
#include "mcp_tools.h"

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
   snprintf(dir, dir_len, "/tmp/aimee-mcp-registry-XXXXXX");
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

static void test_boot_and_lazy_tools(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mock");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");

   assert(mcp_client_registry_boot(&cfg) == 1);
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

static void test_namespaced_tools_and_dispatch(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.mcp_client_count = 1;
   snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mock");
   cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
   cfg.mcp_clients[0].command_count = 2;
   snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
            mock_server_path());
   snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");

   assert(mcp_client_registry_boot(&cfg) == 1);

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
   int saw_job_family = 0;
   int saw_skill_manage = 0;
   int saw_skill_manage_cwd = 0;
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, public_tools)
   {
      cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "mock:echo") == 0)
         saw_namespaced = 1;
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "delegate_status") == 0)
         saw_delegate_status = 1;
      /* job_start/job_status were collapsed into the `job` family (P4b); the
       * member description no longer appears standalone. Verify the family tool. */
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, "job") == 0)
         saw_job_family = 1;
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
   assert(saw_job_family);
   assert(saw_skill_manage);
   assert(saw_skill_manage_cwd);
   cJSON_Delete(public_tools);

   mcp_client_registry_shutdown();
}

static void test_failed_client_does_not_abort_boot(void)
{
   config_t cfg;
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

   assert(mcp_client_registry_boot(&cfg) == 1);
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

   config_t cfg;
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

   assert(mcp_client_registry_boot(&cfg) == 0);
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

   config_t cfg;
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
   assert(mcp_client_registry_boot(&cfg) == 1);
   assert(strcmp(g_last_audit_action, "shadow_block") == 0);
   mcp_client_registry_shutdown();

   reset_osv_stub();
   g_cache_hit = 1;
   snprintf(g_cache_row.verdict, sizeof(g_cache_row.verdict), "%s", "malware");
   snprintf(g_cache_row.advisory_ids, sizeof(g_cache_row.advisory_ids), "%s", "MAL-2026-2");
   cfg.mcp_osv_enforce = 1;
   cfg.mcp_osv_allow_count = 1;
   snprintf(cfg.mcp_osv_allow[0], sizeof(cfg.mcp_osv_allow[0]), "%s", "npm:bad-pkg");
   assert(mcp_client_registry_boot(&cfg) == 1);
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

   config_t cfg;
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

   assert(mcp_client_registry_boot(&cfg) == 1);
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
#define MCP_TOOLS_GOLDEN_COUNT 52
#define MCP_TOOLS_GOLDEN                                                                           \
   "ask_user {choices,question} req:question\n"                                                    \
   "ast_grep_search {lang,path,pattern} req:lang,pattern\n"                                        \
   "attempt {approach,command,filter,lesson,outcome,task_context} req:command\n"                   \
   "autopilot {action,job_id,pipeline_id,plan_depth,plan_id,task} req:action\n"                    \
   "background {action,command,cwd,id,tail_lines} req:action\n"                                    \
   "clarify {answer,command,description,session_id} req:command\n"                                 \
   "dashboard_metrics {} req:\n"                                                                   \
   "delegate {branch,cwd,persona,prompt,role} req:persona,prompt,role\n"                           \
   "delegate_reply {content,delegation_id} req:content,delegation_id\n"                            \
   "delegate_status {job_id} req:job_id\n"                                                         \
   "describe_tool {name} req:name\n"                                                               \
   "diagnose {command,content,diagnosis_id,hypothesis_id,rank,source,stance,symptom} "             \
   "req:command\n"                                                                                 \
   "ensemble {assignments,channel,command,id,limit,message,reason,speaker,template} "              \
   "req:command\n"                                                                                 \
   "ensemble_review {brief,diff,rounds,turns} req:diff\n"                                          \
   "epistemic_directive "                                                                          \
   "{anchor_entity,anchor_file,cause,command,id,limit,note,priority,question,resolution_memory_"   \
   "id,state,suppress,topic,valid_until} req:command\n"                                            \
   "find_symbol {identifier} req:identifier\n"                                                     \
   "find_tools {limit,query} req:\n"                                                               \
   "get_help {topic} req:\n"                                                                       \
   "get_identity {} req:\n"                                                                        \
   "git "                                                                                          \
   "{action,async,base,body,branch,command,count,depth,diff_stat,files,force,index,job_id,"        \
   "message,mirror,mode,name,number,path,prune,rebase,ref,remote,source,staged,stat_only,state,"   \
   "title,url,wait} req:command\n"                                                                 \
   "graph {command,entity,episode_key,limit,query} req:command\n"                                  \
   "host {command,name} req:command\n"                                                             \
   "index "                                                                                        \
   "{command,file_path,judge,line_end,line_start,max_results,node,paths,project,query,symbol} "    \
   "req:command\n"                                                                                 \
   "job {command,job_id,max_concurrent,plan_id} req:command\n"                                     \
   "learning "                                                                                     \
   "{command,correction_text,description,evidence_refs,limit,polarity,signal_type,sink,state,"     \
   "target_key,target_memory_id,title,workflow_project,workflow_signal_type} req:command\n"        \
   "list_curiosity_items {limit,state} req:\n"                                                     \
   "lsp {col,command,file,line,workspace} req:command\n"                                           \
   "memory "                                                                                       \
   "{command,confidence,content,dry_run,force,handle,id,key,kind,memory_id,modes,query,reason,"    \
   "tier,verb} req:command\n"                                                                      \
   "memory_recall {limit_tokens,session_start,task_hint} req:\n"                                   \
   "note {command,content,limit,query,tag,tags,title} req:command\n"                               \
   "payload_rewrite_status {} req:\n"                                                              \
   "pdf_inspect_structure {document_key,project} req:document_key,project\n"                       \
   "pdf_list_assets {document_key,project} req:document_key,project\n"                             \
   "pdf_lookup_table {document_key,page_no,project} req:document_key,project\n"                    \
   "pdf_open_asset {asset_id,project} req:asset_id,project\n"                                      \
   "pdf_open_neighbors {chunk_id,project} req:chunk_id,project\n"                                  \
   "pdf_open_page {document_key,page_no,project} req:document_key,page_no,project\n"               \
   "pdf_search_chunks {max_results,project,query} req:project,query\n"                             \
   "pipeline "                                                                                     \
   "{artifact,base_branch,brief,command,done_bar,head_branch,idea,operator_principal,"             \
   "pipeline_id,questions,reason,remote,repo_root,state,verdict,worktree_path} req:command\n"      \
   "prospective_memory "                                                                           \
   "{action_text,anchor_entity,anchor_file,command,id,limit,recurrence,state,trigger_text,valid_"  \
   "until} req:command\n"                                                                          \
   "recall {block_type,command,limit,limit_tokens,query,since} req:command\n"                      \
   "roadmap {command,roadmap_id} req:command\n"                                                    \
   "rules {command,reason,text} req:command\n"                                                     \
   "search_docs {max_results,query} req:query\n"                                                   \
   "search_memory {filter,query} req:query\n"                                                      \
   "send_message {target,text} req:target,text\n"                                                  \
   "session {around_message_id,chain_id,command,include_sources,limit,query,session_id,window} "   \
   "req:command\n"                                                                                 \
   "skill_manage "                                                                                 \
   "{absorbed_into,action,content,cwd,file_path,name,new_string,old_string,replace_all} "          \
   "req:action,name\n"                                                                             \
   "store_workflow {project,rule,signal_type} req:rule,signal_type\n"                              \
   "task_list {limit,session_id,state} req:\n"                                                     \
   "work {command,status_filter} req:command\n"                                                    \
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
   int o = snprintf(out, outsz, "%s {", cJSON_IsString(name) ? name->valuestring : "?");
   for (int i = 0; i < nk; i++)
      o += snprintf(out + o, outsz - (size_t)o, "%s%s", i ? "," : "", keys[i]);
   o += snprintf(out + o, outsz - (size_t)o, "} req:");
   for (int i = 0; i < nr; i++)
      o += snprintf(out + o, outsz - (size_t)o, "%s%s", i ? "," : "", rq[i]);
}
static void test_tools_list_surface(void)
{
   cJSON *tools = mcp_build_tools_list();
   assert(cJSON_IsArray(tools));
   static char sigs[256][256];
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
   static char joined[256 * 256];
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
    * Includes the P2 discovery meta-tools find_tools/describe_tool. */
   static const char *const core[] = {
       "get_help",        "find_tools",    "describe_tool", "search_docs",
       "search_memory",   "memory_recall", "get_identity",  "find_symbol",
       "ast_grep_search", "git",           "delegate",      "ensemble_review",
       "ask_user",        "send_message",  "note",          NULL};
   int expect = 0;
   for (int i = 0; core[i]; i++)
      expect++;
   assert(profile_core_has("find_tools", core) && profile_core_has("describe_tool", core));

   /* Control the env so the default is deterministic across CI runners. */
   unsetenv("AIMEE_MCP_TOOL_PROFILE");

   /* Explicit "full" and an unknown profile are no-ops (fail open). */
   cJSON *t = mcp_build_tools_list();
   int full = cJSON_GetArraySize(t);
   assert(full > expect);
   assert(mcp_filter_tools_for_profile(t, "full") == 0 && cJSON_GetArraySize(t) == full);
   assert(mcp_filter_tools_for_profile(t, "nonsense") == 0 && cJSON_GetArraySize(t) == full);
   cJSON_Delete(t);

   /* P2 default: env unset + NULL profile resolves to "core" and filters. */
   assert(strcmp(mcp_tool_profile_effective(NULL), "core") == 0);
   t = mcp_build_tools_list();
   assert(mcp_filter_tools_for_profile(t, NULL) == full - expect);
   assert(cJSON_GetArraySize(t) == expect);
   cJSON_Delete(t);

   /* "core" keeps exactly the Tier-0 set — and every named core tool exists in
    * the built list (kept count == expected count proves none was dropped). */
   t = mcp_build_tools_list();
   int removed = mcp_filter_tools_for_profile(t, "core");
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
   assert(mcp_filter_tools_for_profile(t, "lean") == full - expect);
   cJSON_Delete(t);
}

int main(void)
{
   printf("test_mcp_client_registry\n");
   test_tools_list_surface();
   test_tool_profile_filter();
   test_boot_and_lazy_tools();
   test_namespaced_tools_and_dispatch();
   test_failed_client_does_not_abort_boot();
   test_osv_gate_blocks_malware();
   test_osv_gate_shadow_and_allowlist_allow();
   test_osv_offline_cache_miss_allows();
   printf("  all tests passed\n");
   return 0;
}
