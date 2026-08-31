/* webuser_name_validator — install webuser_runtime's name validator for tests.
 *
 * webuser_runtime fails closed until core registers a validator, because the
 * rule for what may name a directory belongs to `workspace`, not to webuser. A
 * test that exercises the runtime dir therefore has to install one, and the
 * honest thing to install is workspace's own answer over its wire contract --
 * the same decision the server registers -- rather than a stub that could agree
 * with nothing real.
 *
 * The separator case is deliberately left to webuser: workspace admits
 * `owner/project`, a runtime dir name is one component, and webuser rejects the
 * slash before delegating.
 */
#include "webuser_runtime.h"

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/workspace/module_api.h>

#include <stddef.h>
#include <stdint.h>

extern aimee_module_status_t aimee_workspace_module_handler(const aimee_module_invocation_t *,
                                                            const uint8_t *, uint32_t, uint8_t *,
                                                            uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int validate_name_via_module(const char *name, size_t len, int *allowed)
{
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKSPACE_STAGE_ACCESS};
   return aimee_workspace_request_encode(name, len, request, sizeof(request)) == 0 &&
                  aimee_workspace_module_handler(&invocation, request, sizeof(request), response,
                                                 sizeof(response), &response_len,
                                                 NULL) == AIMEE_MODULE_STATUS_OK
              ? aimee_workspace_response_decode(response, response_len, allowed)
              : -1;
}

void webuser_test_install_name_validator(void)
{
   webuser_runtime_register_name_validator(validate_name_via_module);
}
