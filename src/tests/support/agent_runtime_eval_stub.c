/* agent_runtime_eval_stub.c: inert agent-runtime stubs for the standalone
 * memory-negation eval driver (aimee-negation-eval). agent_eval_memory_support.c
 * references agent_execute_with_tools / agent_run only on its LLM-judge answer
 * paths (locomo / longmemeval), which the corpus-file retrieval eval never takes.
 * Linking the full agent runtime would drag the provider/HTTP/tool graph into a
 * DB-only eval binary, so resolve these with weak inert stubs; the real objects
 * win if ever linked. Never actually called on the corpus path. */
#include "aimee.h"
#include "agent_exec.h"

__attribute__((weak)) int agent_execute_with_tools(const agent_t *agent,
                                                   const agent_network_t *network,
                                                   const char *system_prompt,
                                                   const char *user_prompt, int max_tokens,
                                                   double temperature, agent_result_t *out)
{
   (void)agent;
   (void)network;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   (void)out;
   return -1;
}

__attribute__((weak)) int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt,
                                    const char *user_prompt, int max_tokens, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)out;
   return -1;
}
