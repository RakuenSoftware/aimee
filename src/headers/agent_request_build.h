/* agent_request_build.h -- see agent_request_build.c. Provider request builders for a
 * plain (system, user) agent turn, split out of agent_runtime.c for isolated testing. */
#ifndef DEC_AGENT_REQUEST_BUILD_H
#define DEC_AGENT_REQUEST_BUILD_H

#include "aimee.h" /* size macros for agent_types.h */
#include "agent_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

   struct cJSON;

   int is_chatgpt_provider(const agent_t *agent);
   int is_anthropic_provider(const agent_t *agent);

   /* Build the provider request for a plain (system, user) agent turn via the
    * canonical IR + per-provider backend. Returns a malloc'd cJSON (caller frees) or
    * NULL on build failure. This is the single builder -- the legacy per-provider
    * hand-builders were deleted. */
   struct cJSON *agent_build_request(const agent_t *agent, const char *system_prompt,
                                     const char *user_prompt, int max_tokens, double temperature);

#ifdef __cplusplus
}
#endif
#endif /* DEC_AGENT_REQUEST_BUILD_H */
