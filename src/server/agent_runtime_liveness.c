/* agent_runtime_liveness.c: liveness checks shared by simple agent execution. */
#include "aimee.h"
#include "agent_types.h"
#include "liveness.h"

#include <stdio.h>
#include <stdlib.h>

void agent_reject_degenerate_plain_response(agent_result_t *out, const char *agent_name)
{
   if (!out || !out->success)
      return;

   if (agent_result_project_complete_text(out))
   {
      out->success = 0;
      snprintf(out->error, sizeof out->error, "agent did not return complete canonical text");
      return;
   }
   int unexecuted_tool_plan = out->tool_calls == 0 && out->turns == 0 &&
                              liveness_is_unexecuted_tool_plan_response(out->response);
   if (!liveness_is_degenerate_response(out->response) && !unexecuted_tool_plan)
      return;

   out->success = 0;
   out->abstained = 1;
   snprintf(out->abstain_reason, sizeof(out->abstain_reason), "degenerate response");
   snprintf(out->error, sizeof(out->error),
            unexecuted_tool_plan
                ? "delegate '%s' returned an unexecuted tool-use plan without tool execution"
                : "delegate '%s' returned raw tool-call markup or another degenerate response "
                  "without tool execution",
            agent_name && agent_name[0] ? agent_name : "unknown");
}
