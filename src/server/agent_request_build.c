/* agent_request_build.{c,h}: build the provider request body for a plain
 * (system, user) agent turn, via the canonical IR + per-provider backend. Split out
 * of agent_runtime.c so the builder is unit-testable in isolation. The legacy
 * per-provider hand-builders were deleted -- the IR is the single canonical output. */
#include "agent_request_build.h"

#include "agent.h"
#include "agent_protocol.h"        /* agent_anthropic_set_system, agent_request_max_tokens */
#include "agent_request_shaping.h" /* agent_request_shape_user_prompt */
#include "model_sampling.h"        /* model_sampling_apply_{openai,anthropic} */
#include "config.h"
#include "aimee_ir.h"
#include "aimee_backend.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

int is_chatgpt_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "chatgpt") == 0;
}

int is_anthropic_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "anthropic") == 0;
}

/* Build the provider request for a plain (system, user) agent turn. Constructs the
 * typed IR and dispatches to the matching backend (anthropic / responses / openai),
 * then applies the request-shaping the IR model does not itself carry: openai
 * /no_think user-prompt shaping, anthropic cache_control system marking, and
 * model_sampling (temperature + curated top_p/top_k/...). Returns a malloc'd cJSON
 * (caller frees), or NULL on build failure. */
cJSON *agent_build_request(const agent_t *agent, const char *system_prompt, const char *user_prompt,
                           int max_tokens, double temperature)
{
   const int is_resp = is_chatgpt_provider(agent);
   const int is_anth = is_anthropic_provider(agent);

   /* openai /no_think shaping (qwen-local); anthropic/responses do not shape. */
   char *shaped =
       (!is_resp && !is_anth) ? agent_request_shape_user_prompt(agent, user_prompt) : NULL;
   const char *eff_user = shaped ? shaped : user_prompt;

   aimee_request_t ir;
   memset(&ir, 0, sizeof ir);
   ir.model = strdup(agent->model);
   if (system_prompt && system_prompt[0])
   {
      ir.system = calloc(1, sizeof *ir.system);
      ir.n_system = 1;
      ir.system[0].type = AIMEE_BLK_TEXT;
      ir.system[0].text = strdup(system_prompt);
   }
   ir.messages = calloc(1, sizeof *ir.messages);
   ir.n_messages = 1;
   ir.messages[0].role = strdup("user");
   ir.messages[0].blocks = calloc(1, sizeof(aimee_block_t));
   ir.messages[0].n_blocks = 1;
   ir.messages[0].blocks[0].type = AIMEE_BLK_TEXT;
   ir.messages[0].blocks[0].text = strdup(eff_user ? eff_user : "");
   /* responses omits max_tokens by design (codex 400s); openai/anthropic emit the cap. */
   if (!is_resp)
   {
      ir.max_tokens = agent_request_max_tokens(agent, max_tokens);
      ir.has_max_tokens = 1;
   }
   /* temperature is layered post-build by model_sampling below (so the curated
    * top_p/top_k/... the IR model does not carry are applied alongside it). */

   cJSON *req = is_resp   ? responses_backend_build(&ir)
                : is_anth ? anthropic_backend_build(&ir)
                          : openai_backend_build(&ir);
   aimee_request_free(&ir);
   free(shaped);
   if (!req)
      return NULL;

   if (is_anth)
   {
      /* cache_control system marking (default-off): split the system at the
       * <aimee-context> volatile boundary and mark the stable prefix cacheable. The
       * IR backend emitted a plain system string; re-apply the marking on top. */
      config_t cs;
      if (config_load(&cs) == 0 && cs.cache_shaping_enabled)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(req, "system");
         agent_anthropic_set_system(req, system_prompt, 1, cs.cache_min_chars);
      }
      model_sampling_apply_anthropic(agent, req, temperature);
   }
   else if (!is_resp)
   {
      model_sampling_apply_openai(agent, req, temperature);
   }
   return req;
}
