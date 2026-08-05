#include "learning_signal_policy.h"

#include <aimee/learning/module_api.h>

#include <string.h>

int learning_signal_policy_sink_mask(const char *signal, uint32_t *sink_mask)
{
   if (!signal || !signal[0] || !sink_mask)
      return -1;

   *sink_mask = 0;
   if (strcmp(signal, "thumb_up") == 0 || strcmp(signal, "thumb_down") == 0)
      *sink_mask = AIMEE_LEARNING_SINK_RERANKER;
   else if (strcmp(signal, "correction") == 0)
      *sink_mask =
          AIMEE_LEARNING_SINK_RERANKER | AIMEE_LEARNING_SINK_SUPERSEDE | AIMEE_LEARNING_SINK_RULE;
   else if (strcmp(signal, "preference_statement") == 0 || strcmp(signal, "mark_rule") == 0)
      *sink_mask = AIMEE_LEARNING_SINK_RULE;
   else if (strcmp(signal, "workflow_repetition") == 0)
      *sink_mask = AIMEE_LEARNING_SINK_WORKFLOW;
   return 0;
}
