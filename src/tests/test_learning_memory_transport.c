/* Exercise the production proposal consumer after retiring memory's typed ABI. */
#include <assert.h>
#include "../modules/learning/learning_router.c"

static int available = 1;
static const char *reply_status = "ok";
static cJSON *last_request;
const char *session_id(void)
{
   return "learning-transport-session";
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.runtime") == 0);
   cJSON_Delete(last_request);
   last_request = cJSON_Duplicate(args, 1);
   if (!available)
      return 0;
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", reply_status);
   return 1;
}
int db2_collab_rules_propose(const char *text, const char *reason, const char *source)
{
   (void)text;
   (void)reason;
   (void)source;
   assert(0);
   return -1;
}
int db2_artifact_write(const char *id, const char *kind, const char *state, const char *scope_kind,
                       const char *scope_id, const char *operator_id, double confidence,
                       const char *payload_json)
{
   (void)id;
   (void)kind;
   (void)state;
   (void)scope_kind;
   (void)scope_id;
   (void)operator_id;
   (void)confidence;
   (void)payload_json;
   assert(0);
   return -1;
}
int main(void)
{
   learning_proposal_t proposal = {0};
   proposal.target_memory_id = 77;
   const char *sinks[] = {"reranker", "supersede", "workflow"};
   for (size_t i = 0; i < 3; i++)
   {
      snprintf(proposal.sink, sizeof(proposal.sink), "%s", sinks[i]);
      snprintf(proposal.action_json, sizeof(proposal.action_json), "%s",
               "{\"future_field\":\"retained\",\"new_content\":\"replacement\"}");
      assert(learning_apply_sink(&proposal) == 0);
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last_request, "operation")),
                    "learning-apply") == 0);
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last_request, "sink")), sinks[i]) ==
             0);
      assert(cJSON_GetObjectItem(last_request, "target_memory_id")->valueint == 77);
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last_request, "session_id")),
                    session_id()) == 0);
      assert(cJSON_HasObjectItem(cJSON_GetObjectItem(last_request, "action"), "future_field"));
      reply_status = "error";
      assert(learning_apply_sink(&proposal) == -1);
      reply_status = "ok";
      available = 0;
      assert(learning_apply_sink(&proposal) == -1);
      available = 1;
   }
   snprintf(proposal.action_json, sizeof(proposal.action_json), "malformed");
   assert(learning_apply_sink(&proposal) == -1);
   cJSON_Delete(last_request);
   return 0;
}
