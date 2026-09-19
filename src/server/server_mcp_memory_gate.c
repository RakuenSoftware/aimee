/* MCP memory transport: enforce host capabilities against the Go owner's plan. */
#include "server_mcp_memory_gate.h"
#include "server.h" /* CAP_MEMORY_ADMIN / CAP_MEMORY_WRITE */
#include "aimee.h"
#include "kb_client.h"
#include "json_fluent.h"
#include <aimee/core/event_bus/module_protocol.h>
#include <string.h>

const char *mcp_mutate_verb_method(const char *verb)
{
   if (!verb)
      return NULL;
   if (strcmp(verb, "store") == 0)
      return "memory.store";
   if (strcmp(verb, "update") == 0)
      return "memory.update";
   if (strcmp(verb, "supersede") == 0)
      return "memory.supersede";
   /* The destructive one, and the reason this table exists: the RPC method twin
    * requires CAP_MEMORY_ADMIN while the MCP tool required nothing. */
   if (strcmp(verb, "forget") == 0)
      return "memory.delete";
   if (strcmp(verb, "affirm") == 0)
      return "memory.touch";
   if (strcmp(verb, "reject") == 0)
      return "memory.reject";
   return NULL;
}

cJSON *server_mcp_memory_maintain_command(uint32_t capabilities, const cJSON *args)
{
   cJSON *plan = server_invoke_module_operation("memory.runtime", "maintenance-model-plan", args,
                                                "memory maintenance policy unavailable");
   cJSON *execute = cJSON_GetObjectItemCaseSensitive(plan, "execute");
   const char *required = jo_cstr(plan, "required_capability");
   uint32_t capability = strcmp(required, "write") == 0   ? CAP_MEMORY_WRITE
                         : strcmp(required, "admin") == 0 ? CAP_MEMORY_ADMIN
                                                          : 0;
   if (strcmp(jo_cstr(plan, "status"), "ok") != 0 || !cJSON_IsBool(execute) || !capability)
   {
      cJSON_Delete(plan);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "memory maintenance policy unavailable",
                                    NULL);
   }
   if (!(capabilities & capability))
   {
      cJSON_Delete(plan);
      return server_error_kind_json(SERVER_ERR_PERMISSION_DENIED,
                                    "forbidden: insufficient capabilities for memory maintenance",
                                    NULL);
   }
   if (cJSON_IsFalse(execute))
   {
      if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(plan, "text")) &&
          !cJSON_HasObjectItem(plan, "request"))
         return plan;
      cJSON_Delete(plan);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "invalid memory maintenance plan",
                                    NULL);
   }
   cJSON *request = cJSON_DetachItemFromObjectCaseSensitive(plan, "request");
   cJSON_Delete(plan);
   if (!cJSON_IsObject(request) ||
       !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "model_policy")))
   {
      cJSON_Delete(request);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "invalid memory maintenance plan",
                                    NULL);
   }
   char *raw = kb_v1_action_request("memory.maintenance_run", request);
   cJSON *reply = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                      ? cJSON_ParseWithOpts(raw, NULL, 1)
                      : NULL;
   free(raw);
   if (cJSON_IsObject(reply) && strcmp(jo_cstr(reply, "status"), "ok") == 0 &&
       cJSON_IsString(cJSON_GetObjectItemCaseSensitive(reply, "text")))
      return reply;
   cJSON_Delete(reply);
   return server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                 "memory maintenance failed or returned an invalid response", NULL);
}
