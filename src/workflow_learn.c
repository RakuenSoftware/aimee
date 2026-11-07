/* Host connections for module-owned workflow learning. Capture trusted caller
 * inputs and forward accepted opaque evidence to the learning router. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "headers/module_commands.h"
#include "kb_client.h"
#include "modules/learning/learning_implicit.h"
#include "log.h"
#include <unistd.h>
#pragma weak learning_implicit_record_workflow_evidence
extern cJSON *aimee_module_host_command_context(void) __attribute__((weak));
#pragma weak aimee_module_commands_capture_caller_context
#pragma weak aimee_module_commands_dispatch_internal_context_timeout

static int workflow_dispatch(const cJSON *args, const cJSON *context, cJSON **result)
{
   return context ? aimee_module_commands_dispatch_internal_context_timeout("memory.runtime", args,
                                                                            context, 60000, result)
                  : aimee_module_commands_dispatch_internal("memory.runtime", args, result);
}

cJSON *workflow_execute(const cJSON *input)
{
   cJSON *args = cJSON_Duplicate(input, 1), *plan = NULL;
   if (!args)
      return NULL;
   const char *captured[] = {"operation", "cwd", "workspaces"};
   for (size_t i = 0; i < sizeof(captured) / sizeof(captured[0]); i++)
      while (cJSON_HasObjectItem(args, captured[i]))
         cJSON_DeleteItemFromObjectCaseSensitive(args, captured[i]);
   cJSON_AddStringToObject(args, "operation", "workflow-plan");
   char cwd[MAX_PATH_LEN] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   cJSON_AddStringToObject(args, "cwd", cwd);
   cJSON *roots = cJSON_AddArrayToObject(args, "workspaces");
   if (!roots)
   {
      cJSON_Delete(args);
      return NULL;
   }
   for (int i = 0; i < config_workspace_count(); i++)
      cJSON_AddItemToArray(roots, cJSON_CreateString(config_workspaces(i)));
   /* Capture the original verifier-owned context once. A selected factory's
    * failure cannot fall back to a broader default service identity. */
   cJSON *context = NULL;
   int captured_context = aimee_module_commands_capture_caller_context
                              ? aimee_module_commands_capture_caller_context(&context)
                              : (aimee_module_host_command_context ? -1 : 0);
   if (captured_context < 0 ||
       (context && !aimee_module_commands_dispatch_internal_context_timeout))
   {
      cJSON_Delete(context);
      cJSON_Delete(args);
      return NULL;
   }
   int rc = workflow_dispatch(args, context, &plan);
   cJSON_Delete(args);
   if (rc <= 0)
   {
      cJSON_Delete(plan);
      cJSON_Delete(context);
      return NULL;
   }
   const cJSON *request = cJSON_GetObjectItemCaseSensitive(plan, "request");
   if (!cJSON_IsObject(request))
   {
      cJSON_Delete(context);
      return plan;
   }
   cJSON *write_args = cJSON_Duplicate(request, 1);
   cJSON *result_args = cJSON_CreateObject();
   if (!write_args || !result_args)
   {
      cJSON_Delete(write_args);
      cJSON_Delete(result_args);
      cJSON_Delete(plan);
      cJSON_Delete(context);
      return NULL;
   }
   cJSON_AddStringToObject(result_args, "operation", "workflow-result");
   cJSON_AddItemToObject(result_args, "request", cJSON_DetachItemFromObject(plan, "request"));
   char *receipt = kb_v1_action_request("memory.upsert_workflow", write_args);
   cJSON_AddStringToObject(result_args, "receipt", receipt ? receipt : "");
   free(receipt);
   cJSON_Delete(plan);
   cJSON *result = NULL;
   rc = workflow_dispatch(result_args, context, &result);
   cJSON_Delete(context);
   cJSON_Delete(result_args);
   if (rc <= 0)
   {
      cJSON_Delete(result);
      return NULL;
   }
   return result;
}

void workflow_observe_bash(const char *command)
{
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return;
   cJSON_AddStringToObject(args, "mode", "observe");
   cJSON_AddStringToObject(args, "command", command ? command : "");
   cJSON *result = workflow_execute(args);
   cJSON_Delete(args);
   if (jo_bool(result, "learned", 0))
   {
      const char *workspace = jo_cstr(result, "workspace");
      const char *signal = jo_cstr(result, "signal_type");
      LOG_INFO("workflow_learn", "learned workflow rule: workflow:%s:%s", workspace, signal);
      const cJSON *refs = cJSON_GetObjectItemCaseSensitive(result, "learning_evidence_refs");
      if (!refs)
         learning_implicit_record_workflow(workspace, signal, jo_cstr(result, "rule"));
      else if (cJSON_IsArray(refs) && learning_implicit_record_workflow_evidence)
      {
         char *wire_refs = cJSON_PrintUnformatted(refs);
         if (wire_refs)
            learning_implicit_record_workflow_evidence(workspace, signal, jo_cstr(result, "rule"),
                                                       wire_refs);
         free(wire_refs);
      }
      else
         LOG_WARN("workflow_learn", "accepted workflow evidence cannot be forwarded");
   }
   cJSON_Delete(result);
}
