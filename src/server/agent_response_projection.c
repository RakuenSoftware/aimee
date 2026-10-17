#include "agent_response_projection.h"
#include "gw_stage_governance.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int agent_response_govern(aimee_response_t *response, int enabled, int policy, char *first_denied,
                          size_t capacity)
{
   int count = aimee_ir_response_tool_count(response);
   if (!enabled || !count)
      return 0;
   struct call
   {
      char *id, *name;
   } *before = calloc((size_t)count, sizeof *before);
   if (!before)
      return -1;
   int result = -1;
   for (int i = 0; i < count; i++)
   {
      const aimee_block_t *tool = aimee_ir_response_tool(response, i);
      if (!tool->tool_id || !tool->tool_name)
         goto done;
      before[i].id = strdup(tool->tool_id);
      before[i].name = strdup(tool->tool_name);
      if (!before[i].id || !before[i].name)
         goto done;
   }
   result = gw_ir_response_run_governance(response, enabled, policy);
   int kept = aimee_ir_response_tool_count(response);
   for (int i = 0; i < count; i++)
   {
      int found = 0;
      for (int j = 0; j < kept; j++)
         if (!strcmp(before[i].id, aimee_ir_response_tool(response, j)->tool_id))
         {
            found = 1;
            break;
         }
      if (found)
         continue;
      if (first_denied && capacity && !first_denied[0])
         snprintf(first_denied, capacity, "%s", before[i].name);
      if (aimee_ir_session_emit_tool_result(
              before[i].id, "Tool call denied by host governance; it was not executed.", 1))
      {
         result = -1;
         break;
      }
   }
done:
   for (int i = 0; i < count; i++)
   {
      free(before[i].id);
      free(before[i].name);
   }
   free(before);
   if (result < 0)
      aimee_ir_session_fail_capture();
   return result;
}
int agent_response_consume_final(aimee_response_t *response)
{
   int count = aimee_ir_response_tool_count(response), responds = 0;
   for (int i = 0; i < count; i++)
   {
      const aimee_block_t *tool = aimee_ir_response_tool(response, i);
      if (tool->tool_name && !strcmp(tool->tool_name, "respond"))
         responds++;
   }
   for (int i = 0; responds && i < count; i++)
   {
      const aimee_block_t *tool = aimee_ir_response_tool(response, i);
      if (!tool->tool_name || strcmp(tool->tool_name, "respond"))
         continue;
      const cJSON *answer = cJSON_GetObjectItemCaseSensitive(tool->tool_input, "message");
      const char *result =
          responds == count
              ? (cJSON_IsString(answer) ? answer->valuestring : "Final answer consumed by host.")
              : "Final-answer call ignored because this response also requests tool execution.";
      if (aimee_ir_session_emit_tool_result(tool->tool_id, result, responds != count))
         return -1;
   }
   return agent_ir_strip_delegate_respond(response);
}
