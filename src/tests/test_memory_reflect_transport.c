#include "aimee.h"
#include "cmd_memory_internal.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int unavailable, explicit_scope, view_mode;
static const char *forced;
static char labels_path[4096], artifact_path[4096];
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
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(view_mode && !strcmp(method, view_mode == 1   ? "memory.briefing"
                                       : view_mode == 2 ? "memory.alerts"
                                       : view_mode == 3 ? "memory.audit"
                                                        : "memory.calibrate"));
   if (view_mode >= 3)
   {
      assert(!strcmp(jo_cstr(args, "labels_tsv"), "文 query\t9007199254740993\n"));
      assert(jo_int(args, "limit", 0) == 4 && jo_int(args, "candidate_limit", 0) == 9);
      assert(jo_int(args, "rounds", 0) == 3);
   }
   if (view_mode == 1)
      assert(jo_int(args, "limit_tokens", 0) == 768);
   else if (view_mode == 2)
      assert(!strcmp(jo_cstr(args, "since"), "2026-09-01"));
   assert(!strcmp(jo_cstr(args, "project"), "reflect-project"));
   assert(!strcmp(jo_cstr(args, "profile"), "compact"));
   int json = !strcmp(jo_cstr(args, "format"), "json");
   if (json)
      assert(!strcmp(jo_cstr(args, "fields"), view_mode == 1 ? "key_facts" : "stale_pending"));
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(out, "artifact", "{\"case_count\":1}\n");
   cJSON_AddStringToObject(out, "output",
                           json ? "{\"memory_id\":9223372036854775807}"
                                : (view_mode == 1 ? "Go-owned briefing\n" : "Go-owned alerts\n"));
   char *raw = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return raw;
}
static void invoke(int json)
{
   app_ctx_t ctx = {.json_output = json,
                    .json_fields = view_mode ? (view_mode == 1 ? "key_facts" : "stale_pending")
                                             : "results,contradictions",
                    .response_profile = "compact"};
   if (view_mode >= 3)
   {
      char *args[] = {"--labels", labels_path, "--limit", "4",       "--candidate-limit",
                      "9",        "--rounds",  "3",       "--write", artifact_path};
      if (view_mode == 3)
         mem_audit(&ctx, 10, args);
      else
         mem_calibrate(&ctx, 10, args);
      return;
   }
   if (view_mode == 2)
   {
      char *args[] = {"--since", "2026-09-01"};
      mem_alerts(&ctx, 2, args);
      return;
   }
   if (view_mode)
   {
      char *args[] = {"--limit-tokens", "768"};
      mem_briefing(&ctx, 2, args);
      return;
   }
   char *args[] = {"needle",        "query",           "--limit", "7",
                   "--draft-rule",  "--synthesize",    "--scope", "project",
                   "--scope-value", "explicit-project"};
   mem_reflect(&ctx, explicit_scope ? 10 : 6, args);
}
int main(void)
{
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   assert(snprintf(labels_path, sizeof(labels_path), "%s/aimee-labels-XXXXXX", tmp) <
          (int)sizeof(labels_path));
   assert(snprintf(artifact_path, sizeof(artifact_path), "%s/aimee-artifact-XXXXXX", tmp) <
          (int)sizeof(artifact_path));
   int labels_fd = mkstemp(labels_path), artifact_fd = mkstemp(artifact_path);
   assert(labels_fd >= 0 && artifact_fd >= 0);
   close(artifact_fd);
   FILE *fp = fdopen(labels_fd, "wb");
   assert(fp && fputs("文 query\t9007199254740993\n", fp) >= 0 && fclose(fp) == 0);
   for (view_mode = 0; view_mode < 5; view_mode++)
   {
      unavailable = 0;
      forced = NULL;
      for (int scope = 0; scope < (view_mode ? 1 : 2); scope++)
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
            assert(strstr(
                buf, json ? "9223372036854775807"
                          : (view_mode ? (view_mode == 1 ? "Go-owned briefing" : "Go-owned alerts")
                                       : "Go-owned reflection")));
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
   }
   fp = fopen(artifact_path, "rb");
   char artifact[128] = {0};
   assert(fp && fread(artifact, 1, sizeof(artifact) - 1, fp) > 0 && fclose(fp) == 0);
   assert(!strcmp(artifact, "{\"case_count\":1}\n"));
   unlink(labels_path);
   unlink(artifact_path);
   return 0;
}
