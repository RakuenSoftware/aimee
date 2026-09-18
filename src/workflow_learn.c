/* Host connections for Go-owned workflow learning. The host captures cwd and
 * configured roots, transports the owner's write request to the authenticated
 * KB, and notifies the learning owner only after Go accepts the write receipt. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "headers/module_commands.h"
#include "kb_client.h"
#include "modules/learning/learning_implicit.h"
#include "log.h"
#include <unistd.h>

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
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &plan);
   cJSON_Delete(args);
   if (rc <= 0)
   {
      cJSON_Delete(plan);
      return NULL;
   }
   const cJSON *request = cJSON_GetObjectItemCaseSensitive(plan, "request");
   if (!cJSON_IsObject(request))
      return plan;
   cJSON *write_args = cJSON_Duplicate(request, 1);
   cJSON *result_args = cJSON_CreateObject();
   if (!write_args || !result_args)
   {
      cJSON_Delete(write_args);
      cJSON_Delete(result_args);
      cJSON_Delete(plan);
      return NULL;
   }
   cJSON_AddStringToObject(result_args, "operation", "workflow-result");
   cJSON_AddItemToObject(result_args, "request", cJSON_DetachItemFromObject(plan, "request"));
   char *receipt = kb_v1_action_request("memory.upsert_workflow", write_args);
   cJSON_AddStringToObject(result_args, "receipt", receipt ? receipt : "");
   free(receipt);
   cJSON_Delete(plan);
   cJSON *result = NULL;
   rc = aimee_module_commands_dispatch_internal("memory.runtime", result_args, &result);
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
      learning_implicit_record_workflow(workspace, signal, jo_cstr(result, "rule"));
   }
   cJSON_Delete(result);
}
