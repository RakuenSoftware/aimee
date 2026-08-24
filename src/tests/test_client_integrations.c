/* test_client_integrations.c: Claude MCP registration, Codex plugin payload,
 * and non-destructive settings update tests for client_integrations.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "cJSON.h"

/* Include the source directly to test static functions */
#include "../client_integrations.c"
#include "platform_test_util.h"

static int g_test_integrations_enabled = 1;
static const char *g_test_transport_preference = "cli-first";

static cJSON *test_client_config_value(const char *key)
{
   if (!strcmp(key, "client_integrations_enabled"))
      return cJSON_CreateBool(g_test_integrations_enabled);
   if (!strcmp(key, "client_tool_transport_preference"))
      return cJSON_CreateString(g_test_transport_preference);
   if (!strcmp(key, "subagent_ban_enabled"))
      return cJSON_CreateBool(1);
   return NULL;
}

/* --- Test build_marketplace_root --- */

static void test_build_marketplace_root(void)
{
   cJSON *root = build_marketplace_root();
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Should have name = "local" */
   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   /* Should have interface.displayName */
   cJSON *iface = cJSON_GetObjectItem(root, "interface");
   assert(cJSON_IsObject(iface));
   cJSON *dn = cJSON_GetObjectItem(iface, "displayName");
   assert(cJSON_IsString(dn));

   /* Should have empty plugins array */
   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 0);

   cJSON_Delete(root);
}

/* --- Test build_aimee_plugin_entry --- */

static void test_build_aimee_plugin_entry(void)
{
   cJSON *entry = build_aimee_plugin_entry();
   assert(entry != NULL);
   assert(cJSON_IsObject(entry));

   cJSON *name = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "aimee") == 0);

   cJSON *source = cJSON_GetObjectItem(entry, "source");
   assert(cJSON_IsObject(source));
   cJSON *src_path = cJSON_GetObjectItem(source, "path");
   assert(cJSON_IsString(src_path));
   assert(strstr(src_path->valuestring, "plugins/aimee") != NULL);

   cJSON *policy = cJSON_GetObjectItem(entry, "policy");
   assert(cJSON_IsObject(policy));
   cJSON *install = cJSON_GetObjectItem(policy, "installation");
   assert(cJSON_IsString(install));
   assert(strcmp(install->valuestring, "INSTALLED_BY_DEFAULT") == 0);

   cJSON *category = cJSON_GetObjectItem(entry, "category");
   assert(cJSON_IsString(category));
   assert(strcmp(category->valuestring, "Coding") == 0);

   cJSON_Delete(entry);
}

static void test_tool_transport_registration_plan(void)
{
   client_tool_registration_plan_t plan =
       client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_CLI_FIRST, 1, 1, 0, 0);
   assert(plan.cli == 1 && plan.mcp == 0);

   plan = client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_CLI_FIRST, 0, 1, 0, 0);
   assert(plan.cli == 0 && plan.mcp == 1); /* CLI cannot register -> MCP fallback */

   plan = client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_MCP_FIRST, 1, 1, 0, 0);
   assert(plan.cli == 0 && plan.mcp == 1);

   plan = client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_MCP_FIRST, 1, 0, 0, 0);
   assert(plan.cli == 1 && plan.mcp == 0); /* MCP cannot register -> CLI fallback */

   /* One-surface module capabilities override the preference only for their own
    * surface. Distinct capabilities may therefore require both registrations,
    * while no dual-surface capability is duplicated. */
   plan = client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_CLI_FIRST, 1, 1, 0, 1);
   assert(plan.cli == 1 && plan.mcp == 1);
   plan = client_tool_registration_plan(CLIENT_TOOL_TRANSPORT_MCP_FIRST, 1, 1, 1, 0);
   assert(plan.cli == 1 && plan.mcp == 1);

   assert(client_tool_transport_parse("cli-first") == CLIENT_TOOL_TRANSPORT_CLI_FIRST);
   assert(client_tool_transport_parse("mcp-first") == CLIENT_TOOL_TRANSPORT_MCP_FIRST);
   assert(client_tool_transport_parse("mcp") == CLIENT_TOOL_TRANSPORT_MCP_FIRST);
   assert(client_tool_transport_parse("invalid") == CLIENT_TOOL_TRANSPORT_CLI_FIRST);
}

static void test_codex_cli_registration_is_command_first(void)
{
   char skill[4096];
   assert(format_codex_cli_skill(skill, sizeof(skill), NULL, "/opt/aimee/bin/aimee") == 0);
   assert(strstr(skill, "/opt/aimee/bin/aimee index investigate") != NULL);
   assert(strstr(skill, "/opt/aimee/bin/aimee index hybrid") != NULL);
   assert(strstr(skill, "/opt/aimee/bin/aimee index find") != NULL);
   assert(strstr(skill, "/opt/aimee/bin/aimee memory search") != NULL);
   assert(strstr(skill, "MCP tool `") == NULL);
}

static void test_codex_manifest_registers_selected_transports_only(void)
{
   char manifest[4096];
   client_tool_registration_plan_t plan = {.cli = 1, .mcp = 0};
   format_codex_plugin_json(manifest, sizeof(manifest), 0, plan, NULL, "/opt/aimee/bin/aimee");
   cJSON *root = cJSON_Parse(manifest);
   assert(cJSON_IsObject(root));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "skills")));
   assert(cJSON_GetObjectItemCaseSensitive(root, "mcpServers") == NULL);
   cJSON *iface = cJSON_GetObjectItemCaseSensitive(root, "interface");
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(iface, "defaultPrompt")));
   cJSON_Delete(root);

   plan.cli = 0;
   plan.mcp = 1;
   format_codex_plugin_json(manifest, sizeof(manifest), 0, plan, NULL, "/opt/aimee/bin/aimee");
   root = cJSON_Parse(manifest);
   assert(cJSON_IsObject(root));
   assert(cJSON_GetObjectItemCaseSensitive(root, "skills") == NULL);
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "mcpServers")));
   iface = cJSON_GetObjectItemCaseSensitive(root, "interface");
   cJSON *mcp_prompt =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(iface, "defaultPrompt"), 0);
   assert(cJSON_IsString(mcp_prompt));
   assert(strstr(mcp_prompt->valuestring, "REQUIRED FIRST STEP") != NULL);
   assert(strstr(mcp_prompt->valuestring, "investigate") != NULL);
   cJSON_Delete(root);

   /* Distinct CLI-only and MCP-only capabilities legitimately aggregate to both
    * registrations; the projection decides which capabilities each contains. */
   plan.cli = 1;
   plan.mcp = 1;
   format_codex_plugin_json(manifest, sizeof(manifest), 1, plan, "local doctor",
                            "/opt/aimee/bin/aimee");
   root = cJSON_Parse(manifest);
   assert(cJSON_IsObject(root));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "skills")));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "mcpServers")));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(cJSON_IsString(hooks));
   assert(strcmp(hooks->valuestring, "../hooks/codex-hooks.json") == 0);
   iface = cJSON_GetObjectItemCaseSensitive(root, "interface");
   cJSON *prompt = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(iface, "defaultPrompt"), 0);
   assert(cJSON_IsString(prompt));
   assert(strstr(prompt->valuestring, "local doctor") != NULL);
   assert(strstr(prompt->valuestring, "index") != NULL);
   assert(strstr(prompt->valuestring, "investigate") != NULL);
   cJSON_Delete(root);

   char skill[4096];
   format_codex_cli_skill(skill, sizeof(skill), "local doctor,workspace repair",
                          "/opt/aimee/bin/aimee");
   assert(strstr(skill, "`/opt/aimee/bin/aimee local doctor`") != NULL);
   assert(strstr(skill, "`/opt/aimee/bin/aimee workspace repair`") != NULL);
   assert(strstr(skill, "aimee index investigate") == NULL);
}

static void test_projected_mcp_only_module_registers_filtered_backup(void)
{
   cJSON *projection =
       cJSON_Parse("{\"agent_surfaces\":{\"cli_only\":[],\"mcp_only\":[\"kb_future\"]}}");
   assert(cJSON_IsObject(projection));
   client_tool_surface_requirements_t req = client_tool_surface_requirements_from_json(projection);
   cJSON_Delete(projection);
   assert(req.cli_only[0] == '\0');
   assert(strcmp(req.mcp_only, "kb_future") == 0);

   client_tool_registration_plan_t plan = client_tool_registration_plan(
       CLIENT_TOOL_TRANSPORT_CLI_FIRST, 1, 1, req.cli_only[0], req.mcp_only[0]);
   assert(plan.cli == 1 && plan.mcp == 1);

   char manifest[4096];
   format_codex_plugin_json(manifest, sizeof(manifest), 0, plan, NULL, "/opt/aimee/bin/aimee");
   cJSON *root = cJSON_Parse(manifest);
   assert(cJSON_IsObject(root));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "skills")));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "mcpServers")));
   cJSON_Delete(root);

   char mcp[4096];
   format_mcp_json(mcp, sizeof(mcp), "/aimee-test/bin/aimee", req.mcp_only);
   root = cJSON_Parse(mcp);
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   cJSON *env = cJSON_GetObjectItemCaseSensitive(aimee, "env");
   cJSON *allowlist = cJSON_GetObjectItemCaseSensitive(env, "AIMEE_MCP_TOOL_ALLOWLIST");
   assert(cJSON_IsString(allowlist));
   assert(strcmp(allowlist->valuestring, "kb_future") == 0);
   cJSON_Delete(root);
   client_tool_surface_requirements_dispose(&req);
}

static void test_projected_module_names_are_not_truncated(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *surfaces = cJSON_AddObjectToObject(root, "agent_surfaces");
   cJSON_AddArrayToObject(surfaces, "cli_only");
   cJSON *mcp_only = cJSON_AddArrayToObject(surfaces, "mcp_only");
   char name[64];
   for (int i = 0; i < 400; i++)
   {
      snprintf(name, sizeof(name), "future_module_%03d", i);
      cJSON_AddItemToArray(mcp_only, cJSON_CreateString(name));
   }

   client_tool_surface_requirements_t req = client_tool_surface_requirements_from_json(root);
   cJSON_Delete(root);
   assert(req.complete == 1);
   assert(req.mcp_only != NULL && strlen(req.mcp_only) > 2048);
   assert(strstr(req.mcp_only, "future_module_399") != NULL);

   size_t cap = strlen(req.mcp_only) + 4096;
   char *mcp = malloc(cap);
   assert(mcp != NULL);
   assert(format_mcp_json(mcp, cap, "/aimee-test/bin/aimee", req.mcp_only) == 0);
   root = cJSON_Parse(mcp);
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   cJSON *env = cJSON_GetObjectItemCaseSensitive(aimee, "env");
   cJSON *allowlist = cJSON_GetObjectItemCaseSensitive(env, "AIMEE_MCP_TOOL_ALLOWLIST");
   assert(cJSON_IsString(allowlist));
   assert(strstr(allowlist->valuestring, "future_module_399") != NULL);
   cJSON_Delete(root);
   free(mcp);
   client_tool_surface_requirements_dispose(&req);
}

static void test_codex_plugin_omits_session_start_hook(void)
{
   {
      const char *hooks = codex_hooks_json("/usr/local/bin/aimee", "cli");
      assert(strstr(hooks, "\"SessionStart\"") == NULL);
      assert(strstr(hooks, "session-start") == NULL);
      assert(strstr(hooks, "\"PreToolUse\"") != NULL);
      assert(strstr(hooks, "AIMEE_CLI_PATH=/usr/local/bin/aimee") != NULL);
      /* MUST be `hooks pre`, not `hooks`. Bare `hooks` exits with "hooks requires
       * 'pre' or 'post'" and codex allows the tool -- a hook that is installed,
       * declared, well-formed, and enforces nothing. The first version of this
       * assertion pinned exactly that defect by matching the prefix. */
      assert(strstr(hooks, "/usr/local/bin/aimee hooks pre") != NULL);
      cJSON *parsed = cJSON_Parse(hooks);
      assert(parsed != NULL); /* codex refuses a malformed hooks file outright */
      cJSON_Delete(parsed);
      /* A hooks file codex never loads is the same as no hook, so the manifest
       * must point at it. ensure_codex_plugin writes both; assert the path the
       * manifest declares matches the file the writer emits. */
      assert(strstr(hooks, "\"command\"") != NULL);
   }
}

static void test_mcp_config_uses_resolved_command(void)
{
   char buf[1024];
   assert(format_mcp_json(buf, sizeof(buf), "/aimee-test/bin/aimee", NULL) == 0);
   cJSON *config = cJSON_Parse(buf);
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(config, "mcpServers");
   cJSON *configured = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   cJSON *configured_command = cJSON_GetObjectItemCaseSensitive(configured, "command");
   assert(cJSON_IsString(configured_command));
   assert(strcmp(configured_command->valuestring, "/aimee-test/bin/aimee") == 0);
   cJSON *configured_args = cJSON_GetObjectItemCaseSensitive(configured, "args");
   assert(cJSON_IsArray(configured_args));
   assert(strcmp(cJSON_GetArrayItem(configured_args, 0)->valuestring, "mcp-serve") == 0);
   cJSON_Delete(config);
   assert(strstr(buf, "AIMEE_IR_SESSION_OWNER") == NULL);

   cJSON *server = create_aimee_mcp_server("/aimee-test/bin/aimee");
   assert(cJSON_IsObject(server));
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(server, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, "/aimee-test/bin/aimee") == 0);
   cJSON *args = cJSON_GetObjectItemCaseSensitive(server, "args");
   assert(cJSON_IsArray(args));
   cJSON *arg0 = cJSON_GetArrayItem(args, 0);
   assert(cJSON_IsString(arg0));
   assert(strcmp(arg0->valuestring, "mcp-serve") == 0);
   cJSON_Delete(server);
}

/* The agent host spawns `aimee mcp-serve` itself, with an environment of its
 * own choosing. When AIMEE_HOME is where the config actually lives -- any
 * containerised or managed-server install -- and the generated config does not
 * carry it, the server starts, cannot reach aimee-server, and answers
 * tools/list with an EMPTY LIST. The agent is then silently offered no tools at
 * all and falls back to grep, which is indistinguishable from deciding the
 * index was not worth calling. Measured: 18 tools with AIMEE_HOME, 0 without,
 * regardless of HOME. */
/* THE SKILL IS THE SAME TEXT FOR EVERY RUN.
 *
 * There used to be a "solo" variant that withheld the delegate tools and swapped
 * the delegation bullets for "do all of this work yourself". A profile that hides
 * shipped tools during measurement makes the measured thing a configuration
 * nobody deploys -- the benchmark stops describing aimee. If work should not be
 * delegated, that is a rule of the run, not a different build.
 *
 * Pin that the profile no longer changes what the agent is told. */
static void test_hooks_do_not_vary_by_tool_profile(void)
{
   setenv("AIMEE_MCP_TOOL_PROFILE", "solo", 1);
   char solo_hooks[1536];
   snprintf(solo_hooks, sizeof solo_hooks, "%s", codex_hooks_json("aimee", "cli"));

   setenv("AIMEE_MCP_TOOL_PROFILE", "core", 1);
   assert(strcmp(solo_hooks, codex_hooks_json("aimee", "cli")) == 0);

   unsetenv("AIMEE_MCP_TOOL_PROFILE");
}

static void test_client_markdown_is_retired(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-no-markdown-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char skill[640], command[640], customized[640], unrelated[640];
   snprintf(skill, sizeof skill, "%s/plugins/aimee/skills/aimee/SKILL.md", tmpdir);
   snprintf(command, sizeof command, "%s/.claude/commands/aimee-search.md", tmpdir);
   snprintf(customized, sizeof customized, "%s/.claude/commands/aimee-delegate.md", tmpdir);
   snprintf(unrelated, sizeof unrelated, "%s/.claude/commands/user-command.md", tmpdir);
   write_text_file(skill, "user-customized legacy skill", 0644);
   write_text_file(command,
                   "Search aimee memory for project facts, prior decisions, and stored context.\n"
                   "\n"
                   "Use the aimee MCP tool `search_memory` with the query: $ARGUMENTS\n"
                   "\n"
                   "If no query is provided, use `list_facts` to show all stored facts.\n",
                   0644);
   write_text_file(customized, "user-customized legacy command", 0644);
   write_text_file(unrelated, "keep", 0644);
   assert(access(skill, F_OK) == 0 && access(command, F_OK) == 0);

   retire_client_markdown(tmpdir);
   assert(access(skill, F_OK) == 0);
   assert(access(command, F_OK) != 0);
   assert(access(customized, F_OK) == 0);
   assert(access(unrelated, F_OK) == 0);

   char rm_cmd[700];
   snprintf(rm_cmd, sizeof rm_cmd, "rm -rf '%s'", tmpdir);
   (void)system(rm_cmd);
}

static void test_mcp_config_carries_aimee_home(void)
{
   char buf[1024];
   cJSON *server = NULL;
   cJSON *env = NULL;
   cJSON *home = NULL;

   setenv("AIMEE_HOME", "/var/lib/aimee-home", 1);

   assert(format_mcp_json(buf, sizeof(buf), "/aimee-test/bin/aimee", NULL) == 0);
   assert(strstr(buf, "\"env\"") != NULL);
   assert(strstr(buf, "AIMEE_IR_SESSION_OWNER") == NULL);
   cJSON *config = cJSON_Parse(buf);
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(config, "mcpServers");
   cJSON *configured = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   cJSON *configured_env = cJSON_GetObjectItemCaseSensitive(configured, "env");
   cJSON *configured_home = cJSON_GetObjectItemCaseSensitive(configured_env, "AIMEE_HOME");
   assert(cJSON_IsString(configured_home));
   assert(strcmp(configured_home->valuestring, "/var/lib/aimee-home") == 0);
   cJSON_Delete(config);

   server = create_aimee_mcp_server("/aimee-test/bin/aimee");
   assert(cJSON_IsObject(server));
   env = cJSON_GetObjectItemCaseSensitive(server, "env");
   assert(cJSON_IsObject(env));
   home = cJSON_GetObjectItemCaseSensitive(env, "AIMEE_HOME");
   assert(cJSON_IsString(home));
   assert(strcmp(home->valuestring, "/var/lib/aimee-home") == 0);
   cJSON_Delete(server);

   /* Unset means the default resolution already works; pinning a value the
    * operator never chose would be worse than saying nothing. */
   unsetenv("AIMEE_HOME");
   assert(format_mcp_json(buf, sizeof(buf), "/aimee-test/bin/aimee", NULL) == 0);
   assert(strstr(buf, "AIMEE_HOME") == NULL);
   assert(strstr(buf, "AIMEE_IR_SESSION_OWNER") == NULL);

   server = create_aimee_mcp_server("/aimee-test/bin/aimee");
   assert(cJSON_GetObjectItemCaseSensitive(server, "env") == NULL);
   cJSON_Delete(server);
}

/* 1 if hooks[event] has an entry whose command contains `needle`. */
static int hook_event_has_cmd(cJSON *hooks, const char *event, const char *needle)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
      return 0;
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *ha = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
      for (int j = 0; cJSON_IsArray(ha) && j < cJSON_GetArraySize(ha); j++)
      {
         cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ha, j), "command");
         if (cJSON_IsString(c) && strstr(c->valuestring, needle))
            return 1;
      }
   }
   return 0;
}

/* Assert the remaining tool and recall hooks. Persona delivery is deliberately
 * absent: shared model ingress prepends the editable persona for every client. */
static void assert_required_hooks_present(cJSON *hooks)
{
   static const struct
   {
      const char *event;
      const char *subcommand;
   } required[] = {
       {"UserPromptSubmit", "user-prompt-submit"}, /* per-turn recall envelope */
       {"PreCompact", "pre-compact"},              /* post-compact recall re-prime */
       {"PreToolUse", "attention-guard"},          /* per-file attention + destructive-op guard */
       {"PostToolUse", "hooks post"},              /* post-edit hook */
   };
   for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
      assert(hook_event_has_cmd(hooks, required[i].event, required[i].subcommand));
}

/* --- Test read_json_file --- */

static void test_read_json_file_missing(void)
{
   cJSON *root = read_json_file("/nonexistent/path/file.json");
   assert(root == NULL);
}

static void test_read_json_file_valid(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-json-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *json = "{\"key\": \"value\", \"num\": 42}";
   write(fd, json, strlen(json));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *key = cJSON_GetObjectItem(root, "key");
   assert(cJSON_IsString(key));
   assert(strcmp(key->valuestring, "value") == 0);

   cJSON *num = cJSON_GetObjectItem(root, "num");
   assert(cJSON_IsNumber(num));
   assert(num->valueint == 42);

   cJSON_Delete(root);
   unlink(tmppath);
}

static void test_read_json_file_invalid(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-badjson-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *bad = "not valid json at all {{{";
   write(fd, bad, strlen(bad));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root == NULL);

   unlink(tmppath);
}

static void test_resolved_aimee_bin_path_fallback(void)
{
   const char *path = resolved_aimee_bin_path();
   assert(path != NULL);
   const char *home = getenv("HOME");
   if (home)
   {
      char expected[512];
      snprintf(expected, sizeof(expected), "%s/.local/bin/aimee", home);
      assert(strcmp(path, expected) == 0);
   }
}

/* --- Test ensure_claude_code_mcp: non-destructive merge behavior --- */

static void test_claude_mcp_creates_fresh_user_config(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* Create a fake aimee binary so stat() succeeds */
   char fake_bin[512];
   snprintf(fake_bin, sizeof(fake_bin), "%s/fake-aimee", tmpdir);
   FILE *fp = fopen(fake_bin, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n", fp);
   fclose(fp);
   chmod(fake_bin, 0755);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/.claude.json", tmpdir);

   /* Write a settings file with existing data */
   fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("{\"existingKey\": true, \"mcpServers\": {\"other\": {\"command\": \"other-mcp\"}}}", fp);
   fclose(fp);

   ensure_claude_code_mcp_entry(config_path, fake_bin, NULL);

   cJSON *root = read_json_file(config_path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Verify existing key is preserved */
   cJSON *existing = cJSON_GetObjectItem(root, "existingKey");
   assert(existing != NULL && cJSON_IsTrue(existing));

   /* Verify other server still present */
   cJSON *servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(servers));
   cJSON *other = cJSON_GetObjectItem(servers, "other");
   assert(cJSON_IsObject(other));
   cJSON *other_cmd = cJSON_GetObjectItem(other, "command");
   assert(cJSON_IsString(other_cmd));
   assert(strcmp(other_cmd->valuestring, "other-mcp") == 0);

   cJSON *aimee = cJSON_GetObjectItem(servers, "aimee");
   assert(cJSON_IsObject(aimee));
   cJSON *cmd = cJSON_GetObjectItem(aimee, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, fake_bin) == 0);
   cJSON *type = cJSON_GetObjectItem(aimee, "type");
   assert(cJSON_IsString(type));
   assert(strcmp(type->valuestring, "stdio") == 0);
   cJSON *args = cJSON_GetObjectItem(aimee, "args");
   assert(cJSON_IsArray(args));
   assert(strcmp(cJSON_GetArrayItem(args, 0)->valuestring, "mcp-serve") == 0);

   cJSON_Delete(root);

   /* A projected MCP-only module keeps the fallback registration but narrows
    * it to that module instead of duplicating the dual-surface CLI tools. */
   ensure_claude_code_mcp_entry(config_path, fake_bin, "future_module_399");
   root = read_json_file(config_path);
   servers = cJSON_GetObjectItem(root, "mcpServers");
   aimee = cJSON_GetObjectItem(servers, "aimee");
   cJSON *env = cJSON_GetObjectItem(aimee, "env");
   cJSON *allowlist = cJSON_GetObjectItem(env, "AIMEE_MCP_TOOL_ALLOWLIST");
   assert(cJSON_IsString(allowlist));
   assert(strcmp(allowlist->valuestring, "future_module_399") == 0);
   cJSON_Delete(root);

   /* CLI-first with no MCP-only projection retires only Aimee's generated MCP
    * entry and preserves every unrelated Claude setting/server. */
   remove_claude_code_mcp(config_path);
   root = read_json_file(config_path);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(root, "existingKey")));
   servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(cJSON_GetObjectItem(servers, "other")));
   assert(cJSON_GetObjectItem(servers, "aimee") == NULL);
   cJSON_Delete(root);

   /* Cleanup */
   char rm_cmd[512];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   system(rm_cmd);
}

static void test_claude_hooks_create_post_hook_on_fresh_settings(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{}", fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(cJSON_IsObject(hooks));
   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   assert(cJSON_IsArray(post));

   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(post); i++)
   {
      cJSON *entry = cJSON_GetArrayItem(post, i);
      cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
      if (!cJSON_IsString(matcher) || !cJSON_IsArray(hook_arr))
         continue;
      if (!strstr(matcher->valuestring, "EnterWorktree") ||
          !strstr(matcher->valuestring, "ExitWorktree"))
         continue;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *hook = cJSON_GetArrayItem(hook_arr, j);
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(hook, "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, "hooks post"))
         {
            found = 1;
            break;
         }
      }
      if (found)
         break;
   }
   assert(found);

   /* Regression: from an empty settings.json, EVERY required hook is registered. */
   assert_required_hooks_present(hooks);
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* 1 if root.permissions.deny[] contains the exact string `tool`. */
static int perms_deny_has(cJSON *root, const char *tool)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   cJSON *deny = perms ? cJSON_GetObjectItemCaseSensitive(perms, "deny") : NULL;
   for (int i = 0; cJSON_IsArray(deny) && i < cJSON_GetArraySize(deny); i++)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
         return 1;
   }
   return 0;
}

static int stub_delegates_available(void)
{
   return 1;
}
static int stub_delegates_none(void)
{
   return 0;
}
static int stub_delegates_unknown(void)
{
   return -1;
}

/* The sub-agent-ban gate: ensure_claude_code_hooks installs the subagent-guard
 * PreToolUse hook + permissions.deny [Task, Agent] ONLY when the injected delegate
 * probe reports usable delegates, and removes both when it does not. An "unknown"
 * probe (server down) must leave settings untouched. Config subagent_ban_enabled
 * defaults ON (no aimee.yaml opt-out in the test env). */
static void test_claude_hooks_subagent_ban_gate(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-subagent-ban-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);

   /* Delegates available -> install the guard hook AND the static deny backstop. */
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{}", fp);
   fclose(fp);
   client_integrations_set_delegate_probe(stub_delegates_available);
   ensure_claude_code_hooks(settings_path);
   cJSON *root = read_json_file(settings_path);
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(perms_deny_has(root, "Task"));
   assert(perms_deny_has(root, "Agent"));
   /* The attention-guard matcher no longer claims Task|Agent (the dedicated guard
    * owns them now). */
   assert(hook_event_has_cmd(hooks, "PreToolUse", "attention-guard"));
   cJSON_Delete(root);

   /* The path written into the user's GLOBAL config must be the INSTALLED
    * client, never the binary that happened to run the wiring.
    *
    * Running a client built in a throwaway worktree used to rewrite
    * ~/.claude/settings.json to that worktree path. When the worktree was
    * deleted, every hook in every session failed with
    *   /bin/sh: 1: /home/.../aimee-<sha>/aimee: not found
    * in projects that had nothing to do with the build. Durable config must
    * never capture a transient path.
    *
    * Asserted against the decision function directly: an end-to-end check
    * cannot reach this, because the test binary is not named `aimee` and so
    * takes the same fallback either way — it passes with the bug present. */
   {
      char out[512];
      const char *installed = "/home/dev/.local/bin/aimee";
      const char *worktree_exe = "/home/dev/dev/aimee-225c45f/aimee";

      /* The regression: an install exists, so the transient path must lose. */
      assert(client_integrations_pick_bin_path(installed, worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, installed) == 0);

      /* No install yet (first-run bootstrap) -> the running binary is all we
       * have, and is still better than nothing. */
      assert(client_integrations_pick_bin_path(NULL, worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, worktree_exe) == 0);
      assert(client_integrations_pick_bin_path("", worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, worktree_exe) == 0);

      /* Neither available -> fail rather than emit a half-formed command. */
      assert(client_integrations_pick_bin_path(NULL, NULL, out, sizeof(out)) != 0);
      assert(client_integrations_pick_bin_path("", "", out, sizeof(out)) != 0);

      /* A path that would be truncated must fail, not be silently cut into a
       * different (wrong, possibly existing) path. */
      char tiny[8];
      assert(client_integrations_pick_bin_path(installed, NULL, tiny, sizeof(tiny)) != 0);
      assert(client_integrations_pick_bin_path(installed, NULL, out, 0) != 0);
      assert(client_integrations_pick_bin_path(installed, NULL, NULL, sizeof(out)) != 0);

      printf("  PASS: config records the installed client, not a transient build path\n");
   }

   /* Delegates gone -> the SAME settings must have the guard + deny removed. */
   client_integrations_set_delegate_probe(stub_delegates_none);
   ensure_claude_code_hooks(settings_path);
   root = read_json_file(settings_path);
   hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(!hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(!perms_deny_has(root, "Task"));
   assert(!perms_deny_has(root, "Agent"));
   cJSON_Delete(root);

   /* Re-install, then an UNKNOWN probe (server unreachable) must leave it as-is. */
   client_integrations_set_delegate_probe(stub_delegates_available);
   ensure_claude_code_hooks(settings_path);
   client_integrations_set_delegate_probe(stub_delegates_unknown);
   ensure_claude_code_hooks(settings_path);
   root = read_json_file(settings_path);
   hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(perms_deny_has(root, "Task"));
   cJSON_Delete(root);

   client_integrations_set_delegate_probe(NULL); /* don't leak into other tests */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_hooks_patch_existing_matcher(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\","
         "\"command\":\"aimee session-start\"}]}],"
         "\"PostToolUse\":[{\"matcher\":\"Edit|Write|MultiEdit\","
         "\"hooks\":[{\"type\":\"command\",\"command\":\"AIMEE_HOOK_CLIENT=claude "
         "aimee hooks post\"}]}]}}",
         fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   assert(cJSON_IsArray(post));
   assert(cJSON_GetArraySize(post) == 1);

   cJSON *entry = cJSON_GetArrayItem(post, 0);
   cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
   assert(cJSON_IsString(matcher));
   assert(strstr(matcher->valuestring, "Edit|Write|MultiEdit") != NULL);
   assert(strstr(matcher->valuestring, "EnterWorktree") != NULL);
   assert(strstr(matcher->valuestring, "ExitWorktree") != NULL);

   /* Existing installations are migrated away from SessionStart persona
    * delivery while the remaining hooks are merged normally. */
   assert_required_hooks_present(hooks);
   assert(!hook_event_has_cmd(hooks, "SessionStart", "session-start"));
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* A stale aimee hook command (e.g. an old/transient binary path) is re-pointed
 * to the resolved binary, so a reinstall heals the hook rather than leaving it
 * dangling at a path that no longer exists. */
static void test_claude_hooks_repoint_stale_command(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   /* A stale PreToolUse attention-guard AND a stale PostToolUse hooks-post entry,
    * both referencing a transient /tmp build path. */
   fputs("{\"hooks\":{"
         "\"PreToolUse\":[{\"matcher\":\"Bash\",\"hooks\":[{\"type\":\"command\","
         "\"command\":\"AIMEE_HOOK_CLIENT=claude /tmp/old-build/aimee attention-guard\"}]}],"
         "\"PostToolUse\":[{\"matcher\":\"Edit|EnterWorktree|ExitWorktree\","
         "\"hooks\":[{\"type\":\"command\","
         "\"command\":\"AIMEE_HOOK_CLIENT=claude /tmp/old-build/aimee hooks post\"}]}]"
         "}}",
         fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");

   /* Both the PreToolUse attention-guard and the PostToolUse hooks-post commands
    * are re-pointed off the stale /tmp path to the resolved binary. */
   const char *events[] = {"PreToolUse", "PostToolUse"};
   const char *needles[] = {"attention-guard", "hooks post"};
   for (int e = 0; e < 2; e++)
   {
      cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, events[e]);
      assert(cJSON_IsArray(arr));
      const char *found = NULL;
      for (int i = 0; i < cJSON_GetArraySize(arr); i++)
      {
         cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
         for (int j = 0; cJSON_IsArray(hook_arr) && j < cJSON_GetArraySize(hook_arr); j++)
         {
            cJSON *cmd =
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hook_arr, j), "command");
            if (cJSON_IsString(cmd) && strstr(cmd->valuestring, needles[e]))
               found = cmd->valuestring;
         }
      }
      assert(found != NULL);
      assert(strstr(found, "/tmp/old-build/aimee") == NULL);
      assert(strstr(found, "AIMEE_HOOK_CLIENT=claude ") == found);
      assert(strstr(found, needles[e]) != NULL);
   }
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test Codex plugin config (TOML-like) non-destructive update --- */

static void test_codex_plugin_enabled_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Call with no existing file - should create with section + enabled */
   ensure_codex_plugin_enabled(config_path);

   FILE *fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_preserves_existing(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Write existing config with other settings */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[general]\ntheme = \"dark\"\n", fp);
   fclose(fp);

   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[2048];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Original content should be preserved */
   assert(strstr(buf, "[general]") != NULL);
   assert(strstr(buf, "theme = \"dark\"") != NULL);

   /* Plugin section should be appended */
   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Create file with the section already present */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[plugins.\"aimee@local\"]\nenabled = true\n", fp);
   fclose(fp);

   /* Call again - should not modify */
   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Should still have exactly one instance */
   char *first = strstr(buf, "[plugins.\"aimee@local\"]");
   assert(first != NULL);
   /* No second instance */
   char *second = strstr(first + 1, "[plugins.\"aimee@local\"]");
   assert(second == NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static int count_substr(const char *haystack, const char *needle)
{
   int count = 0;
   const char *p = haystack;
   while ((p = strstr(p, needle)) != NULL)
   {
      count++;
      p += strlen(needle);
   }
   return count;
}

static void read_text_or_die(const char *path, char *buf, size_t buf_len)
{
   FILE *fp = fopen(path, "r");
   assert(fp != NULL);
   size_t n = fread(buf, 1, buf_len - 1, fp);
   fclose(fp);
   buf[n] = '\0';
}

static void test_codex_trusted_project_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(strstr(buf, "[projects.\"/tmp/workspace/project\"]") != NULL);
   assert(strstr(buf, "trust_level = \"trusted\"") != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_trusted_project_updates_existing_section(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[projects.\"/tmp/workspace/project\"]\n"
         "trust_level = \"untrusted\"\n"
         "model = \"gpt-5.4\"\n"
         "\n"
         "[plugins.\"aimee@local\"]\n"
         "enabled = true\n",
         fp);
   fclose(fp);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(strstr(buf, "trust_level = \"untrusted\"") == NULL);
   assert(strstr(buf, "trust_level = \"trusted\"") != NULL);
   assert(strstr(buf, "model = \"gpt-5.4\"") != NULL);
   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_trusted_project_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);
   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(count_substr(buf, "[projects.\"/tmp/workspace/project\"]") == 1);
   assert(count_substr(buf, "trust_level = \"trusted\"") == 1);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test ensure_codex_marketplace: non-destructive merge --- */

static void test_codex_marketplace_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-market-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Call with no existing file */
   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 1);

   cJSON *entry = cJSON_GetArrayItem(plugins, 0);
   cJSON *ename = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(ename));
   assert(strcmp(ename->valuestring, "aimee") == 0);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_marketplace_preserves_other_plugins(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-market2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Write existing marketplace with another plugin */
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("{\"name\":\"local\",\"interface\":{\"displayName\":\"Local\"},"
         "\"plugins\":[{\"name\":\"other-plugin\",\"category\":\"Tools\"}]}",
         fp);
   fclose(fp);

   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   /* Should have both plugins */
   assert(cJSON_GetArraySize(plugins) == 2);

   /* Verify other plugin is preserved */
   int found_other = 0, found_aimee = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *n = cJSON_GetObjectItem(item, "name");
      if (cJSON_IsString(n))
      {
         if (strcmp(n->valuestring, "other-plugin") == 0)
            found_other = 1;
         if (strcmp(n->valuestring, "aimee") == 0)
            found_aimee = 1;
      }
   }
   assert(found_other);
   assert(found_aimee);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test ensure_claude_code_trust --- */

static void write_claude_json(const char *path, const char *content)
{
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs(content, fp);
   fclose(fp);
   chmod(path, 0600);
}

static void test_claude_trust_creates_entry_for_new_path(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust1-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   write_claude_json(claude_json, "{\"projects\":{}}");

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   assert(cJSON_IsObject(root));
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   assert(cJSON_IsArray(enabled));
   assert(cJSON_GetArraySize(enabled) == 1);
   cJSON *item = cJSON_GetArrayItem(enabled, 0);
   assert(cJSON_IsString(item) && strcmp(item->valuestring, "aimee") == 0);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_updates_existing_untrusted_entry(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   /* Simulate an existing entry with trust rejected */
   char initial[1024];
   snprintf(initial, sizeof(initial),
            "{\"projects\":{\"%s\":{\"hasTrustDialogAccepted\":false,"
            "\"enabledMcpjsonServers\":[],\"disabledMcpjsonServers\":[]}}}",
            worktree);
   write_claude_json(claude_json, initial);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   assert(cJSON_IsObject(root));
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   assert(cJSON_IsArray(enabled));
   /* Should have added "aimee" */
   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *it = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(it) && strcmp(it->valuestring, "aimee") == 0)
         found = 1;
   }
   assert(found);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   write_claude_json(claude_json, "{\"projects\":{}}");
   ensure_claude_code_trust(worktree);
   ensure_claude_code_trust(worktree); /* Call again — should be a no-op */

   cJSON *root = read_json_file(claude_json);
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   /* Should only have one "aimee" entry, not two */
   int aimee_count = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *it = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(it) && strcmp(it->valuestring, "aimee") == 0)
         aimee_count++;
   }
   assert(aimee_count == 1);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_preserves_other_projects(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust4-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   write_claude_json(claude_json,
                     "{\"projects\":{\"/other/project\":{\"hasTrustDialogAccepted\":true}}}");

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   /* Other project must still be there */
   cJSON *other = cJSON_GetObjectItemCaseSensitive(projects, "/other/project");
   assert(cJSON_IsObject(other));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(other, "hasTrustDialogAccepted")));
   /* New path should also be added */
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_no_op_when_claude_json_missing(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust5-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir); /* No .claude.json in this dir */

   /* Should not crash or create .claude.json */
   ensure_claude_code_trust("/some/path");

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   FILE *fp = fopen(claude_json, "r");
   assert(fp == NULL); /* Must not have been created */

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test write_text_file: no-op when content unchanged --- */

static void test_write_text_file_no_op(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-write-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *content = "test content here";
   write(fd, content, strlen(content));
   close(fd);

   /* Get mtime before */
   struct stat st1;
   assert(stat(tmppath, &st1) == 0);

   /* Small sleep to ensure mtime would differ */
   usleep(10000);

   /* Write same content: should be no-op */
   int rc = write_text_file(tmppath, content, 0600);
   assert(rc == 0);

   /* mtime should not change since content is identical */
   struct stat st2;
   assert(stat(tmppath, &st2) == 0);
   assert(st1.st_mtime == st2.st_mtime);

   unlink(tmppath);
}

static void test_unselected_generated_surface_is_removed(void)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee-test-generated-surface-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);

   assert(sync_generated_surface_file(path, "generated\n", 1) == 0);
   assert(access(path, F_OK) == 0);
   assert(sync_generated_surface_file(path, NULL, 0) == 0);
   assert(access(path, F_OK) != 0);
   assert(errno == ENOENT);
   /* Retiring an already-absent generated surface is idempotent. */
   assert(sync_generated_surface_file(path, NULL, 0) == 0);
}

static void test_generated_surface_availability_drives_fallback(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-surface-probe-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char writable[640];
   snprintf(writable, sizeof(writable), "%s/skills/aimee/SKILL.md", tmpdir);
   assert(generated_surface_path_writable(writable) == 1);

   char blocker[640];
   snprintf(blocker, sizeof(blocker), "%s/not-a-directory", tmpdir);
   assert(write_text_file(blocker, "block", 0644) == 0);
   char unavailable[700];
   snprintf(unavailable, sizeof(unavailable), "%s/child/SKILL.md", blocker);
   assert(generated_surface_path_writable(unavailable) == 0);

   client_tool_registration_plan_t plan = client_tool_registration_plan(
       CLIENT_TOOL_TRANSPORT_CLI_FIRST, generated_surface_path_writable(unavailable),
       generated_surface_path_writable(writable), 0, 0);
   assert(plan.cli == 0 && plan.mcp == 1);

   char rm_cmd[700];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   (void)system(rm_cmd);
}

/* --- Test the client-integrations opt-out gate --- */

static void test_tool_transport_preference_config(void)
{
   g_test_transport_preference = "mcp-first";
   assert(client_tool_transport_preference() == CLIENT_TOOL_TRANSPORT_MCP_FIRST);
   g_test_transport_preference = "cli-first";
   assert(client_tool_transport_preference() == CLIENT_TOOL_TRANSPORT_CLI_FIRST);
}

static void test_client_integrations_optout_gate(void)
{
   /* Hermetic config: point AIMEE_HOME at an empty temp dir so the gate reads
    * an aimee.yaml under our control (absent -> default client_integrations
    * ON). */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ci-optout-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* platform_unsetenv is not declared in this include context, and the gate
    * treats "0"/"false"/empty as "not opted out", so "0" stands in for unset. */
   char old_home[512] = {0};
   const char *prev_home = getenv("AIMEE_HOME");
   if (prev_home)
      snprintf(old_home, sizeof(old_home), "%s", prev_home);

   platform_setenv("AIMEE_HOME", tmpdir);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");

   /* Default config, no env override -> integrations allowed. */
   assert(client_integrations_allowed() == 1);

   /* Env override opts out regardless of config. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "1");
   assert(client_integrations_allowed() == 0);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "yes");
   assert(client_integrations_allowed() == 0);

   /* "0" and "false" are treated as unset, so the (default-ON) config wins. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   assert(client_integrations_allowed() == 1);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "false");
   assert(client_integrations_allowed() == 1);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");

   /* Config-driven opt-out from the remote config contract closes the gate. */
   g_test_integrations_enabled = 0;
   assert(client_integrations_allowed() == 0);

   /* And the env override still wins the other way: "0" cannot re-enable it once
    * the config disables it (env only forces OFF, never ON). */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   assert(client_integrations_allowed() == 0);

   /* Restore AIMEE_HOME (best-effort) and neutralize the opt-out env so later
    * code in this process sees a clean state. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   g_test_integrations_enabled = 1;
   if (old_home[0])
      platform_setenv("AIMEE_HOME", old_home);

   char rm_cmd[600];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   system(rm_cmd);
}

int main(void)
{
   client_config_set_provider(test_client_config_value);
   printf("client_integrations: ");

   test_build_marketplace_root();
   test_build_aimee_plugin_entry();
   test_tool_transport_registration_plan();
   test_codex_cli_registration_is_command_first();
   test_codex_manifest_registers_selected_transports_only();
   test_projected_mcp_only_module_registers_filtered_backup();
   test_projected_module_names_are_not_truncated();
   test_codex_plugin_omits_session_start_hook();
   test_mcp_config_uses_resolved_command();
   test_hooks_do_not_vary_by_tool_profile();
   test_client_markdown_is_retired();
   test_mcp_config_carries_aimee_home();
   test_read_json_file_missing();
   test_read_json_file_valid();
   test_read_json_file_invalid();
   test_resolved_aimee_bin_path_fallback();
   test_claude_mcp_creates_fresh_user_config();
   test_claude_hooks_create_post_hook_on_fresh_settings();
   test_claude_hooks_patch_existing_matcher();
   test_claude_hooks_repoint_stale_command();
   test_claude_hooks_subagent_ban_gate();
   test_codex_plugin_enabled_fresh();
   test_codex_plugin_enabled_preserves_existing();
   test_codex_plugin_enabled_idempotent();
   test_codex_trusted_project_fresh();
   test_codex_trusted_project_updates_existing_section();
   test_codex_trusted_project_idempotent();
   test_codex_marketplace_fresh();
   test_codex_marketplace_preserves_other_plugins();
   test_write_text_file_no_op();
   test_unselected_generated_surface_is_removed();
   test_generated_surface_availability_drives_fallback();
   test_claude_trust_creates_entry_for_new_path();
   test_claude_trust_updates_existing_untrusted_entry();
   test_claude_trust_idempotent();
   test_claude_trust_preserves_other_projects();
   test_claude_trust_no_op_when_claude_json_missing();
   test_tool_transport_preference_config();
   test_client_integrations_optout_gate();

   printf("all tests passed\n");
   return 0;
}
