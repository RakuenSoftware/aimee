#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/roundtable/module_api.h>

#include <stdio.h>
#include <string.h>

static void copy_severity(const char *claimed, char out[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u])
{
   snprintf(out, AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u, "%s",
            claimed && claimed[0] ? claimed : "suggestion");
}

static void cap_severity(const char *claimed, char out[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u])
{
   copy_severity(claimed && strcmp(claimed, "blocking") == 0 ? "suggestion" : claimed, out);
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   aimee_roundtable_replay_status_t status;
   int factual = 0;
   char claimed[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u];
   if (!invocation || !response_len || invocation->stage_id != AIMEE_ROUNDTABLE_STAGE_DELIBERATE ||
       response_capacity < AIMEE_ROUNDTABLE_RESPONSE_LEN ||
       aimee_roundtable_request_decode(request_body, request_len, &status, &factual, claimed,
                                       sizeof(claimed)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   aimee_roundtable_verify_action_t action = AIMEE_ROUNDTABLE_VERIFY_CAP;
   char severity[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u] = {0};
   switch (status)
   {
   case AIMEE_ROUNDTABLE_REPLAY_CONTRADICTED:
   case AIMEE_ROUNDTABLE_REPLAY_VACUOUS:
      action = AIMEE_ROUNDTABLE_VERIFY_REJECT;
      break;
   case AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE:
      action = AIMEE_ROUNDTABLE_VERIFY_DEGRADE;
      copy_severity(claimed, severity);
      break;
   case AIMEE_ROUNDTABLE_REPLAY_NO_EVIDENCE:
      cap_severity(claimed, severity);
      break;
   case AIMEE_ROUNDTABLE_REPLAY_MATCH:
   case AIMEE_ROUNDTABLE_REPLAY_CORRECTED:
      if (factual)
      {
         action = AIMEE_ROUNDTABLE_VERIFY_KEEP;
         copy_severity(claimed, severity);
      }
      else
         cap_severity(claimed, severity);
      break;
   }
   if (aimee_roundtable_response_encode(action, severity, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_ROUNDTABLE_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
