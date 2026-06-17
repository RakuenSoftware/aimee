#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sqlite3.h>
#include "aimee.h"
#include "db.h"
#include "db_schema.h"
#include "db1.h"
#include "agent.h"
#include "agent_config.h"
#include "agent_tools.h"
#include "workspace_provider.h"
#include "agent_adapter.h"
#include "provider_cli_adapter.h"
#include "agent_protocol.h"
#include "agent_shell.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_test_util.h"

void test_agent_route_with_caps_honors_tools_enabled(void);
void test_agent_route_with_caps_honors_context_override(void);
void test_tools_enabled_capability_default(void);

/* Defined in test_agent_responses.c (split out to keep this file under the
 * 2000-line hard limit); called from main() below. */
void test_responses_parser_keeps_all_output_text_parts(void);
void test_responses_parser_accumulates_output_text_deltas(void);
void test_responses_parser_uses_output_text_done(void);
void test_responses_parser_separates_message_items(void);

/* --- Expose tool functions for testing via redeclaration --- */
char *tool_bash(const char *command, int timeout_ms);
char *tool_read_file(const char *path, int offset, int limit);
char *tool_write_file(const char *path, const char *content);
char *tool_edit_file(const char *path, const char *old_string, const char *new_string,
                     int replace_all);
char *tool_list_files(const char *path, const char *pattern);
char *tool_grep(const char *path, const char *pattern, int max_results);
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms);
int agent_execute_cli_shell_driver(const agent_t *agent, const char *driver_name,
                                   const char *display_name, const char *default_cli_cmd,
                                   const char *system_prompt, const char *user_prompt,
                                   agent_result_t *out);
void test_cancelled_durable_job_blocks_tool_dispatch(void);
void test_delegate_bash_cancel_kills_running_tool(void);
void test_parent_write_guard_readonly_pipeline(void);
void test_parent_write_guard_readonly_large_find(void);
void test_parent_write_guard_allows_mkdir_in_delegate_worktree(void);
void test_parent_write_guard_allows_workspace_file_ops(void);
void test_parent_write_guard_allows_workspace_chain(void);
void test_parent_write_guard_allows_readonly_printf(void);
void otel_init(const char *endpoint, const char *service_name, const char *session)
{
   (void)endpoint;
   (void)service_name;
   (void)session;
}
void otel_on_trace(const char *direction, const char *tool_name, const char *tool_args,
                   const char *tool_result, int turn)
{
   (void)direction;
   (void)tool_name;
   (void)tool_args;
   (void)tool_result;
   (void)turn;
}
const agent_shell_driver_t *agent_shell_driver_get(const char *name)
{
   (void)name;
   return NULL;
}
static cJSON *parse_json_or_die(const char *text)
{
   cJSON *json = cJSON_Parse(text);
   assert(json != NULL);
   return json;
}
static int tools_array_has_name(cJSON *tools, const char *expected)
{
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (!cJSON_IsString(name))
      {
         cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
         name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
      }
      if (cJSON_IsString(name) && strcmp(name->valuestring, expected) == 0)
         return 1;
   }
   return 0;
}

static void test_agent_expand_env(void)
{
   char dst[128];
   platform_setenv("AIMEE_TEST_ENV", "expanded");
   agent_expand_env("$AIMEE_TEST_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "expanded") == 0);
   agent_expand_env("$AIMEE_NO_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "$AIMEE_NO_ENV") == 0);
   agent_expand_env("", dst, sizeof(dst));
   assert(strcmp(dst, "") == 0);
   agent_expand_env("plain string", dst, sizeof(dst));
   assert(strcmp(dst, "plain string") == 0);
}

static void test_agent_has_role(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   strcpy(agent.roles[0], "summarize");
   agent.role_count = 1;
   assert(agent_has_role(&agent, "summarize") == 1);
   assert(agent_has_role(&agent, "translate") == 0);
   agent.role_count = 0;
   assert(agent_has_role(&agent, "summarize") == 0);
}

static void test_agent_find(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "agent_one");
   strcpy(cfg.agents[1].name, "agent_two");

   assert(agent_find(&cfg, "agent_one") == &cfg.agents[0]);
   assert(agent_find(&cfg, "agent_two") == &cfg.agents[1]);
   assert(agent_find(&cfg, "missing") == NULL);
}

static void test_agent_route(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;

   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);

   cfg.agents[0].enabled = 0;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.default_agent, "cheap");
   strcpy(cfg.agents[0].name, "cheap");
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   strcpy(cfg.agents[1].exec_roles[0], "custom_exec");
   cfg.agents[1].exec_role_count = 1;
   assert(agent_route(&cfg, "execute") == &cfg.agents[0]);
   assert(agent_route(&cfg, "custom_exec") == &cfg.agents[1]);
   assert(agent_route(&cfg, "no_role") == NULL);
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "expensive");
   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 2;
   cfg.agents[1].enabled = 1;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);
   cfg.agents[1].cost_tier = 0;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "missing-cli");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(cfg.agents[0].cli_cmd, "aimee-definitely-missing-provider-cli-test");
   strcpy(cfg.agents[1].name, "http-fallback");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
   assert(agent_is_available_for_routing(&cfg.agents[1]) == 1);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   assert(agent_route_at_tier(&cfg, "summarize", 0) == NULL);
   assert(agent_route_at_tier(&cfg, "summarize", 1) == &cfg.agents[1]);
   strcpy(cfg.agents[0].cli_cmd, "/bin/echo ; /bin/true");
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
}

/* The OpenAI Chat and Responses tool surfaces are generated from one builtin
 * tool table (agent_tools.c). This gate locks in the single-source guarantee:
 *   - exact per-surface membership (the surface-specific tools are explicit,
 *     not accidental drift), and
 *   - every tool present on BOTH surfaces has byte-identical name/description/
 *     parameters (modulo the type:function nesting), so the two surfaces can no
 *     longer drift in the content of a shared tool the way they had before. */
static cJSON *tools_find(cJSON *arr, const char *name, int chat)
{
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, arr)
   {
      cJSON *body = chat ? cJSON_GetObjectItem(t, "function") : t;
      cJSON *nm = cJSON_GetObjectItem(body, "name");
      if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0)
         return body;
   }
   return NULL;
}

static int tools_has(cJSON *arr, const char *name, int chat)
{
   return tools_find(arr, name, chat) != NULL;
}

static void test_tool_surface_single_source(void)
{
   cJSON *chat = build_tools_array();
   cJSON *resp = build_tools_array_responses();

   /* Chat tools nest {name,description,parameters} under "function"; Responses
    * tools carry them flat. Both tag type:function. */
   cJSON *first_chat = cJSON_GetArrayItem(chat, 0);
   assert(cJSON_IsObject(cJSON_GetObjectItem(first_chat, "function")));
   cJSON *first_resp = cJSON_GetArrayItem(resp, 0);
   assert(cJSON_IsString(cJSON_GetObjectItem(first_resp, "name")));
   assert(!cJSON_GetObjectItem(first_resp, "function"));

   /* Declared per-surface membership: the surface-specific tools are explicit.
    * code_search (code) and search_docs (docs) are different tools, not a
    * rename — each stays on its surface until a product decision unifies them. */
   assert(tools_has(chat, "code_search", 1) && tools_has(chat, "execute_script", 1));
   assert(!tools_has(chat, "search_docs", 1));
   assert(tools_has(resp, "search_docs", 0));
   assert(!tools_has(resp, "code_search", 0) && !tools_has(resp, "execute_script", 0));

   /* Drift gate: every tool on BOTH surfaces has identical name/description/
    * parameters. cJSON_Compare(case-sensitive) over the normalized body. */
   cJSON *t = NULL;
   int shared = 0;
   cJSON_ArrayForEach(t, chat)
   {
      cJSON *cbody = cJSON_GetObjectItem(t, "function");
      const char *name = cJSON_GetObjectItem(cbody, "name")->valuestring;
      cJSON *rbody = tools_find(resp, name, 0);
      if (!rbody)
         continue; /* surface-specific (execute_script, code_search) */
      shared++;
      assert(cJSON_Compare(cJSON_GetObjectItem(cbody, "description"),
                           cJSON_GetObjectItem(rbody, "description"), 1));
      assert(cJSON_Compare(cJSON_GetObjectItem(cbody, "parameters"),
                           cJSON_GetObjectItem(rbody, "parameters"), 1));
   }
   assert(shared >= 20); /* the bulk of the surface is shared and now consistent */

   cJSON_Delete(chat);
   cJSON_Delete(resp);
}

static void test_current_code_only_role_tool_policy(void)
{
   assert(agent_tools_role_current_code_only("review") == 1 &&
          agent_tools_role_current_code_only("diagnose") == 1);
   assert(agent_tools_role_current_code_only("inspect") == 1 &&
          agent_tools_role_current_code_only("validate") == 0);
   assert(agent_tools_tool_allowed_for_role("validate", "bash") == 1);
   assert(agent_tools_tool_allowed_for_role("validate", "write_file") == 0);
   assert(agent_tools_tool_allowed_for_role("search", "bash") == 0);
   cJSON *tools = build_tools_array();
   assert(tools_array_has_name(tools, "read_file") && tools_array_has_name(tools, "find_symbol"));
   agent_tools_filter_for_role(tools, "review");
   assert(tools_array_has_name(tools, "read_file") && !tools_array_has_name(tools, "find_symbol"));
   assert(!tools_array_has_name(tools, "search_memory") &&
          !tools_array_has_name(tools, "search_docs"));
   cJSON_Delete(tools);
   tools = build_tools_array_anthropic();
   agent_tools_filter_for_role(tools, "diagnose");
   assert(tools_array_has_name(tools, "read_file") && !tools_array_has_name(tools, "find_symbol"));
   cJSON_Delete(tools);
   tools = build_tools_array();
   agent_tools_filter_for_role(tools, "validate");
   assert(tools_array_has_name(tools, "bash") && tools_array_has_name(tools, "read_file"));
   assert(tools_array_has_name(tools, "search_memory") &&
          !tools_array_has_name(tools, "write_file"));
   assert(!tools_array_has_name(tools, "search_docs"));
   cJSON_Delete(tools);
}

static void test_current_code_only_dispatch_blocks_stale_context_tools(void)
{
   agent_tools_set_dispatch_role("diagnose");

   char *result = dispatch_tool_call("find_symbol", "{\"identifier\":\"main\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "current-checkout evidence") != NULL);
   free(result);

   result = dispatch_tool_call("aimee:search_memory", "{\"query\":\"prior facts\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "current-checkout evidence") != NULL);
   free(result);

   result = dispatch_tool_call("bash", "{\"command\":\"aimee index scan\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "mutating or broad aimee context commands are disabled") != NULL);
   free(result);

   agent_tools_set_dispatch_role(NULL);
}

static void restore_env(const char *name, const char *value)
{
   if (value)
      setenv(name, value, 1);
   else
      unsetenv(name);
}

static void test_provider_env_credentials_and_headers(void)
{
   char old_openrouter[256] = "";
   char old_anthropic[256] = "";
   char old_gemini[256] = "";
   char old_google[256] = "";
   char old_gemini_mechanism[256] = "";
   const char *v;

   if ((v = getenv("OPENROUTER_API_KEY")))
      snprintf(old_openrouter, sizeof(old_openrouter), "%s", v);
   if ((v = getenv("ANTHROPIC_API_KEY")))
      snprintf(old_anthropic, sizeof(old_anthropic), "%s", v);
   if ((v = getenv("GEMINI_API_KEY")))
      snprintf(old_gemini, sizeof(old_gemini), "%s", v);
   if ((v = getenv("GOOGLE_API_KEY")))
      snprintf(old_google, sizeof(old_google), "%s", v);
   if ((v = getenv("GEMINI_API_KEY_AUTH_MECHANISM")))
      snprintf(old_gemini_mechanism, sizeof(old_gemini_mechanism), "%s", v);

   agent_t ag;
   char auth[512];
   char headers[512];

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "openrouter");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "bearer");
   unsetenv("OPENROUTER_API_KEY");
   assert(agent_has_resolvable_credentials(&ag) == 0);
   assert(agent_is_available_for_routing(&ag) == 0);
   setenv("OPENROUTER_API_KEY", "or-test-key", 1);
   assert(agent_has_resolvable_credentials(&ag) == 1);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer or-test-key") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "HTTP-Referer: https://github.com/JBailes/aimee") != NULL);
   assert(strstr(headers, "X-Title: aimee") != NULL);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "anthropic");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "api_key");
   setenv("ANTHROPIC_API_KEY", "anth-test-key", 1);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "x-api-key: anth-test-key") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "anthropic-version: 2023-06-01") != NULL);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "gemini");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "api_key");
   unsetenv("GEMINI_API_KEY");
   unsetenv("GEMINI_API_KEY_AUTH_MECHANISM");
   setenv("GOOGLE_API_KEY", "google-test-key", 1);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "x-goog-api-key: google-test-key") == 0);

   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "bearer");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer google-test-key") == 0);

   restore_env("OPENROUTER_API_KEY", old_openrouter[0] ? old_openrouter : NULL);
   restore_env("ANTHROPIC_API_KEY", old_anthropic[0] ? old_anthropic : NULL);
   restore_env("GEMINI_API_KEY", old_gemini[0] ? old_gemini : NULL);
   restore_env("GOOGLE_API_KEY", old_google[0] ? old_google : NULL);
   restore_env("GEMINI_API_KEY_AUTH_MECHANISM",
               old_gemini_mechanism[0] ? old_gemini_mechanism : NULL);
}

/* A thin-client-supplied per-turn Codex OAuth token takes precedence over any
 * server-side file, and the account id is injected as the ChatGPT-Account-ID
 * header; clearing removes both. */
static void test_codex_oauth_request_creds(void)
{
   agent_t ag;
   char auth[512];
   char headers[512];
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "codex");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "codex-oauth");

   agent_set_request_codex_creds("REQ-TOKEN", "acct-1");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer REQ-TOKEN") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "ChatGPT-Account-ID: acct-1") != NULL);
   assert(strstr(headers, "originator: codex_cli_rs") != NULL);

   /* Clearing the per-turn creds removes the token path + header injection. */
   agent_set_request_codex_creds(NULL, NULL);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "ChatGPT-Account-ID:") == NULL);
}

/* WP-C.2c(3): the vault principal must ride along in the creds snapshot so a
 * fan-out delegate (fresh thread; rebinds via agent_request_creds_restore)
 * reaches the user's vault like a same-thread one; empty restores to empty. */
static void test_request_creds_snapshot_carries_vault_principal(void)
{
   agent_set_request_vault_principal("webuser:dave");
   agent_request_creds_t snap;
   agent_request_creds_snapshot(&snap);
   assert(strcmp(snap.vault_principal, "webuser:dave") == 0);
   agent_set_request_vault_principal(NULL); /* fresh fan-out worker starts clear */
   agent_request_creds_restore(&snap);
   assert(strcmp(agent_get_request_vault_principal(), "webuser:dave") == 0);
   agent_set_request_vault_principal(NULL);
   agent_request_creds_snapshot(&snap);
   agent_set_request_vault_principal("webuser:eve");
   agent_request_creds_restore(&snap);
   assert(agent_get_request_vault_principal()[0] == '\0');
   agent_set_request_session(NULL); /* restore re-bound session+codex; clear all */
   agent_set_request_codex_creds(NULL, NULL);
   agent_set_request_vault_principal(NULL);
}

static void test_agent_config_provider_cli_roundtrip(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   char fake_bin[MAX_PATH_LEN];
   snprintf(fake_bin, sizeof(fake_bin), "%s/aimee-test-agent-bin-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(fake_bin) != NULL);
   char fake_tmux[MAX_PATH_LEN];
   snprintf(fake_tmux, sizeof(fake_tmux), "%s/tmux", fake_bin);
   FILE *tmux = fopen(fake_tmux, "w");
   assert(tmux != NULL);
   fputs("#!/bin/sh\nexit 0\n", tmux);
   fclose(tmux);
   assert(chmod(fake_tmux, 0700) == 0);

   const char *old_path_env = getenv("PATH");
   char *old_path = old_path_env ? strdup(old_path_env) : NULL;
   size_t path_len = strlen(fake_bin) + 2 + (old_path ? strlen(old_path) : 0);
   char *new_path = malloc(path_len);
   assert(new_path != NULL);
   snprintf(new_path, path_len, "%s:%s", fake_bin, old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   free(new_path);

   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"codex-cli\",\"roles\":[\"code\"],"
            "\"backend\":\"cli-stdio\",\"cli_kind\":\"codex\",\"cli_cmd\":\"codex\","
            "\"cli_idle_timeout_ms\":1234,\"session_reuse\":true},"
            "{\"name\":\"claude\",\"provider\":\"claude\",\"roles\":[\"code\"],"
            "\"backend\":\"provider-cli\",\"cli_kind\":\"claude\",\"cli_cmd\":\"claude-p\"},"
            "{\"name\":\"mistral-plan\",\"provider\":\"mistral\","
            "\"roles\":[\"code\",\"review\",\"explain\",\"refactor\",\"draft\",\"execute\"],"
            "\"backend\":\"provider-cli\",\"cli_kind\":\"mistral-plan\",\"cli_cmd\":\"vibe\","
            "\"cost_tier\":2},"
            "{\"name\":\"gemini-cli\",\"provider\":\"gemini\","
            "\"roles\":[\"code\"],\"backend\":\"provider-cli\","
            "\"cli_kind\":\"gemini\",\"cli_cmd\":\"gemini\"},"
            "{\"name\":\"mistral-cli\",\"provider\":\"mistral\","
            "\"roles\":[\"code\"],\"backend\":\"provider-cli\","
            "\"cli_kind\":\"mistral\",\"cli_cmd\":\"mistral\"},"
            "{\"name\":\"claude-code\",\"provider\":\"claude-code\","
            "\"roles\":[\"code\"],\"backend\":\"tmux-cli\","
            "\"cli_kind\":\"claude-code\",\"cli_cmd\":\"/bin/echo\","
            "\"session_reuse\":true}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   assert(loaded.agent_count == 6);
   assert(strcmp(loaded.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(loaded.agents[0].cli_kind, "codex") == 0);
   assert(strcmp(loaded.agents[0].cli_cmd, "codex") == 0);
   assert(loaded.agents[0].cli_idle_timeout_ms == 1234);
   assert(loaded.agents[0].session_reuse == 1);
   assert(strcmp(loaded.agents[1].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(loaded.agents[1].provider, "claude") == 0);
   assert(strcmp(loaded.agents[1].auth_type, "none") == 0);
   assert(loaded.agents[1].cli_kind[0] == '\0');
   assert(strcmp(loaded.agents[1].cli_cmd, "claude") == 0);
   assert(strcmp(loaded.agents[2].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(loaded.agents[2].provider, "mistral") == 0);
   assert(strcmp(loaded.agents[2].cli_kind, "mistral-plan") == 0);
   assert(strcmp(loaded.agents[2].cli_cmd, "vibe") == 0);
   assert(loaded.agents[2].cost_tier == 0);
   assert(strcmp(loaded.agents[2].roles[0], "code") == 0);
   assert(loaded.agents[2].exec_role_count == 0);
   assert(agent_is_exec_role(&loaded.agents[2], "code") == 1);
   assert(strcmp(loaded.agents[3].name, "gemini-cli") == 0);
   assert(strcmp(loaded.agents[3].provider, "gemini") == 0);
   assert(strcmp(loaded.agents[3].cli_kind, "gemini") == 0);
   assert(strcmp(loaded.agents[3].cli_cmd, "gemini") == 0);
   assert(strcmp(loaded.agents[4].name, "mistral-cli") == 0);
   assert(strcmp(loaded.agents[4].provider, "mistral") == 0);
   assert(strcmp(loaded.agents[4].cli_kind, "mistral") == 0);
   assert(strcmp(loaded.agents[4].cli_cmd, "mistral") == 0);
   assert(strcmp(loaded.agents[5].name, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].provider, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(loaded.agents[5].cli_kind, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].cli_cmd, "/bin/echo") == 0);
   assert(loaded.agents[5].session_reuse == 1);
   assert(agent_is_available_for_routing(&loaded.agents[5]) == 1);

   assert(agent_save_config(&loaded) == 0);

   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   assert(reloaded.agent_count == 6);
   assert(strcmp(reloaded.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(reloaded.agents[0].cli_kind, "codex") == 0);
   assert(strcmp(reloaded.agents[0].cli_cmd, "codex") == 0);
   assert(reloaded.agents[0].cli_idle_timeout_ms == 1234);
   assert(reloaded.agents[0].session_reuse == 1);
   assert(strcmp(reloaded.agents[1].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(reloaded.agents[1].provider, "claude") == 0);
   assert(strcmp(reloaded.agents[1].auth_type, "none") == 0);
   assert(reloaded.agents[1].cli_kind[0] == '\0');
   assert(strcmp(reloaded.agents[1].cli_cmd, "claude") == 0);
   assert(strcmp(reloaded.agents[2].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(reloaded.agents[2].provider, "mistral") == 0);
   assert(strcmp(reloaded.agents[2].cli_kind, "mistral-plan") == 0);
   assert(strcmp(reloaded.agents[2].cli_cmd, "vibe") == 0);
   assert(reloaded.agents[2].cost_tier == 0);
   assert(strcmp(reloaded.agents[2].roles[0], "code") == 0);
   assert(reloaded.agents[2].exec_role_count == 0);
   assert(agent_is_exec_role(&reloaded.agents[2], "code") == 1);
   assert(strcmp(reloaded.agents[3].name, "gemini-cli") == 0);
   assert(strcmp(reloaded.agents[3].provider, "gemini") == 0);
   assert(strcmp(reloaded.agents[3].cli_kind, "gemini") == 0);
   assert(strcmp(reloaded.agents[3].cli_cmd, "gemini") == 0);
   assert(strcmp(reloaded.agents[4].name, "mistral-cli") == 0);
   assert(strcmp(reloaded.agents[4].provider, "mistral") == 0);
   assert(strcmp(reloaded.agents[4].cli_kind, "mistral") == 0);
   assert(strcmp(reloaded.agents[4].cli_cmd, "mistral") == 0);
   assert(strcmp(reloaded.agents[5].name, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].provider, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(reloaded.agents[5].cli_kind, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].cli_cmd, "/bin/echo") == 0);
   assert(reloaded.agents[5].session_reuse == 1);
   assert(agent_is_available_for_routing(&reloaded.agents[5]) == 1);

   if (old_path)
   {
      setenv("PATH", old_path, 1);
      free(old_path);
   }
   else
   {
      unsetenv("PATH");
   }
}

static void test_agent_adapter_registry(void)
{
   const agent_adapter_t *codex = agent_adapter_for_name("codex");
   assert(codex != NULL);
   assert(strcmp(codex->provider, "chatgpt") == 0);
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_DELEGATE_TURN));
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION));
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_DIRECT_HTTP));

   const agent_adapter_t *mistral = agent_adapter_for_provider("mistral");
   assert(mistral != NULL);
   assert(strcmp(mistral->name, "mistral") == 0);
   assert(agent_adapter_supports(mistral, AGENT_ADAPTER_CAP_PRIMARY_SESSION));

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "codex");
   snprintf(ag.provider, sizeof(ag.provider), "chatgpt");
   assert(agent_adapter_for_agent(&ag) == codex);
   assert(agent_adapter_agent_is_direct(&ag) == 1);

   snprintf(ag.backend, sizeof(ag.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   assert(agent_adapter_agent_is_direct(&ag) == 0);
}

static void test_provider_cli_shell_exec_uses_argv_not_shell(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "cat-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/cat");
   ag.cli_idle_timeout_ms = 5000;

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Cat CLI", NULL, "system", "user",
                                         &result) == 0);
   assert(result.success == 1);
   assert(result.response != NULL);
   assert(strcmp(result.response, "system\n\nuser") == 0);
   free(result.response);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "bad-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/cat ; /bin/echo injected");
   ag.cli_idle_timeout_ms = 5000;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Bad CLI", NULL, NULL, "user", &result) !=
          0);
   assert(strstr(result.error, "shell operators") != NULL);
}

static void test_provider_cli_shell_timeout_covers_prompt_write(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "sleep-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/sleep 2");
   ag.cli_idle_timeout_ms = 100;

   char *large_prompt = malloc(256 * 1024);
   assert(large_prompt != NULL);
   memset(large_prompt, 'x', (256 * 1024) - 1);
   large_prompt[(256 * 1024) - 1] = '\0';

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Sleep CLI", NULL, NULL, large_prompt,
                                         &result) != 0);
   assert(strstr(result.error, "timed out") != NULL);
   free(large_prompt);
}

static void test_codex_oauth_auth_resolution(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/codex-auth.json", cfg_dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("{\"access_token\":\"codex-test-token\"}\n", f);
   fclose(f);

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.auth_type, sizeof(ag.auth_type), "codex-oauth");

   char auth[MAX_API_KEY_LEN + 32];
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer codex-test-token") == 0);

   assert(unlink(path) == 0);
   const char *home = getenv("HOME");
   assert(home != NULL && home[0]);

   char codex_dir[MAX_PATH_LEN];
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", home);
   assert(platform_mkdir_p(codex_dir, 0700) == 0 || access(codex_dir, F_OK) == 0);

   char codex_path[MAX_PATH_LEN];
   snprintf(codex_path, sizeof(codex_path), "%s/auth.json", codex_dir);
   f = fopen(codex_path, "w");
   assert(f != NULL);
   fputs("{\"tokens\":{\"access_token\":\"codex-cli-token\"}}\n", f);
   fclose(f);

   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer codex-cli-token") == 0);
}

static void test_agent_is_exec_role(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));

   /* No explicit exec_roles: use defaults */
   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 1);
   assert(agent_is_exec_role(&agent, "test") == 1);
   assert(agent_is_exec_role(&agent, "diagnose") == 1);
   assert(agent_is_exec_role(&agent, "execute") == 1);
   assert(agent_is_exec_role(&agent, "code") == 1);
   assert(agent_is_exec_role(&agent, "refactor") == 1);
   assert(agent_is_exec_role(&agent, "draft") == 1);
   assert(agent_is_exec_role(&agent, "implement") == 1);
   assert(agent_is_exec_role(&agent, "summarize") == 0);

   /* With explicit exec_roles */
   strcpy(agent.exec_roles[0], "deploy");
   strcpy(agent.exec_roles[1], "custom_role");
   agent.exec_role_count = 2;

   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "custom_role") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 0);
   assert(agent_is_exec_role(&agent, "execute") == 0);
}

static void test_tool_bash(void)
{
   /* Basic echo */
   char *result = tool_bash("echo hello", 5000);
   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *out = cJSON_GetObjectItem(json, "stdout");
   assert(out && cJSON_IsString(out));
   assert(strstr(out->valuestring, "hello") != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Non-zero exit code */
   result = tool_bash("exit 42", 5000);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 42);
   cJSON_Delete(json);
   free(result);

   /* Timeout */
   result = tool_bash("sleep 60", 200);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == -1);
   cJSON_Delete(json);
   free(result);

   result = tool_bash("yes x | head -c 65536", 5000);
   assert(result && strstr(result, "\"exit_code\":0") != NULL);
   free(result);
}

/* A write into a source checkout is redirected into an aimee-managed worktree
 * by guardrails (pre_tool_check returns 1 with the rewritten path) under the
 * default (shared) provider. When a `detached` workspace provider is active the
 * tool marshals to the serving client (file tools + tool_bash -> exec_shell), so
 * the server-side worktree rewrite must be SKIPPED — otherwise the path/command
 * would be re-pointed at a checkout that does not exist on the client. This
 * locks in the guardrails skip that lets detached delegates operate on the
 * client's live tree. */
static void test_detached_skips_worktree_rewrite(void)
{
   char repo[256];
   snprintf(repo, sizeof(repo), "/tmp/det_wt_test.XXXXXX");
   assert(mkdtemp(repo) != NULL);
   char shellcmd[512];
   snprintf(shellcmd, sizeof(shellcmd), "git init -q '%s' >/dev/null 2>&1", repo);
   (void)system(shellcmd);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char input[512];
   snprintf(input, sizeof(input), "{\"file_path\":\"%s/foo.c\",\"content\":\"int x;\"}", repo);
   char msg[1024];

   /* Shared (default) provider: the write is redirected into a worktree. */
   workspace_provider_clear_active();
   msg[0] = '\0';
   int rc_shared = pre_tool_check("Write", input, &state, MODE_APPROVE, repo, msg, sizeof(msg));
   assert(rc_shared == 1);                /* allow-with-rewrite */
   assert(strstr(msg, ".aimee") != NULL); /* rewritten into an aimee worktree */

   /* Detached provider active: the rewrite is skipped (path left untouched). */
   workspace_provider_t fake_detached;
   memset(&fake_detached, 0, sizeof(fake_detached));
   fake_detached.kind = WS_PROVIDER_DETACHED;
   workspace_provider_set_active(&fake_detached);
   msg[0] = '\0';
   int rc_detached = pre_tool_check("Write", input, &state, MODE_APPROVE, repo, msg, sizeof(msg));
   workspace_provider_clear_active();
   assert(rc_detached != 1);              /* allow-with-rewrite did NOT fire */
   assert(strstr(msg, ".aimee") == NULL); /* no worktree path was injected */
   /* NB: rc_detached here reflects the orthogonal, config-based write gate
    * (cwd_is_detached_workspace) which this test's bare temp repo is not
    * registered for. In production workspace_turn_bind_active and
    * cwd_is_detached_workspace read the SAME workspace registry, so a real
    * detached turn lifts both and the write is allowed (rc 0). What this test
    * pins is the one thing the provider bind controls: the worktree rewrite. */

   snprintf(shellcmd, sizeof(shellcmd), "rm -rf '%s'", repo);
   (void)system(shellcmd);

   printf("  detached_skips_worktree_rewrite: ok (shared=%d, detached=%d)\n", rc_shared,
          rc_detached);
}

static void test_tool_read_file(void)
{
   /* Write a temp file */
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_read_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);
   const char *content = "line1\nline2\nline3\nline4\n";
   if (write(fd, content, strlen(content)) < 0)
   { /* ignore */
   }
   close(fd);

   /* Read entire file */
   char *result = tool_read_file(tmppath, 0, 0);
   assert(result != NULL);
   assert(strcmp(result, content) == 0);
   free(result);

   /* Read with offset */
   result = tool_read_file(tmppath, 1, 0);
   assert(result != NULL);
   assert(strcmp(result, "line2\nline3\nline4\n") == 0);
   free(result);

   /* Read with limit */
   result = tool_read_file(tmppath, 0, 2);
   assert(result != NULL);
   assert(strcmp(result, "line1\nline2\n") == 0);
   free(result);

   /* Nonexistent path */
   result = tool_read_file("/nonexistent/path/file.txt", 0, 0);
   assert(result != NULL);
   assert(strstr(result, "error: cannot open") != NULL);
   free(result);

   unsigned char binary_content[] = {0x7f, 'E', 'L', 'F', '\0', 0x01, 0x02, 0x03};
   assert((fd = open(tmppath, O_WRONLY | O_TRUNC)) >= 0);
   assert(write(fd, binary_content, sizeof(binary_content)) == (ssize_t)sizeof(binary_content));
   close(fd);
   result = tool_read_file(tmppath, 0, 0);
   assert(result != NULL);
   assert(strstr(result, "error: binary file omitted") != NULL);
   assert(memchr(result, 0x7f, strlen(result)) == NULL);
   free(result);

   unlink(tmppath);
}

static void test_tool_write_file(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_write_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);

   char *result = tool_write_file(tmppath, "hello world");
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *status = cJSON_GetObjectItem(json, "status");
   cJSON *changed = cJSON_GetObjectItem(json, "changed");
   cJSON *summary = cJSON_GetObjectItem(json, "summary");
   cJSON *diff = cJSON_GetObjectItem(json, "diff");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   assert(changed && cJSON_IsTrue(changed));
   assert(summary && strstr(summary->valuestring, "+1") != NULL);
   assert(diff && cJSON_GetObjectItem(diff, "additions")->valueint == 1);
   assert(cJSON_GetObjectItem(diff, "deletions")->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Verify contents */
   char *readback = tool_read_file(tmppath, 0, 0);
   assert(readback != NULL);
   assert(strcmp(readback, "hello world") == 0);
   free(readback);

   /* Overwrite and verify structured diff output */
   result = tool_write_file(tmppath, "hello changed");
   assert(result != NULL);
   json = parse_json_or_die(result);
   status = cJSON_GetObjectItem(json, "status");
   changed = cJSON_GetObjectItem(json, "changed");
   summary = cJSON_GetObjectItem(json, "summary");
   diff = cJSON_GetObjectItem(json, "diff");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   assert(changed && cJSON_IsTrue(changed));
   assert(summary && strstr(summary->valuestring, "+1") != NULL);
   assert(diff && cJSON_GetObjectItem(diff, "additions")->valueint == 1);
   assert(cJSON_GetObjectItem(diff, "deletions")->valueint == 1);
   cJSON_Delete(json);
   free(result);

   /* Write identical content — should return plain "ok" */
   result = tool_write_file(tmppath, "hello changed");
   assert(result != NULL);
   assert(strcmp(result, "ok") == 0);
   free(result);

   unlink(tmppath);
}

static void test_tool_edit_file(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_edit_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);

   char *result = tool_write_file(tmppath, "alpha\nbeta\ngamma\nbeta\n");
   assert(result != NULL);
   free(result);

   /* Unique replacement succeeds and returns a structured diff. */
   result = tool_edit_file(tmppath, "gamma", "GAMMA", 0);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *status = cJSON_GetObjectItem(json, "status");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   cJSON_Delete(json);
   free(result);
   char *readback = tool_read_file(tmppath, 0, 0);
   assert(readback && strcmp(readback, "alpha\nbeta\nGAMMA\nbeta\n") == 0);
   free(readback);

   /* old_string absent → error, file unchanged. */
   result = tool_edit_file(tmppath, "does-not-exist", "x", 0);
   assert(result != NULL && strncmp(result, "error:", 6) == 0);
   free(result);

   /* Non-unique old_string without replace_all → error, file unchanged. */
   result = tool_edit_file(tmppath, "beta", "B", 0);
   assert(result != NULL && strncmp(result, "error:", 6) == 0);
   free(result);
   readback = tool_read_file(tmppath, 0, 0);
   assert(readback && strcmp(readback, "alpha\nbeta\nGAMMA\nbeta\n") == 0);
   free(readback);

   /* replace_all rewrites every occurrence. */
   result = tool_edit_file(tmppath, "beta", "B", 1);
   assert(result != NULL && strncmp(result, "error:", 6) != 0);
   free(result);
   readback = tool_read_file(tmppath, 0, 0);
   assert(readback && strcmp(readback, "alpha\nB\nGAMMA\nB\n") == 0);
   free(readback);

   unlink(tmppath);
}

static void test_parent_write_guard_blocks_parent_writes(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char src_dir[512];
   snprintf(src_dir, sizeof(src_dir), "%s/src", root);
   assert(platform_mkdir_p(src_dir, 0700) == 0 || access(src_dir, F_OK) == 0);

   char aimee_dir[512];
   char worktrees_dir[512];
   char delegate_dir[512];
   char worktree[512];
   snprintf(aimee_dir, sizeof(aimee_dir), "%s/.aimee", root);
   snprintf(worktrees_dir, sizeof(worktrees_dir), "%s/worktrees", aimee_dir);
   snprintf(delegate_dir, sizeof(delegate_dir), "%s/delegate", worktrees_dir);
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   char parent_file[512];
   snprintf(parent_file, sizeof(parent_file), "%s/src/owned.c", root);
   FILE *f = fopen(parent_file, "w");
   assert(f != NULL);
   fputs("original", f);
   fclose(f);

   char worktree_file[512];
   snprintf(worktree_file, sizeof(worktree_file), "%s/owned.c", worktree);

   char sibling_file[512];
   snprintf(sibling_file, sizeof(sibling_file), "%s-sibling/owned.c", root);

   char root_with_slash[512];
   snprintf(root_with_slash, sizeof(root_with_slash), "%s/", root);
   agent_tools_parent_write_guard_set(root_with_slash, worktree);
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 1);
   assert(agent_tools_parent_write_guard_blocks("src/owned.c", root) == 1);
   assert(agent_tools_parent_write_guard_blocks(worktree_file, NULL) == 0);
   assert(agent_tools_parent_write_guard_blocks(sibling_file, NULL) == 0);

   char *result = tool_write_file(parent_file, "blocked");
   assert(result != NULL);
   assert(strstr(result, "parent worktree is read-only") != NULL);
   free(result);

   result = tool_read_file(parent_file, 0, 0);
   assert(result != NULL);
   assert(strcmp(result, "original") == 0);
   free(result);

   char parent_command_file[512];
   char verify_cmd[1024];
   snprintf(parent_command_file, sizeof(parent_command_file), "%s/src/command-owned.c", root);
   snprintf(verify_cmd, sizeof(verify_cmd), "touch %s", parent_command_file);
   result = tool_verify("command_succeeds", verify_cmd, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *pass = cJSON_GetObjectItem(json, "pass");
   cJSON *reason = cJSON_GetObjectItem(json, "reason");
   assert(pass && cJSON_IsFalse(pass));
   assert(reason && strstr(reason->valuestring, "parent worktree is read-only") != NULL);
   cJSON_Delete(json);
   free(result);
   assert(access(parent_command_file, F_OK) != 0);

   agent_tools_parent_write_guard_set(root_with_slash, root);
   assert(agent_tools_parent_write_guard_root() == NULL);
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 0);
   agent_tools_parent_write_guard_set(root_with_slash, worktree);

   result = tool_write_file(worktree_file, "allowed");
   assert(result != NULL);
   assert(strstr(result, "error:") == NULL);
   free(result);

   result = tool_read_file(worktree_file, 0, 0);
   assert(result != NULL);
   assert(strcmp(result, "allowed") == 0);
   free(result);

   run_cmd_set_cwd(worktree);
   result = tool_bash("pwd", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, worktree) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char worktree_subdir[512];
   snprintf(worktree_subdir, sizeof(worktree_subdir), "%s/subdir", worktree);
   assert(mkdir(worktree_subdir, 0700) == 0);
   run_cmd_set_cwd(worktree);
   char cd_pwd_cmd[1024];
   snprintf(cd_pwd_cmd, sizeof(cd_pwd_cmd), "cd %s && pwd", worktree_subdir);
   result = tool_bash(cd_pwd_cmd, 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, worktree_subdir) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char script_path[512];
   char wrote_path[512];
   snprintf(script_path, sizeof(script_path), "%s/write-ok.sh", worktree);
   snprintf(wrote_path, sizeof(wrote_path), "%s/wrote.txt", worktree);
   f = fopen(script_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\nprintf allowed > wrote.txt\n", f);
   fclose(f);
   assert(chmod(script_path, 0700) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("./write-ok.sh", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   assert(access(wrote_path, F_OK) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("make --version", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char fake_aimee_path[512];
   char old_path[8192] = "";
   int had_old_path = 0;
   snprintf(fake_aimee_path, sizeof(fake_aimee_path), "%s/aimee", worktree);
   f = fopen(fake_aimee_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf index-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = memory ] && [ \"$2\" = search ]; then\n"
         "  printf memory-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = delegate ] && [ \"$2\" = status ]; then\n"
         "  printf delegate-status-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = jobs ] && [ \"$2\" = logs ]; then\n"
         "  printf jobs-logs-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(fake_aimee_path, 0700) == 0);
   const char *path_env = getenv("PATH");
   if (path_env)
   {
      had_old_path = 1;
      snprintf(old_path, sizeof(old_path), "%s", path_env);
   }
   char new_path[8704];
   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   char cd_index_cmd[1024];
   snprintf(cd_index_cmd, sizeof(cd_index_cmd), "cd %s && aimee index overview", worktree);
   result = tool_bash(cd_index_cmd, 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview && aimee memory search delegate", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(strstr(stdout_item->valuestring, "memory-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee delegate status 123 && aimee jobs logs 123", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "delegate-status-ok") != NULL);
   assert(strstr(stdout_item->valuestring, "jobs-logs-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   const char *home = getenv("HOME");
   assert(home && home[0]);
   char external_bin[512];
   char external_aimee[512];
   snprintf(external_bin, sizeof(external_bin), "%s/.aimee-test-external-bin-XXXXXX", home);
   assert(platform_mkdtemp(external_bin) != NULL);
   snprintf(external_aimee, sizeof(external_aimee), "%s/aimee", external_bin);
   f = fopen(external_aimee, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf external-index-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(external_aimee, 0700) == 0);

   snprintf(new_path, sizeof(new_path), "%s:%s", external_bin, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "external-index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(external_bin);

   char saved_home[512] = "";
   int had_home = 0;
   if (home && home[0])
   {
      had_home = 1;
      snprintf(saved_home, sizeof(saved_home), "%s", home);
   }
   char tmp_home[512];
   char tmp_local[512];
   char tmp_local_bin[512];
   char tmp_local_aimee[512];
   snprintf(tmp_home, sizeof(tmp_home), "%s/home", root);
   snprintf(tmp_local, sizeof(tmp_local), "%s/.local", tmp_home);
   snprintf(tmp_local_bin, sizeof(tmp_local_bin), "%s/bin", tmp_local);
   snprintf(tmp_local_aimee, sizeof(tmp_local_aimee), "%s/aimee", tmp_local_bin);
   assert(mkdir(tmp_home, 0700) == 0);
   assert(mkdir(tmp_local, 0700) == 0);
   assert(mkdir(tmp_local_bin, 0700) == 0);
   f = fopen(tmp_local_aimee, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf local-bin-index-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(tmp_local_aimee, 0700) == 0);

   setenv("HOME", tmp_home, 1);
   setenv("PATH", "/usr/bin:/bin", 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_home)
      setenv("HOME", saved_home, 1);
   else
      unsetenv("HOME");
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "local-bin-index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   agent_tools_parent_write_guard_clear();
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 0);

   unlink(tmp_local_aimee);
   unlink(fake_aimee_path);
   unlink(wrote_path);
   unlink(script_path);
   unlink(worktree_file);
   unlink(parent_file);
   rmdir(tmp_local_bin);
   rmdir(tmp_local);
   rmdir(tmp_home);
   rmdir(worktree);
   rmdir(delegate_dir);
   rmdir(worktrees_dir);
   rmdir(aimee_dir);
   rmdir(src_dir);
   rmdir(root);
}

static void test_parent_write_guard_shell_uses_delegate_cwd_in_git_parent(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_git_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char cmd[2048];
   int rc = 0;
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   char *out = run_cmd(cmd, &rc);
   free(out);
   assert(rc == 0);

   char parent_worktree[512];
   char delegate_worktree[512];
   snprintf(parent_worktree, sizeof(parent_worktree), "%s/.aimee/worktrees/parent/main", root);
   snprintf(delegate_worktree, sizeof(delegate_worktree), "%s/.aimee/worktrees/delegate/main",
            root);
   assert(platform_mkdir_p(parent_worktree, 0700) == 0 || access(parent_worktree, F_OK) == 0);
   assert(platform_mkdir_p(delegate_worktree, 0700) == 0 || access(delegate_worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(parent_worktree, delegate_worktree);
   run_cmd_set_cwd(delegate_worktree);

   char *result = tool_bash("pwd", 5000);
   run_cmd_set_cwd(NULL);

   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, delegate_worktree) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   run_cmd_set_cwd(delegate_worktree);
   result = tool_bash("git rev-parse --show-prefix", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, ".aimee/worktrees/delegate/main/") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(root);
}
static void test_tool_list_files(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_list_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char alpha[512], beta[512], dockerfile[512], compose[512];
   snprintf(alpha, sizeof(alpha), "%s/alpha.txt", tmpdir);
   snprintf(beta, sizeof(beta), "%s/beta.log", tmpdir);
   snprintf(dockerfile, sizeof(dockerfile), "%s/Dockerfile.test", tmpdir);
   snprintf(compose, sizeof(compose), "%s/compose.yaml", tmpdir);
   const char *fixtures[][2] = {
       {alpha, "alpha"}, {beta, "beta"}, {dockerfile, "from scratch"}, {compose, "services: {}"}};
   for (int i = 0; i < 4; i++)
   {
      FILE *f = fopen(fixtures[i][0], "w");
      assert(f != NULL);
      fputs(fixtures[i][1], f);
      fclose(f);
   }
   char *result = tool_list_files(tmpdir, "*.txt");
   assert(result != NULL);
   assert(strstr(result, "alpha.txt") != NULL);
   assert(strstr(result, "beta.log") == NULL);
   free(result);
   result = tool_list_files(tmpdir, "**/Dockerfile*");
   assert(result != NULL);
   assert(strstr(result, "Dockerfile.test") != NULL);
   assert(strstr(result, "compose.yaml") == NULL);
   free(result);
   result = tool_list_files("/nonexistent_dir_12345", NULL);
   assert(result != NULL);
   assert(result[0] == '\0');
   free(result);
   unlink(alpha);
   unlink(beta);
   unlink(dockerfile);
   unlink(compose);
   rmdir(tmpdir);
}
static void test_tool_grep_excludes_heavy_dirs(void)
{
   const char *excluded_dirs[] = {".git", ".aimee", "build", "dist", "node_modules"};
   char tmpdir[512], heavy_dirs[5][512], alpha[512], vendored[5][512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_grep_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      snprintf(heavy_dirs[i], sizeof(heavy_dirs[i]), "%s/%s", tmpdir, excluded_dirs[i]);
      assert(mkdir(heavy_dirs[i], 0700) == 0);
   }
   snprintf(alpha, sizeof(alpha), "%s/alpha.txt", tmpdir);

   FILE *f = fopen(alpha, "w");
   assert(f != NULL);
   fputs("needle normal\n", f);
   fclose(f);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      snprintf(vendored[i], sizeof(vendored[i]), "%s/ignored.txt", heavy_dirs[i]);
      f = fopen(vendored[i], "w");
      assert(f != NULL);
      fputs("needle vendored\n", f);
      fclose(f);
   }

   char *result = tool_grep(tmpdir, "needle", 20);
   assert(result != NULL);
   assert(strstr(result, "alpha.txt") != NULL);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
      assert(strstr(result, excluded_dirs[i]) == NULL);
   free(result);

   unlink(alpha);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      unlink(vendored[i]);
      rmdir(heavy_dirs[i]);
   }
   rmdir(tmpdir);
}

static void test_dispatch_tool_call(void)
{
   /* bash tool */
   char *result = dispatch_tool_call("bash", "{\"command\":\"echo dispatch_test\"}", 5000);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "dispatch_test") != NULL);
   cJSON_Delete(json);
   free(result);

   /* Unknown tool */
   result = dispatch_tool_call("unknown_tool", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: unknown tool") != NULL);
   free(result);

   /* Unknown tool with suggestion */
   result = dispatch_tool_call("read_flie", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "Did you mean 'read_file'?") != NULL);
   free(result);

   result = dispatch_tool_call("writ_file", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "Did you mean 'write_file'?") != NULL);
   free(result);

   /* Argument alias: "cmd" → "command" */
   result = dispatch_tool_call("bash", "{\"cmd\":\"echo alias_test\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "alias_test") != NULL);
   assert(strstr(result, "error") == NULL);
   free(result);

   result = dispatch_tool_call(
       "execute_script",
       "{\"language\":\"python\",\"body\":\"print('script_dispatch')\",\"timeout_secs\":5}", 5000);
   assert(result != NULL);
   json = parse_json_or_die(result);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "script_dispatch") != NULL);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Argument alias: "filepath" → "file_path" then falls through to "path" */
   result = dispatch_tool_call("read_file", "{\"filepath\":\"/dev/null\"}", 5000);
   assert(result != NULL);
   /* Should not fail with "missing 'path'" */
   assert(strstr(result, "missing") == NULL);
   free(result);

   /* Type coercion: string "5" → integer for offset */
   result = dispatch_tool_call("read_file", "{\"path\":\"/dev/null\",\"offset\":\"5\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error") == NULL);
   free(result);

   /* Write file error includes recovery hint */
   result = dispatch_tool_call("write_file", "{\"path\":\"/nonexistent/dir/file\"}", 5000);
   assert(result != NULL);
   if (strstr(result, "error"))
      assert(strstr(result, "Recovery:") != NULL);
   free(result);

   /* Missing parameter */
   result = dispatch_tool_call("bash", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: missing 'command' parameter") != NULL);
   free(result);
}

#include "test_agent_source_authority.inc"

static void test_parse_openai_tool_calls(void)
{
   /* Build a mock OpenAI response with tool_calls */
   const char *mock_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"tool_calls\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": null,"
                           "      \"tool_calls\": [{"
                           "        \"id\": \"call_abc123\","
                           "        \"type\": \"function\","
                           "        \"function\": {"
                           "          \"name\": \"bash\","
                           "          \"arguments\": \"{\\\"command\\\":\\\"ls\\\"}\""
                           "        }"
                           "      }]"
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 100, \"completion_tokens\": 50}"
                           "}";

   /* We need to call the internal parse function. Since it's static,
    * we test indirectly by verifying the cJSON structure matches what
    * the parser expects. This validates the JSON format contract. */
   cJSON *root = cJSON_Parse(mock_json);
   assert(root != NULL);

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   assert(choices && cJSON_GetArraySize(choices) == 1);

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "tool_calls") == 0);

   cJSON *message = cJSON_GetObjectItem(choice, "message");
   cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
   assert(tool_calls && cJSON_GetArraySize(tool_calls) == 1);

   cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
   cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
   assert(tc_id && strcmp(tc_id->valuestring, "call_abc123") == 0);

   cJSON *fn = cJSON_GetObjectItem(tc, "function");
   cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
   assert(fn_name && strcmp(fn_name->valuestring, "bash") == 0);

   cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
   assert(fn_args && cJSON_IsString(fn_args));
   cJSON *args_parsed = cJSON_Parse(fn_args->valuestring);
   assert(args_parsed != NULL);
   cJSON *cmd = cJSON_GetObjectItem(args_parsed, "command");
   assert(cmd && strcmp(cmd->valuestring, "ls") == 0);
   cJSON_Delete(args_parsed);

   cJSON_Delete(root);

   /* Test a text response (no tool calls) */
   const char *text_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"stop\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": \"Task complete.\""
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 200, \"completion_tokens\": 10}"
                           "}";

   root = cJSON_Parse(text_json);
   assert(root != NULL);
   choices = cJSON_GetObjectItem(root, "choices");
   choice = cJSON_GetArrayItem(choices, 0);
   finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "stop") == 0);
   message = cJSON_GetObjectItem(choice, "message");
   cJSON *content = cJSON_GetObjectItem(message, "content");
   assert(content && strcmp(content->valuestring, "Task complete.") == 0);
   cJSON_Delete(root);
}

/* --- Shared path-policy tests (traversal and sensitive-path rejection) --- */

static void test_path_traversal_rejected(void)
{
   /* read_file with traversal should be rejected */
   char *result = tool_read_file("/tmp/../etc/shadow", 0, 0);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* write_file with traversal */
   result = tool_write_file("/tmp/../etc/shadow", "hacked");
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* dispatch traversal via read_file */
   result = dispatch_tool_call("read_file", "{\"path\":\"/tmp/../../etc/shadow\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* list_files with traversal in path */
   result = tool_list_files("/tmp/../etc", NULL);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);
}

static void test_sensitive_path_rejected(void)
{
   /* read_file: .ssh directory */
   char ssh_path[256];
   const char *home = getenv("HOME");
   if (home)
   {
      snprintf(ssh_path, sizeof(ssh_path), "%s/.ssh/id_rsa", home);
      char *result = tool_read_file(ssh_path, 0, 0);
      assert(result != NULL);
      /* Should be rejected as sensitive or fail to open */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
   }
}

static void test_symlink_escape_rejected(void)
{
   /* Create a symlink pointing to /etc/shadow and try to read through it */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_symlink_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char link_path[512];
   snprintf(link_path, sizeof(link_path), "%s/sneaky_link", tmpdir);

   /* Create symlink to a sensitive path */
   if (symlink("/etc/shadow", link_path) == 0)
   {
      char *result = tool_read_file(link_path, 0, 0);
      assert(result != NULL);
      /* Should be blocked by the realpath-based check */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
      unlink(link_path);
   }

   rmdir(tmpdir);
}

/* --- messages_compact_consecutive tests --- */

static cJSON *make_msg(const char *role, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", role);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

#include "test_agent_compact.inc"

static void test_delegation_error_guidance(void)
{
   char buf[512];

   /* Known patterns return guidance */
   assert(delegation_error_guidance("missing prompt", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "non-empty prompt") != NULL);

   assert(delegation_error_guidance("prompt too short (5 chars)", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "too brief") != NULL);

   assert(delegation_error_guidance("no agent available for role 'xyzzy'", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "valid role") != NULL);

   assert(delegation_error_guidance("missing delegation_id or content", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "delegate_reply") != NULL);

   assert(delegation_error_guidance("delegation depth limit exceeded (3/3)", buf, sizeof(buf)) ==
          1);
   assert(strstr(buf, "max_delegation_depth") != NULL);

   assert(delegation_error_guidance(
              "delegation spawn limit exceeded (51/50 nested delegates for root)", buf,
              sizeof(buf)) == 1);
   assert(strstr(buf, "max_delegation_spawns") != NULL);
   assert(strstr(buf, "sub-delegation") != NULL);

   /* Unknown errors produce no guidance */
   assert(delegation_error_guidance("internal error", buf, sizeof(buf)) == 0);
   assert(buf[0] == '\0');

   /* NULL / empty inputs */
   assert(delegation_error_guidance(NULL, buf, sizeof(buf)) == 0);
   assert(delegation_error_guidance("missing prompt", NULL, 0) == 0);
   assert(delegation_error_guidance("missing prompt", buf, 0) == 0);
}

static void test_agent_trace_log_uses_db1_execution_trace(void)
{
   /* Earlier tests in this suite exercise dispatch_tool_call, which now
    * lazy-inits DB1 against the real config db_path. Tear that down so
    * this test's :memory: init actually takes effect. */
   db1_shutdown();

   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);
   assert(db1_init(":memory:") == 0);

   agent_trace_log(7, 3, "call", "content", "bash", "{}", "ok", "abc123");

   db1_execution_trace_recent_row_t rows[4];
   int count = db1_execution_trace_list_recent(rows, 4);
   assert(count == 1);
   assert(rows[0].turn == 3);
   assert(strcmp(rows[0].tool_name, "bash") == 0);

   db1_shutdown();
   db1_stmt_cache_clear();
   sqlite3_close(db);
}

static void test_agent_name_valid(void)
{
   /* legit agent/model slugs accepted */
   assert(agent_name_valid("minimax"));
   assert(agent_name_valid("mimo-2.5"));
   assert(agent_name_valid("my-gpt"));
   assert(agent_name_valid("a"));
   assert(agent_name_valid("gpt_4.1-mini"));
   /* junk rejected: empty, leading non-alnum, spaces, illegal chars, over-long */
   assert(!agent_name_valid(""));
   assert(!agent_name_valid(NULL));
   assert(!agent_name_valid("-leading"));
   assert(!agent_name_valid(".dot"));
   assert(!agent_name_valid("has space"));
   assert(!agent_name_valid("bad/slash"));
   assert(!agent_name_valid(
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); /* 63 */
}

int main(void)
{
   char tmp_home[512];
   snprintf(tmp_home, sizeof(tmp_home), "%s/aimee-test-agent-home-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmp_home) != NULL);
   assert(platform_setenv("HOME", tmp_home) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(config_output_dir()[0] != '\0');
   session_id_set_override("unit-test-agent");
   test_tool_surface_single_source();
   test_agent_name_valid();
   test_agent_expand_env();
   test_agent_has_role();
   test_agent_find();
   test_agent_route();
   test_agent_route_with_caps_honors_tools_enabled();
   test_agent_route_with_caps_honors_context_override();
   test_current_code_only_role_tool_policy();
   test_current_code_only_dispatch_blocks_stale_context_tools();
   test_provider_env_credentials_and_headers();
   test_codex_oauth_request_creds();
   test_request_creds_snapshot_carries_vault_principal();
   test_agent_config_provider_cli_roundtrip();
   test_tools_enabled_capability_default();
   test_agent_adapter_registry();
   test_provider_cli_shell_exec_uses_argv_not_shell();
   test_provider_cli_shell_timeout_covers_prompt_write();
   test_codex_oauth_auth_resolution();
   test_responses_parser_keeps_all_output_text_parts();
   test_responses_parser_accumulates_output_text_deltas();
   test_responses_parser_uses_output_text_done();
   test_responses_parser_separates_message_items();
   test_agent_is_exec_role();
   test_tool_bash();
   test_detached_skips_worktree_rewrite();
   test_tool_read_file();
   test_tool_write_file();
   test_tool_edit_file();
   test_parent_write_guard_blocks_parent_writes();
   test_parent_write_guard_readonly_pipeline();
   test_parent_write_guard_allows_mkdir_in_delegate_worktree();
   test_parent_write_guard_allows_workspace_file_ops();
   test_parent_write_guard_allows_workspace_chain();
   test_parent_write_guard_allows_readonly_printf();
   test_parent_write_guard_shell_uses_delegate_cwd_in_git_parent();
   test_tool_list_files();
   test_tool_grep_excludes_heavy_dirs();
   test_dispatch_tool_call();
   test_source_authority_overlay_tools();
   test_source_authority_tls_thread_isolation();
   test_parse_openai_tool_calls();
   test_path_traversal_rejected();
   test_sensitive_path_rejected();
   test_symlink_escape_rejected();
   test_compact_empty();
   test_compact_single();
   test_compact_two_same_role();
   test_compact_five_same_role();
   test_compact_mixed_roles();
   test_compact_no_consecutive();
   test_compact_idempotent();
   test_compact_skips_structured_content();
   test_compact_skips_openai_tool_results();
   test_compact_system_role();
   test_delegation_error_guidance();
   test_cancelled_durable_job_blocks_tool_dispatch();
   test_delegate_bash_cancel_kills_running_tool();
   test_parent_write_guard_readonly_large_find();
   test_agent_trace_log_uses_db1_execution_trace();

   /* Kill any aimee-kb daemon these tests autostarted under tmp_home and remove
    * the scratch home, so the daemon is not orphaned. Leaked aimee-kb daemons
    * retry DB2 provisioning and were the source of the stuck-`createdb` runaway
    * (#2569); tmp_home is a unique mkdtemp path, so this targets only this
    * test's own daemon. */
   {
      char cmd[640];
      snprintf(cmd, sizeof(cmd), "pkill -KILL -f 'aimee-kb --socket=%s' 2>/dev/null", tmp_home);
      (void)system(cmd);
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_home);
      (void)system(cmd);
   }
   printf("agent: all tests passed\n");
   return 0;
}
