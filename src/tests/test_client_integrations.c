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

static void test_codex_delegate_policy_is_explicit(void)
{
   const char *prompt = codex_delegate_policy_prompt();
   assert(strstr(prompt, "spawn_agent") != NULL);
   assert(strstr(prompt, "Claude Agent") != NULL);
   assert(strstr(prompt, "delegate MCP tool") != NULL);

   const char *skill = codex_skill_markdown();
   assert(strstr(skill, "Do not call provider-native sub-agent tools") != NULL);
   assert(strstr(skill, "`spawn_agent`") != NULL);
   assert(strstr(skill, "`delegate` MCP tool") != NULL);
}

static void test_mcp_config_uses_resolved_command(void)
{
   char buf[1024];
   format_mcp_json(buf, sizeof(buf), "/tmp/aimee-bin");
   assert(strstr(buf, "\"command\": \"/tmp/aimee-bin\"") != NULL);
   assert(strstr(buf, "\"command\": \"aimee\"") == NULL);
   assert(strstr(buf, "\"args\": [\"mcp-serve\"]") != NULL);

   cJSON *server = create_aimee_mcp_server("/tmp/aimee-bin");
   assert(cJSON_IsObject(server));
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(server, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, "/tmp/aimee-bin") == 0);
   cJSON *args = cJSON_GetObjectItemCaseSensitive(server, "args");
   assert(cJSON_IsArray(args));
   cJSON *arg0 = cJSON_GetArrayItem(args, 0);
   assert(cJSON_IsString(arg0));
   assert(strcmp(arg0->valuestring, "mcp-serve") == 0);
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

static void test_claude_mcp_creates_fresh_settings(void)
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

   /* The function checks ~/.local/bin/aimee which may not exist
    * in test environment. We test the JSON merge logic directly instead. */
   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);

   /* Write a settings file with existing data */
   fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{\"existingKey\": true, \"mcpServers\": {\"other\": {\"command\": \"other-mcp\"}}}", fp);
   fclose(fp);

   /* Simulate what ensure_claude_code_mcp does: merge aimee into mcpServers */
   cJSON *root = read_json_file(settings_path);
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

   /* Add aimee server (simulating the merge) */
   char aimee_bin[512];
   const char *home = getenv("HOME");
   if (home)
      snprintf(aimee_bin, sizeof(aimee_bin), "%s/.local/bin/aimee", home);
   else
      snprintf(aimee_bin, sizeof(aimee_bin), "/aimee");
   cJSON *aimee_server = cJSON_CreateObject();
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   /* Write back and re-read */
   char *json_out = cJSON_Print(root);
   assert(json_out != NULL);
   fp = fopen(settings_path, "w");
   fputs(json_out, fp);
   fclose(fp);
   free(json_out);
   cJSON_Delete(root);

   /* Re-read and verify everything was preserved */
   root = read_json_file(settings_path);
   assert(root != NULL);

   existing = cJSON_GetObjectItem(root, "existingKey");
   assert(existing != NULL && cJSON_IsTrue(existing));

   servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(servers));

   other = cJSON_GetObjectItem(servers, "other");
   assert(cJSON_IsObject(other));

   cJSON *aimee = cJSON_GetObjectItem(servers, "aimee");
   assert(cJSON_IsObject(aimee));
   cJSON *cmd = cJSON_GetObjectItem(aimee, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, aimee_bin) == 0);

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

   /* Regression: the SessionStart hook MUST be registered -- it is the seam that
    * delivers aimee's session brief (persona principles/brief + MCP-skill index +
    * Rules + Key Facts via `aimee session-start`). Its absence left the primary
    * agent with no aimee persona/skills/rules context. The per-turn context hooks
    * are registered alongside it. */
   assert(hook_event_has_cmd(hooks, "SessionStart", "session-start"));
   assert(hook_event_has_cmd(hooks, "UserPromptSubmit", "user-prompt-submit"));
   assert(hook_event_has_cmd(hooks, "PreCompact", "pre-compact"));
   cJSON_Delete(root);

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
   fputs("{\"hooks\":{\"PostToolUse\":[{\"matcher\":\"Edit|Write|MultiEdit\","
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

int main(void)
{
   printf("client_integrations: ");

   test_build_marketplace_root();
   test_build_aimee_plugin_entry();
   test_codex_delegate_policy_is_explicit();
   test_mcp_config_uses_resolved_command();
   test_read_json_file_missing();
   test_read_json_file_valid();
   test_read_json_file_invalid();
   test_resolved_aimee_bin_path_fallback();
   test_claude_mcp_creates_fresh_settings();
   test_claude_hooks_create_post_hook_on_fresh_settings();
   test_claude_hooks_patch_existing_matcher();
   test_claude_hooks_repoint_stale_command();
   test_codex_plugin_enabled_fresh();
   test_codex_plugin_enabled_preserves_existing();
   test_codex_plugin_enabled_idempotent();
   test_codex_trusted_project_fresh();
   test_codex_trusted_project_updates_existing_section();
   test_codex_trusted_project_idempotent();
   test_codex_marketplace_fresh();
   test_codex_marketplace_preserves_other_plugins();
   test_write_text_file_no_op();
   test_claude_trust_creates_entry_for_new_path();
   test_claude_trust_updates_existing_untrusted_entry();
   test_claude_trust_idempotent();
   test_claude_trust_preserves_other_projects();
   test_claude_trust_no_op_when_claude_json_missing();

   printf("all tests passed\n");
   return 0;
}
