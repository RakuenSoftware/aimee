/* agent_request_build.h -- see agent_request_build.c. Provider request builders for a
 * plain (system, user) agent turn, split out of agent_runtime.c for isolated testing. */
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
   /* Owned model-egress view; original custody and hidden blocks remain untouched. */
   int agent_request_project_model(const aimee_request_t *request, aimee_request_t *out);
   /* 1 = owned augmented IR, 0 = no hook context, -1 = refusal. Input is borrowed. */
   int agent_prepare_plain_request_hooks(const aimee_request_t *request, aimee_request_t *out);
   /* Host cache/sampling preparation preserves the input IR on an owned clone. */
   struct cJSON *agent_build_host_request_ir(const agent_t *agent, const aimee_request_t *request);
   /* Already admitted tool-free input retains its blocks and provenance. */
   struct cJSON *agent_build_plain_request_ir(const agent_t *agent, const aimee_request_t *request);

#ifdef __cplusplus
}
#endif
#endif /* DEC_AGENT_REQUEST_BUILD_H */
