/* Apply delegate policy to the canonical catalog before native rendering. */
#include <aimee/gateway/gateway_delegate.h>

int gateway_delegate_run_request_pipeline(aimee_request_t *request, int is_delegate)
{
   if (!request)
      return -1;
   return is_delegate ? gateway_policy_apply_ir(request) : 0;
}
