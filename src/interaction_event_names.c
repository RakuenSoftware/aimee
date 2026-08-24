/* interaction_event_names.c: what an interaction event is called.
 *
 * An enum-to-string map over a type the caller already holds. It reads no
 * database and answers no query, so it is not storage, and it lived in a DB1
 * source only because the function that records an event used it to decide
 * what to write.
 *
 * The recording moved to the module; this did not. The module has no use for
 * it either: db1_interaction_event_record takes the event's name as the text it
 * was always stored as, so the conversion happens once, here, on the side that
 * has the enum.
 */
#include "db1_client/interaction_events.h"

const char *ie_event_type_name(ie_event_type_t type)
{
   switch (type)
   {
   case IE_USER_TURN:
      return "user_turn";
   case IE_AGENT_TURN:
      return "agent_turn";
   case IE_TOOL_CALL:
      return "tool_call";
   case IE_TOOL_OUTCOME:
      return "tool_outcome";
   case IE_DELEGATE_EXIT:
      return "delegate_exit";
   case IE_GUARDRAIL_DECISION:
      return "guardrail_decision";
   case IE_SKILL_ACTIVATION:
      return "skill_activation";
   case IE_USER_CORRECTION:
      return "user_correction";
   case IE_FAILOVER_EVENT:
      return "failover_event";
   case IE_MCP_PACKAGE_CHECK:
      return "mcp_package_check";
   default:
      return "unknown";
   }
}
