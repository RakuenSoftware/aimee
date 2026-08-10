/* agent_request_build.{c,h}: build the provider request body for a plain
 * (system, user) agent turn, via the canonical IR + per-provider backend. Split out
 * of agent_runtime.c so the builder is unit-testable in isolation. The legacy
 * per-provider hand-builders were deleted -- the IR is the single canonical output. */
#include "agent_request_build.h"

#include "agent.h"
#include "agent_protocol.h"        /* agent_anthropic_set_system, agent_request_max_tokens */
#include "agent_request_shaping.h" /* agent_request_shape_user_prompt */
#include "agent_config.h"          /* agent_catalog_provider */
#include "model_registry.h"        /* model_capability_get, MODEL_CAP_* */
#include "model_sampling.h"        /* model_sampling_apply_{openai,anthropic} */
#include "config.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/translation/aimee_backend.h>
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

   /* Extended thinking on aimee's own Anthropic turns (default-off). Without this
    * the IR carried no `thinking` config on an aimee-originated turn -- ir->thinking
    * is otherwise populated only by an inbound client request -- so aimee never
    * asked a reasoning-capable model to reason.
    *
    * GATED ON THE MODEL SAYING IT ACCEPTS THIS SHAPE, not on the wire format. Two
    * separate faults are fixed by that single condition:
    *
    *  - The shape. This used to emit {"type":"enabled", budget_tokens: N}, which
    *    Anthropic removed: it is a 400 on Opus 4.7/4.8/5, Sonnet 5 and Fable 5, and
    *    survives only on the 4.6 generation. It now emits {"type":"adaptive"}, and
    *    only for a model whose provider positively reports adaptive support.
    *
    *  - The predicate. `is_anth` is the WIRE FORMAT, not the vendor. A third-party
    *    model behind an Anthropic-compatible endpoint (MiniMax and Moonshot both
    *    ship one) satisfied it, so enabling this knob sent Anthropic thinking
    *    config to a vendor that never advertised it -- while the Claude seat, a
    *    provider-CLI agent with no HTTP endpoint, never reached this path at all.
    *    A capability check excludes the former without needing to name it.
    *
    * FAILS CLOSED. MODEL_CAP_THINKING_ADAPTIVE is set only by a provider that
    * publishes the capability; models.dev carries a bare `reasoning` boolean and
    * cannot distinguish the two shapes, so a catalogued-only model leaves it clear
    * and gets no thinking config. Sending nothing costs a missed opportunity to
    * reason; sending the wrong shape costs a 400 the operator sees as an agent
    * failure. */
   int thinking_on = 0;
   if (is_anth && config_extended_thinking_enabled())
   {
      model_capability_t cap;
      memset(&cap, 0, sizeof cap);
      if (model_capability_get(agent_catalog_provider(agent), agent->model, &cap) &&
          (cap.flags & MODEL_CAP_THINKING_ADAPTIVE))
      {
         thinking_on = 1;
         cJSON *think = cJSON_CreateObject();
         cJSON_AddStringToObject(think, "type", "adaptive");
         ir.thinking = think;
      }
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
      if (config_cache_shaping_enabled())
      {
         cJSON_DeleteItemFromObjectCaseSensitive(req, "system");
         agent_anthropic_set_system(req, system_prompt, 1, config_cache_min_chars());
      }
      model_sampling_apply_anthropic(agent, req, temperature);
      /* Strip sampling entirely alongside thinking, rather than pinning
       * temperature to 1 as this used to.
       *
       * Every model that reaches this branch is adaptive-capable, i.e. 4.6 or
       * later, and across that range removal is the only universally valid
       * choice: on 4.7 and later ANY of temperature / top_p / top_k is a 400,
       * while on 4.6 they are merely permitted. Adding temperature=1 was correct
       * for 4.6 and is itself a rejection on everything newer -- the old code
       * turned one 400 into two.
       *
       * Done after model_sampling has layered the curated values on, because
       * that is where they arrive; there is nothing to talk out of it earlier. */
      if (thinking_on)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(req, "temperature");
         cJSON_DeleteItemFromObjectCaseSensitive(req, "top_p");
         cJSON_DeleteItemFromObjectCaseSensitive(req, "top_k");
      }
   }
   else if (!is_resp)
   {
      model_sampling_apply_openai(agent, req, temperature);
   }
   return req;
}
