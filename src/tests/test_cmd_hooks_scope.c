#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include <aimee/translation/hook_ir.h>
#include "headers/cmd_hooks_scope.h"
#include "cJSON.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void test_hook_scope_project_is_not_configured_workspace(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-hook-scope-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char workspace_root[512];
   char project_root[512];
   char manifest_path[512];
   snprintf(workspace_root, sizeof(workspace_root), "%s/configured-workspace", root);
   snprintf(project_root, sizeof(project_root), "%s/nested-project", workspace_root);
   snprintf(manifest_path, sizeof(manifest_path), "%s/aimee.workspace.yaml", project_root);
   assert(mkdir(workspace_root, 0700) == 0);
   assert(mkdir(project_root, 0700) == 0);

   FILE *fp = fopen(manifest_path, "w");
   assert(fp != NULL);
   fputs("id: stable-hook-project\n", fp);
   fclose(fp);
   assert(config_workspace_add(workspace_root, NULL, NULL, NULL) == 0);

   char workspace[512];
   char project[512];
   hook_scope_labels_for_cwd(project_root, workspace, sizeof(workspace), project, sizeof(project));
   assert(strcmp(workspace, "configured-workspace") == 0);
   assert(strcmp(project, "stable-hook-project") == 0);
   assert(config_workspace_remove(workspace_root) == 0);

   assert(unlink(manifest_path) == 0);
   assert(rmdir(project_root) == 0);
   assert(rmdir(workspace_root) == 0);
   assert(rmdir(root) == 0);
}

static void test_owned_hook_ir(void)
{
   char id[401];
   memset(id, 'i', 400);
   id[400] = 0;
   cJSON *source =
       cJSON_Parse("{\"tool_name\":\"run\",\"namespace\":\"functions\","
                   "\"session_id\":\"native-session\",\"cwd\":\"/workspace\","
                   "\"tool_input\":{\"cwd\":\"/workspace/inner\",\"number\":9007199254740993},"
                   "\"tool_response\":{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":"
                   "\"failed\"}]}}");
   assert(source && cJSON_AddStringToObject(source, "tool_use_id", id));
   char *frame = cJSON_PrintUnformatted(source);
   assert(frame);
   aimee_hook_ir_t hook;
   assert(!aimee_hook_ir_parse(frame, strlen(frame), &hook));
   free(frame);
   cJSON_Delete(source);
   assert(!strcmp(hook.call.tool_name, "run") && !strcmp(hook.call.tool_namespace, "functions"));
   assert(!strcmp(hook.call.tool_id, id) && !strcmp(hook.result.tool_id, id));
   assert(!strcmp(hook.session_id, "native-session"));
   assert(!strcmp(hook.cwd, "/workspace") && !strcmp(hook.tool_cwd, "/workspace/inner"));
   assert(hook.result.tool_is_error &&
          cJSON_IsArray(cJSON_GetObjectItem(hook.result.tool_result, "content")));
   char *arguments = aimee_hook_ir_arguments(&hook);
   assert(arguments && strstr(arguments, "9007199254740993"));
   free(arguments);
   aimee_hook_ir_free(&hook);
   const char *strings[] = {"{ echo ok; }", "[ \"a\" = \"b\" ]", "echo raw", ""};
   for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++)
   {
      source = cJSON_CreateObject();
      assert(cJSON_AddStringToObject(source, "tool_name", "Bash"));
      assert(cJSON_AddStringToObject(source, "tool_input", strings[i]));
      frame = cJSON_PrintUnformatted(source);
      assert(!aimee_hook_ir_parse(frame, strlen(frame), &hook));
      assert(cJSON_IsString(hook.call.tool_input));
      arguments = aimee_hook_ir_arguments(&hook);
      assert(arguments && !strcmp(arguments, strings[i]));
      free(arguments);
      free(frame);
      cJSON_Delete(source);
      aimee_hook_ir_free(&hook);
   }
   const char *serialized =
       "{\"tool_name\":\"Bash\",\"tool_input\":\"{\\\"command\\\":\\\"echo ok\\\"}\"}";
   assert(!aimee_hook_ir_parse(serialized, strlen(serialized), &hook));
   assert(cJSON_IsObject(hook.call.tool_input) && !hook.call.tool_id && !hook.result.tool_id);
   aimee_hook_ir_free(&hook);
   const char *literal =
       "{\"tool_name\":\"Write\",\"tool_input\":{\"content\":\"literal \\\\u0000\"}}";
   assert(!aimee_hook_ir_parse(literal, strlen(literal), &hook));
   aimee_hook_ir_free(&hook);
   const char *invalid[] = {
       "{}",
       "[]",
       "{\"tool_name\":null}",
       "{\"tool_name\":\"a\",\"tool_name\":\"b\"}",
       "{\"tool_name\":\"x\"} trailing",
       "{\"tool_name\":\"bad\\u0000suffix\"}",
       "{\"tool_name\":\"x\",\"cwd\":\"/a\",\"workdir\":\"/b\"}",
       "{\"tool_name\":\"x\",\"tool_input\":\"{\\\"command\\\":\\\"a\\\",\\\"command\\\":\\\"b\\\"}"
       "\"}",
       "{\"tool_name\":\"x\",\"tool_input\":\"{} trailing\"}",
       "{\"tool_name\":\"x\",\"namespace\":\"a\",\"tool_namespace\":\"b\"}",
       "{\"tool_name\":\"x\",\"tool_use_id\":\"a\",\"tool_call_id\":\"b\"}",
       "{\"tool_name\":\"x\",\"tool_response\":{\"is_error\":true,\"isError\":false}}",
       "{\"tool_name\":\"\xc0\xaf\"}",
       "\x01{\"tool_name\":\"x\"}",
       "{\"tool_name\":\"unescaped\ncontrol\"}"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      memset(&hook, 'x', sizeof hook);
      assert(aimee_hook_ir_parse(invalid[i], strlen(invalid[i]), &hook));
      for (size_t j = 0; j < sizeof hook; j++)
         assert(!((unsigned char *)&hook)[j]);
   }
   const char nul[] = "{\"tool_name\":\"x\"}\0hidden";
   assert(aimee_hook_ir_parse(nul, sizeof nul - 1, &hook));
   char *large = malloc(131073);
   assert(large);
   memset(large, 'x', 131072);
   large[131072] = 0;
   source = cJSON_CreateObject();
   assert(cJSON_AddStringToObject(source, "tool_name", "Write"));
   assert(cJSON_AddStringToObject(source, "tool_input", large));
   frame = cJSON_PrintUnformatted(source);
   assert(!aimee_hook_ir_parse(frame, strlen(frame), &hook));
   arguments = aimee_hook_ir_arguments(&hook);
   assert(arguments && strlen(arguments) == 131072);
   free(arguments);
   free(frame);
   free(large);
   cJSON_Delete(source);
   aimee_hook_ir_free(&hook);
   puts(
       "  canonical hook admission: owned identities, arguments, results and exact framing passed");
}

int main(void)
{
   printf("cmd_hooks_scope: ");
   test_owned_hook_ir();
   test_hook_scope_project_is_not_configured_workspace();
   printf("all tests passed\n");
   return 0;
}
