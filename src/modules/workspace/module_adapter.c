#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/workspace/module_api.h>

#include <string.h>

static int name_valid(const uint8_t *name, size_t len)
{
   if (!name || len == 0 || len > 64 || (len == 1 && name[0] == '.') ||
       (len == 2 && name[0] == '.' && name[1] == '.'))
      return 0;
   unsigned char first = name[0];
   if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
         (first >= '0' && first <= '9')))
      return 0;
   for (size_t i = 0; i < len; ++i)
   {
      unsigned char c = name[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

static int ref_valid(const uint8_t *ref, size_t len)
{
   size_t slash = len;
   if (!ref || len == 0 || len > AIMEE_WORKSPACE_REF_MAX)
      return 0;
   for (size_t i = 0; i < len; ++i)
   {
      if (ref[i] == '\0')
         return 0;
      if (ref[i] == '/')
      {
         if (slash != len)
            return 0;
         slash = i;
      }
   }
   return slash == len ? name_valid(ref, len)
                       : name_valid(ref, slash) && name_valid(ref + slash + 1, len - slash - 1);
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_WORKSPACE_STAGE_ACCESS ||
       !request_body || request_len != AIMEE_WORKSPACE_REQUEST_LEN ||
       response_capacity < AIMEE_WORKSPACE_RESPONSE_LEN ||
       aimee_workspace_get_u32(request_body) != AIMEE_WORKSPACE_REQUEST_MAGIC ||
       request_body[4] != AIMEE_WORKSPACE_WIRE_VERSION || request_body[5] != 0 ||
       aimee_workspace_get_u16(request_body + 6) == 0 ||
       aimee_workspace_get_u16(request_body + 6) > AIMEE_WORKSPACE_REF_MAX)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   aimee_workspace_put_u32(response_body, AIMEE_WORKSPACE_RESPONSE_MAGIC);
   aimee_workspace_put_u32(
       response_body + 4,
       ref_valid(request_body + 8, aimee_workspace_get_u16(request_body + 6)));
   *response_len = AIMEE_WORKSPACE_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
