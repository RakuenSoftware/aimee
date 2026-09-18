/* Actual CLI consumers preserve scope and complete Go responses. */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void mem_assemble(app_ctx_t *, int, char **);
void mem_cognify(app_ctx_t *, int, char **);
void mem_drain(app_ctx_t *, int, char **);
static cJSON *captured;
static const char *forced;
static int unavailable;
static int assembly_explain;
static char text[16001];
void emit_json_ctx(cJSON *json, const char *fields, const char *profile)
{
   (void)fields;
   (void)profile;
   captured = json;
}
void kb_client_memory_scope_context_apply(cJSON *args)
{
   cJSON_AddBoolToObject(args, "scope_context", 1);
   cJSON_AddStringToObject(args, "project", "cognify-test");
}
const char *config_embedder_command_current(const char *requested)
{
   (void)requested;
   return "";
}
char *kb_client_queue_drain_json(const char *command, int timeout)
{
   (void)command;
   (void)timeout;
   return strdup("{\"status\":\"ok\",\"processed\":4}");
}
char *kb_v1_action_request_with_timeout(const char *method, cJSON *args, int timeout)
{
   assert(timeout == 60000);
   assert(jo_bool(args, "scope_context", 0));
   assert(!strcmp(jo_cstr(args, "project"), "cognify-test"));
   if (!strcmp(method, "memory.cognify"))
      assert(jo_int(args, "unit", 0) == 42);
   else
      assert(!strcmp(method, "memory.cognify_drain"));
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(out, "summary", text);
   cJSON_AddNumberToObject(out, "processed", 2);
   cJSON_AddNumberToObject(out, "pending", 3);
   char *raw = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return raw;
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strcmp(method, "memory.assemble_context"));
   assert(jo_bool(args, "scope_context", 0));
   assert(!strcmp(jo_cstr(args, "project"), "cognify-test"));
   assert(!strcmp(jo_cstr(args, "task_hint"), "cert auth"));
   assert(jo_bool(args, "explain", 0) == assembly_explain);
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   return strdup(
       "{\"status\":\"ok\",\"context\":\"shared Go context\",\"explain_text\":\"shared Go "
       "explanation\",\"budget\":{\"used_tokens\":8},\"candidates\":[{\"id\":"
       "\"9223372036854775807\",\"selected\":true}]}");
}

static void run(int drain)
{
   app_ctx_t ctx = {.json_output = 1};
   char *args[] = {"--unit=42"};
   if (drain == 2)
   {
      char *assembly_args[] = {"cert", "auth", "--explain"};
      mem_assemble(&ctx, assembly_explain ? 3 : 2, assembly_args);
   }
   else if (drain)
      mem_drain(&ctx, 0, NULL);
   else
      mem_cognify(&ctx, 1, args);
}
static void rejected(int drain)
{
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      run(drain);
      _exit(0);
   }
   int status;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}
int main(void)
{
   memset(text, 'x', sizeof(text) - 1);
   run(0);
   assert(!strcmp(jo_cstr(captured, "summary"), text));
   cJSON_Delete(captured);
   run(1);
   assert(jo_int(captured, "processed", 0) == 4);
   cJSON *cog = cJSON_GetObjectItemCaseSensitive(captured, "cognify");
   assert(jo_int(cog, "processed", 0) == 2 && jo_int(cog, "pending", 0) == 3);
   cJSON_Delete(captured);
   forced = "{\"status\":\"ok\",\"queued\":true}";
   run(0);
   assert(jo_bool(captured, "queued", 0));
   cJSON_Delete(captured);
   forced = "{\"status\":\"disabled\",\"processed\":0}";
   run(1);
   assert(!strcmp(jo_cstr(cJSON_GetObjectItemCaseSensitive(captured, "cognify"), "status"),
                  "disabled"));
   cJSON_Delete(captured);
   forced = NULL;
   for (assembly_explain = 0; assembly_explain <= 1; assembly_explain++)
   {
      run(2);
      assert(!strcmp(jo_cstr(captured, "context"), "shared Go context"));
      assert(!strcmp(jo_cstr(captured, "task_hint"), "cert auth"));
      assert(!cJSON_HasObjectItem(captured, "explain_text"));
      cJSON *candidate =
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(captured, "candidates"), 0);
      assert(!strcmp(jo_cstr(candidate, "id"), "9223372036854775807"));
      cJSON_Delete(captured);
   }
   assembly_explain = 1;
   const char *errors[] = {"bad-json", "{}", "{\"status\":\"error\",\"kind\":\"forbidden\"}"};
   for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
   {
      forced = errors[i];
      rejected(0);
      rejected(1);
      rejected(2);
   }
   forced = "{\"status\":\"ok\",\"context\":\"partial\"}";
   rejected(2);
   unavailable = 1;
   rejected(0);
   rejected(1);
   rejected(2);
   return 0;
}
