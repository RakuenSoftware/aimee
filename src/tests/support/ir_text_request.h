/* Text fixture origins only. Production execution accepts the resulting IR. */
#ifndef AIMEE_TEST_IR_TEXT_REQUEST_H
#define AIMEE_TEST_IR_TEXT_REQUEST_H
#include "agent_exec.h"
#include "ir_response_fixture.h"
#include <assert.h>

static inline int fixture_agent_result_from_text(agent_result_t *result, const char *text)
{
   aimee_response_t response = {0};
   if (!result || fixture_ir_response_from_text(&response, text, NULL))
      return -1;
   aimee_ir_block_set_context(&response.content[0], AIMEE_CTX_ORIGIN_PLATFORM,
                              AIMEE_CTX_AUTH_DIAGNOSTIC, AIMEE_CTX_TRUST_UNVERIFIED,
                              AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
   int status = agent_result_adopt_response(result, &response);
   aimee_response_free(&response);
   return status;
}

static inline int fixture_agent_execute_text(const agent_t *agent, const char *system,
                                             const char *user, int max, double temp,
                                             agent_result_t *out)
{
   aimee_request_t request = {0};
   assert(!aimee_ir_request_from_text(&request, system, user, max, temp));
   int rc = agent_dispatch_one(agent, NULL, NULL, &request, 0, out);
   aimee_request_free(&request);
   return rc;
}
static inline int fixture_agent_role_text(const agent_t *agent, const agent_network_t *network,
                                          const char *role, const char *system, const char *user,
                                          int max, double temp, agent_result_t *out)
{
   aimee_request_t request = {0};
   assert(!aimee_ir_request_from_text(&request, system, user, max, temp));
   int rc = agent_execute_with_tools_for_role(agent, network, role, &request, out);
   aimee_request_free(&request);
   return rc;
}
static inline int fixture_agent_tools_text(const agent_t *agent, const agent_network_t *network,
                                           const char *system, const char *user, int max,
                                           double temp, agent_result_t *out)
{
   return fixture_agent_role_text(agent, network, NULL, system, user, max, temp, out);
}
static inline int fixture_agent_dispatch_text(const agent_t *agent, const agent_network_t *network,
                                              const char *role, const char *system,
                                              const char *user, int max, double temp, int tools,
                                              agent_result_t *out)
{
   aimee_request_t request = {0};
   assert(!aimee_ir_request_from_text(&request, system, user, max, temp));
   int rc = agent_dispatch_one(agent, network, role, &request, tools, out);
   aimee_request_free(&request);
   return rc;
}
#endif
