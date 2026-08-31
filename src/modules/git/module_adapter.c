#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/git/module_api.h>

#include <string.h>

static aimee_git_operation_t classify(const char *op)
{
   static const char *const names[] = {NULL,   "status", "log",      "diff",   "branch", "fetch",
                                       "pull", "push",   "checkout", "commit", "pr"};
   for (uint32_t i = 1; i <= AIMEE_GIT_OP_PR; ++i)
      if (strcmp(op, names[i]) == 0)
         return (aimee_git_operation_t)i;
   return AIMEE_GIT_OP_UNSUPPORTED;
}

static int ref_valid(const char *ref)
{
   if (!ref || !ref[0] || ref[0] == '-' || strstr(ref, ".."))
      return 0;
   for (size_t i = 0; ref[i]; ++i)
   {
      unsigned char c = (unsigned char)ref[i];
      int allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '/' || c == '-';
      if (!allowed)
         return 0;
   }
   return 1;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (invocation->stage_id == AIMEE_GIT_STAGE_OPERATION)
   {
      char op[AIMEE_GIT_OP_MAX + 1];
      if (response_capacity < AIMEE_GIT_RESPONSE_LEN ||
          aimee_git_request_decode(request_body, request_len, op, sizeof(op)) != 0)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      aimee_git_operation_t operation = classify(op);
      int credentials = operation == AIMEE_GIT_OP_FETCH || operation == AIMEE_GIT_OP_PULL ||
                        operation == AIMEE_GIT_OP_PUSH;
      aimee_git_put_u32(response_body, AIMEE_GIT_RESPONSE_MAGIC);
      aimee_git_put_u32(response_body + 4, (uint32_t)operation);
      aimee_git_put_u32(response_body + 8, (uint32_t)credentials);
      *response_len = AIMEE_GIT_RESPONSE_LEN;
      return AIMEE_MODULE_STATUS_OK;
   }
   if (invocation->stage_id == AIMEE_GIT_STAGE_REF_VALIDATE)
   {
      char ref[AIMEE_GIT_REF_MAX + 1];
      if (response_capacity < AIMEE_GIT_REF_RESPONSE_LEN ||
          aimee_git_ref_request_decode(request_body, request_len, ref, sizeof(ref)) != 0)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      aimee_git_put_u32(response_body, AIMEE_GIT_REF_RESPONSE_MAGIC);
      aimee_git_put_u32(response_body + 4, (uint32_t)ref_valid(ref));
      *response_len = AIMEE_GIT_REF_RESPONSE_LEN;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_INVALID_REQUEST;
}
