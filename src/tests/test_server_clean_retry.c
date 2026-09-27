#include "aimee.h"
#include "primary_session_adapter.h"
#include "request_context.h"
#include "db1_client/session_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int server_clean_retry_begin(const primary_session_request_t *, cJSON **, char **, char[65],
                                    char *, size_t);
extern int server_clean_retry_admit(const void *, size_t, const char *);
extern int server_clean_retry_finish(int, const agent_result_t *);
static request_context_t ctx;
static int calls, refuse;
const request_context_t *request_context_get(void)
{
   return &ctx;
}
void request_context_set(const request_context_t *v)
{
   ctx = *v;
}
int request_context_refuse_assembly(const char *reason)
{
   ctx.context_refused = 1;
   snprintf(ctx.context_refusal_kind, sizeof(ctx.context_refusal_kind), "%s", reason);
   return 0;
}
int policy_action_inherit(const char *sid)
{
   return strcmp(sid, "session");
}
static const char *text(const cJSON *j, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
   return s ? s : "";
}
int db1_governed_action_apply(const char *principal, const char *sid, const char *wire, char *reply,
                              size_t size)
{
   assert(!strcmp(principal, "alice") && !strcmp(sid, "session"));
   calls++;
   cJSON *j = cJSON_Parse(wire), *body = cJSON_GetObjectItemCaseSensitive(j, "retry");
   const char *op = text(j, "operation");
   if (!strcmp(op, "retry_begin"))
   {
      assert(!strcmp(text(body, "input"), "current user constraints"));
      assert(!cJSON_GetObjectItemCaseSensitive(body, "policy"));
   }
   else if (!strcmp(op, "retry_send"))
   {
      cJSON *call = cJSON_GetObjectItemCaseSensitive(body, "call");
      assert(strlen(text(call, "payload_sha256")) == 64);
      assert(strlen(text(call, "source_reference")) == 64);
      assert(strcmp(text(call, "source_reference"), ctx.memory_source_release));
   }
   else
      assert(!strcmp(op, "retry_finish"));
   cJSON_Delete(j);
   snprintf(reply, size,
            "{\"allowed\":%s,\"reason\":\"fixture\",\"attempt_id\":\"host-issued\",\"summary\":"
            "\"host observations\"}",
            refuse ? "false" : "true");
   return 0;
}
int main(void)
{
   strcpy(ctx.principal, "alice");
   strcpy(ctx.request_id, "request");
   primary_session_request_t req = {.aimee_session_id = "session",
                                    .user_prompt = "current user constraints"};
   cJSON *input = cJSON_Parse(
       "{\"clean_retry\":{\"previous_attempt\":\"prior\",\"replace_constraints\":true}}");
   req.task_request = input;
   cJSON *messages = cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"FAILED_SPECULATION\"}]");
   char attempt[65] = {0}, error[512] = {0};
   char *summary = NULL;
   assert(server_clean_retry_begin(&req, &messages, &summary, attempt, error, sizeof(error)) == 0);
   assert(cJSON_GetArraySize(messages) == 0 && !strcmp(ctx.retry_attempt, "host-issued"));
   assert(!strcmp(summary, "host observations") && !strstr(summary, "FAILED_SPECULATION"));
   strcpy(ctx.memory_source_release, "opaque-capability");
   assert(server_clean_retry_admit("provider bytes", 14, "receipt") == 0);
   refuse = 1;
   assert(server_clean_retry_admit("provider bytes", 14, "receipt") == -1 && ctx.context_refused);
   refuse = 0;
   agent_result_t result = {0};
   assert(server_clean_retry_finish(-1, &result) == 0 && !ctx.retry_attempt[0]);
   free(summary);
   cJSON_Delete(messages);
   cJSON_Delete(input);
   int before = calls;
   input = cJSON_Parse("{\"clean_retry\":{\"max_attempts\":999}}");
   req.task_request = input;
   messages = NULL;
   summary = NULL;
   assert(server_clean_retry_begin(&req, &messages, &summary, attempt, error, sizeof(error)) ==
              -1 &&
          calls == before);
   cJSON_Delete(input);
   puts("clean retry host: current constraints, isolated history, private reservations and failure "
        "persistence passed");
   return 0;
}
