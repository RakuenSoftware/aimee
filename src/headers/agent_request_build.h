/* Canonical request profiles, admission and provider-boundary rendering. */
#ifndef DEC_AGENT_REQUEST_BUILD_H
#define DEC_AGENT_REQUEST_BUILD_H

#include "aimee.h" /* size macros for agent_types.h */
#include "agent_types.h"
#include <aimee/ir/aimee_ir.h>

#ifdef __cplusplus
extern "C"
{
#endif

   struct cJSON;

   /* HTTP control preflight; native/ACP owners retain their own admission order. */
   int agent_request_controls_supported(const agent_t *agent, const aimee_request_t *request);
   int is_chatgpt_provider(const agent_t *agent);
   int is_anthropic_provider(const agent_t *agent);

   /* Admission is independent of a particular provider's wire projection. */
   int agent_host_request_supported(const aimee_request_t *request, int use_tools);
   /* 1 = owned augmented IR, 0 = no hook context, -1 = refusal. Input is borrowed. */
   int agent_prepare_plain_request_hooks(const aimee_request_t *request, aimee_request_t *out);
   /* Shared owned profile mutation and provider egress; no intermediate contract. */
   int agent_prepare_host_profile(const agent_t *agent, aimee_wire_t wire,
                                  const char *system_prompt, int max_tokens, double temperature,
                                  int plain, aimee_request_t *request);
   struct cJSON *agent_render_request_ir(const agent_t *agent, const aimee_request_t *source,
                                         const aimee_request_t *prepared, aimee_wire_t wire,
                                         int host_profile, int plain);
   /* Host-generated policy is owned; wire conversion runs only at egress after
    * source qualification and emits required projection receipts. */
   int agent_request_append_host_policy(aimee_request_t *request, char *owned_policy);
   char *agent_render_gateway_request_ir(const agent_t *agent, aimee_request_t *request,
                                         aimee_wire_t wire);
   /* Host cache/sampling preparation preserves the input IR on an owned clone. */
   struct cJSON *agent_build_host_request_ir(const agent_t *agent, const aimee_request_t *request);
   /* Already admitted tool-free input retains its blocks and provenance. */
   struct cJSON *agent_build_plain_request_ir(const agent_t *agent, const aimee_request_t *request);

#ifdef __cplusplus
}
#endif
#endif /* DEC_AGENT_REQUEST_BUILD_H */
