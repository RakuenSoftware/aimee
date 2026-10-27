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

   if (aimee_ir_response_complete_valid(&out->canonical_response))
   {
      out->success = 0;
      snprintf(out->error, sizeof out->error, "agent did not return a complete canonical answer");
      return;
   }
   char *display = aimee_ir_response_text_alloc(&out->canonical_response);
   if (!display)
   {
      out->success = 0;
      snprintf(out->error, sizeof out->error, "cannot project canonical answer display");
      return;
   }
   for (int i = 0; i < out->canonical_response.n_content; i++)
      if (out->canonical_response.content[i].type == AIMEE_BLK_IMAGE ||
          out->canonical_response.content[i].type == AIMEE_BLK_DOCUMENT)
      {
         free(display);
         return; /* Empty text does not make an authored media answer degenerate. */
      }
   int unexecuted_tool_plan = out->tool_calls == 0 && out->turns == 0 &&
                              liveness_is_unexecuted_tool_plan_response(display);
   int degenerate = liveness_is_degenerate_response(display);
   free(display);
   if (!degenerate && !unexecuted_tool_plan)
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
