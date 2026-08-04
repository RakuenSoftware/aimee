#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/skills/module_api.h>
#include "skill_trigger_policy.h"

#include <stdlib.h>
#include <string.h>

static aimee_module_status_t handle_review(const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len)
{
   if (!request_body || request_len != AIMEE_SKILLS_REQUEST_LEN ||
       response_capacity < AIMEE_SKILLS_RESPONSE_LEN ||
       aimee_skills_get_u32(request_body) != AIMEE_SKILLS_REQUEST_MAGIC ||
       aimee_skills_get_u32(request_body + 4) != AIMEE_SKILLS_WIRE_VERSION)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   int count = (int)(int32_t)aimee_skills_get_u32(request_body + 8);
   int interval = (int)(int32_t)aimee_skills_get_u32(request_body + 12);
   int fire = interval > 0 && count > 0 && (count % interval) == 0;
   aimee_skills_put_u32(response_body, AIMEE_SKILLS_RESPONSE_MAGIC);
   aimee_skills_put_u32(response_body + 4, (uint32_t)fire);
   *response_len = AIMEE_SKILLS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

static aimee_module_status_t handle_trigger(const uint8_t *request_body, uint32_t request_len,
                                            uint8_t *response_body, uint32_t response_capacity,
                                            uint32_t *response_len)
{
   const uint8_t *content_bytes, *tool_bytes, *subject_bytes;
   uint32_t content_len, tool_len, subject_len;
   if (response_capacity < AIMEE_SKILLS_TRIGGER_RESPONSE_LEN ||
       aimee_skills_trigger_request_decode(request_body, request_len, &content_bytes, &content_len,
                                           &tool_bytes, &tool_len, &subject_bytes,
                                           &subject_len) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   char *content = malloc((size_t)content_len + 1u);
   char *tool_name = malloc((size_t)tool_len + 1u);
   char *subject = malloc((size_t)subject_len + 1u);
   if (!content || !tool_name || !subject)
   {
      free(content);
      free(tool_name);
      free(subject);
      return AIMEE_MODULE_STATUS_INTERNAL;
   }
   memcpy(content, content_bytes, content_len);
   content[content_len] = '\0';
   memcpy(tool_name, tool_bytes, tool_len);
   tool_name[tool_len] = '\0';
   memcpy(subject, subject_bytes, subject_len);
   subject[subject_len] = '\0';

   int match = skill_trigger_policy_matches_content(content, tool_name, subject);
   free(content);
   free(tool_name);
   free(subject);
   if (aimee_skills_trigger_response_encode(match, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_SKILLS_TRIGGER_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (invocation->stage_id == AIMEE_SKILLS_STAGE_CONTEXT)
      return handle_review(request_body, request_len, response_body, response_capacity,
                           response_len);
   if (invocation->stage_id == AIMEE_SKILLS_STAGE_TRIGGER)
      return handle_trigger(request_body, request_len, response_body, response_capacity,
                            response_len);
   return AIMEE_MODULE_STATUS_INVALID_REQUEST;
}
