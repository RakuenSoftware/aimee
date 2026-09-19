/* Console record readers carry scope and exact IDs to Go and forward output. */
#include "aimee.h"
#include "cmd_memory_internal.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int listing, unavailable;
static const char *forced;
static char payload[24000];

void kb_client_memory_scope_context_apply(cJSON *req)
{
   cJSON_AddBoolToObject(req, "scope_context", 1);
   cJSON_AddStringToObject(req, "project", "console-project");
}

char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strcmp(method, listing ? "memory.list" : "memory.get"));
   assert(!strcmp(jo_cstr(args, "view"), "console"));
   assert(jo_bool(args, "scope_context", 0));
   assert(!strcmp(jo_cstr(args, "project"), "console-project"));
   assert(!strcmp(jo_cstr(args, "fields"), "id,content"));
   assert(!strcmp(jo_cstr(args, "profile"), "compact"));
   if (listing)
   {
      assert(!strcmp(jo_cstr(args, "tier"), "L3"));
      assert(!strcmp(jo_cstr(args, "kind"), "pattern"));
      assert(jo_int(args, "limit", 0) == 1000);
   }
   else
      assert(!strcmp(jo_cstr(args, "id"), "9007199254740993"));
   int json = !strcmp(jo_cstr(args, "format"), "json");
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "ok");
   cJSON_AddStringToObject(reply, "output", json ? payload : "");
   char *wire = cJSON_PrintUnformatted(reply);
   cJSON_Delete(reply);
   return wire;
}

static void invoke(int json)
{
   app_ctx_t ctx = {
       .json_output = json, .json_fields = "id,content", .response_profile = "compact"};
   char *get[] = {"9007199254740993"};
   char *list[] = {"--tier", "L3", "--kind", "pattern", "--limit", "1000"};
   if (listing)
      mem_list(&ctx, 6, list);
   else
      mem_get(&ctx, 1, get);
}

int main(void)
{
   int start = snprintf(payload, sizeof(payload), "{\"id\":9007199254740993,\"content\":\"");
   memset(payload + start, 'x', 20000);
   strcpy(payload + start + 20000, "\"}\n");
   for (listing = 0; listing < 2; listing++)
   {
      for (int json = 0; json < 2; json++)
      {
         FILE *output = tmpfile();
         assert(output);
         fflush(stdout);
         int saved = dup(STDOUT_FILENO);
         assert(saved >= 0 && dup2(fileno(output), STDOUT_FILENO) >= 0);
         invoke(json);
         fflush(stdout);
         assert(dup2(saved, STDOUT_FILENO) >= 0);
         close(saved);
         rewind(output);
         char actual[sizeof(payload)];
         size_t n = fread(actual, 1, sizeof(actual) - 1, output);
         actual[n] = 0;
         assert(!strcmp(actual, json ? payload : ""));
         fclose(output);
      }
      const char *errors[] = {"not-json", "{}", "{\"status\":\"error\",\"kind\":\"not_found\"}",
                              "{\"status\":\"ok\"}", "{\"status\":\"ok\",\"output\":[]}"};
      for (int i = 0; i < 6; i++)
      {
         FILE *output = tmpfile();
         assert(output);
         pid_t pid = fork();
         assert(pid >= 0);
         if (!pid)
         {
            assert(dup2(fileno(output), STDOUT_FILENO) >= 0);
            forced = i < 5 ? errors[i] : NULL;
            unavailable = i == 5;
            invoke(1);
            _exit(0);
         }
         int status;
         assert(waitpid(pid, &status, 0) == pid);
         assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
         rewind(output);
         assert(fgetc(output) == EOF); /* Failure must never print a healthy []. */
         fclose(output);
      }
   }
   puts("PASS: console get/list forward exact Go output and refuse owner failures");
   return 0;
}
