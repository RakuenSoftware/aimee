#include "aimee.h"
#include "cmd_memory_internal.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int unavailable, explicit_scope;
static const char *forced;
void cmd_memory_apply_rerank_mode(const opt_parsed_t *opts)
{
   (void)opts;
}
int config_workspace_count(void)
{
   return 0;
}
const char *config_workspaces(int index)
{
   (void)index;
   return "";
}
void kb_client_memory_scope_context_apply(cJSON *args)
{
   cJSON_AddStringToObject(args, "project", "reflect-project");
}
char *kb_v1_action_request_with_timeout(const char *method, cJSON *args, int timeout)
{
   assert(!strcmp(method, "memory.reflect") && timeout == 60000);
   assert(!strcmp(jo_cstr(args, "query"), "needle query"));
   assert(jo_bool(args, "draft_rule", 0) && jo_bool(args, "synthesize", 0));
   assert(jo_int(args, "limit", 0) == 7);
   if (explicit_scope)
   {
      assert(!strcmp(jo_cstr(args, "scope_type"), "project"));
      assert(!strcmp(jo_cstr(args, "scope_value"), "explicit-project"));
      assert(!cJSON_HasObjectItem(args, "scope_context"));
   }
   else
   {
      assert(jo_bool(args, "scope_context", 0));
      assert(!strcmp(jo_cstr(args, "project"), "reflect-project"));
   }
   int json = !strcmp(jo_cstr(args, "format"), "json");
   if (json)
      assert(!strcmp(jo_cstr(args, "fields"), "results,contradictions"));
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(
       out, "output",
       json ? "{\"results\":[{\"id\":9223372036854775807}],\"contradictions\":[]}"
            : "Go-owned reflection\n");
   char *raw = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return raw;
}
static void invoke(int json)
{
   app_ctx_t ctx = {
       .json_output = json, .json_fields = "results,contradictions", .response_profile = "compact"};
   char *args[] = {"needle",        "query",           "--limit", "7",
                   "--draft-rule",  "--synthesize",    "--scope", "project",
                   "--scope-value", "explicit-project"};
   mem_reflect(&ctx, explicit_scope ? 10 : 6, args);
}
int main(void)
{
   for (int scope = 0; scope < 2; scope++)
   {
      explicit_scope = scope;
      for (int json = 0; json < 2; json++)
      {
         int fds[2];
         assert(pipe(fds) == 0);
         fflush(stdout);
         int saved = dup(STDOUT_FILENO);
         assert(saved >= 0);
         assert(dup2(fds[1], STDOUT_FILENO) >= 0);
         close(fds[1]);
         invoke(json);
         fflush(stdout);
         assert(dup2(saved, STDOUT_FILENO) >= 0);
         close(saved);
         char buf[1024];
         int n = read(fds[0], buf, sizeof(buf) - 1);
         assert(n > 0);
         buf[n] = 0;
         close(fds[0]);
         assert(strstr(buf, json ? "9223372036854775807" : "Go-owned reflection"));
      }
   }
   const char *errors[] = {"not-json", "{}", "{\"status\":\"error\"}", "{\"status\":\"ok\"}"};
   for (int i = 0; i < 5; i++)
   {
      forced = i < 4 ? errors[i] : NULL;
      unavailable = i == 4;
      pid_t pid = fork();
      assert(pid >= 0);
      if (pid == 0)
      {
         invoke(0);
         _exit(0);
      }
      int status;
      assert(waitpid(pid, &status, 0) == pid);
      assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
   }
   return 0;
}
