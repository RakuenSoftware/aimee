#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "headers/cmd_hooks_scope.h"
#include "cJSON.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void test_hook_payload_session_id_prefers_payload(void)
{
   setenv("AIMEE_SESSION_ID", "env-aimee", 1);
   setenv("CLAUDE_SESSION_ID", "env-claude", 1);
   setenv("CODEX_THREAD_ID", "env-codex", 1);

   cJSON *json = cJSON_Parse("{\"session_id\":\"payload-sid_123\"}");
   assert(json != NULL);
   char sid[64];
   assert(hook_payload_session_id(json, sid, sizeof(sid)) == 1);
   assert(strcmp(sid, "payload-sid_123") == 0);
   cJSON_Delete(json);
}

static void test_hook_payload_session_id_env_fallback_order(void)
{
   unsetenv("AIMEE_SESSION_ID");
   setenv("CLAUDE_SESSION_ID", "env-claude", 1);
   setenv("CODEX_THREAD_ID", "env-codex", 1);

   char sid[64];
   assert(hook_payload_session_id(NULL, sid, sizeof(sid)) == 1);
   assert(strcmp(sid, "env-claude") == 0);

   unsetenv("CLAUDE_SESSION_ID");
   assert(hook_payload_session_id(NULL, sid, sizeof(sid)) == 1);
   assert(strcmp(sid, "env-codex") == 0);
}

static void test_hook_payload_session_id_rejects_unsafe_ids(void)
{
   unsetenv("AIMEE_SESSION_ID");
   unsetenv("CLAUDE_SESSION_ID");
   unsetenv("CODEX_THREAD_ID");

   cJSON *json = cJSON_Parse("{\"session_id\":\"../../bad\"}");
   assert(json != NULL);
   char sid[64] = "unchanged";
   assert(hook_payload_session_id(json, sid, sizeof(sid)) == 0);
   assert(sid[0] == '\0');
   cJSON_Delete(json);
}

static void test_hook_payload_cwd_prefers_top_level(void)
{
   const char *payload = "{\"cwd\":\"/tmp/top\",\"tool_input\":{\"workdir\":\"/tmp/nested\"}}";
   cJSON *json = cJSON_Parse(payload);
   assert(json != NULL);
   char cwd[128];
   assert(hook_payload_cwd(json, cwd, sizeof(cwd)) == 1);
   assert(strcmp(cwd, "/tmp/top") == 0);
   cJSON_Delete(json);
}

static void test_hook_payload_cwd_reads_nested_object(void)
{
   cJSON *json = cJSON_Parse("{\"tool_input\":{\"cmd\":\"date\",\"workdir\":\"/tmp/nested\"}}");
   assert(json != NULL);
   char cwd[128];
   assert(hook_payload_cwd(json, cwd, sizeof(cwd)) == 1);
   assert(strcmp(cwd, "/tmp/nested") == 0);
   cJSON_Delete(json);
}

static void test_hook_payload_cwd_reads_nested_string(void)
{
   const char *payload = "{\"tool_input\":\"{\\\"command\\\":\\\"date\\\","
                         "\\\"workdir\\\":\\\"/tmp/string\\\"}\"}";
   cJSON *json = cJSON_Parse(payload);
   assert(json != NULL);
   char cwd[128];
   assert(hook_payload_cwd(json, cwd, sizeof(cwd)) == 1);
   assert(strcmp(cwd, "/tmp/string") == 0);
   cJSON_Delete(json);
}

static void test_hook_scope_project_is_not_configured_workspace(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-hook-scope-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char config_dir[512];
   char aimee_dir[512];
   char config_path[512];
   char workspace_root[512];
   char project_root[512];
   char manifest_path[512];
   snprintf(config_dir, sizeof(config_dir), "%s/.config", root);
   snprintf(aimee_dir, sizeof(aimee_dir), "%s/aimee", config_dir);
   snprintf(config_path, sizeof(config_path), "%s/aimee.yaml", aimee_dir);
   snprintf(workspace_root, sizeof(workspace_root), "%s/configured-workspace", root);
   snprintf(project_root, sizeof(project_root), "%s/nested-project", workspace_root);
   snprintf(manifest_path, sizeof(manifest_path), "%s/aimee.workspace.yaml", project_root);
   assert(mkdir(config_dir, 0700) == 0);
   assert(mkdir(aimee_dir, 0700) == 0);
   assert(mkdir(workspace_root, 0700) == 0);
   assert(mkdir(project_root, 0700) == 0);

   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fprintf(fp, "workspaces:\n  - %s\n", workspace_root);
   fclose(fp);
   fp = fopen(manifest_path, "w");
   assert(fp != NULL);
   fputs("id: stable-hook-project\n", fp);
   fclose(fp);

   const char *old_home_env = getenv("HOME");
   char *old_home = old_home_env ? strdup(old_home_env) : NULL;
   const char *old_no_cache_env = getenv("AIMEE_NO_CACHE");
   char *old_no_cache = old_no_cache_env ? strdup(old_no_cache_env) : NULL;
   assert(setenv("HOME", root, 1) == 0);
   assert(setenv("AIMEE_NO_CACHE", "1", 1) == 0);

   char workspace[512];
   char project[512];
   hook_scope_labels_for_cwd(project_root, workspace, sizeof(workspace), project, sizeof(project));
   assert(strcmp(workspace, "configured-workspace") == 0);
   assert(strcmp(project, "stable-hook-project") == 0);

   if (old_home)
      assert(setenv("HOME", old_home, 1) == 0);
   else
      assert(unsetenv("HOME") == 0);
   if (old_no_cache)
      assert(setenv("AIMEE_NO_CACHE", old_no_cache, 1) == 0);
   else
      assert(unsetenv("AIMEE_NO_CACHE") == 0);
   free(old_home);
   free(old_no_cache);

   assert(unlink(manifest_path) == 0);
   assert(unlink(config_path) == 0);
   assert(rmdir(project_root) == 0);
   assert(rmdir(workspace_root) == 0);
   assert(rmdir(aimee_dir) == 0);
   assert(rmdir(config_dir) == 0);
   assert(rmdir(root) == 0);
}

int main(void)
{
   printf("cmd_hooks_scope: ");
   test_hook_payload_session_id_prefers_payload();
   test_hook_payload_session_id_env_fallback_order();
   test_hook_payload_session_id_rejects_unsafe_ids();
   test_hook_payload_cwd_prefers_top_level();
   test_hook_payload_cwd_reads_nested_object();
   test_hook_payload_cwd_reads_nested_string();
   test_hook_scope_project_is_not_configured_workspace();
   printf("all tests passed\n");
   return 0;
}
