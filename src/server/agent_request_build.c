/* agent_request_build.{c,h}: build the provider request body for a plain
 * (system, user) agent turn, via the canonical IR + per-provider backend. Split out
 * of agent_runtime.c so the builder is unit-testable in isolation. The legacy
 * per-provider hand-builders were deleted -- the IR is the single canonical output. */
#include "agent_request_build.h"

#include "agent.h"
#include "agent_protocol.h"        /* agent_request_max_tokens */
#include "agent_request_shaping.h" /* agent_request_shape_user_prompt */
#include "agent_config.h"          /* agent_catalog_provider */
#include "model_registry.h"        /* model_capability_get, MODEL_CAP_* */
#include "model_sampling.h"        /* typed sampling resolution */
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

cJSON *agent_build_plain_request_ir(const agent_t *agent, const aimee_request_t *request)
{
   aimee_request_t owned = {0};
   if (!agent || !request || aimee_ir_request_admit(request) || request->n_tools ||
       request->stream || request->model ||
       (request->has_max_tokens &&
        agent_request_max_tokens(agent, request->max_tokens) != request->max_tokens) ||
       aimee_ir_request_clone(request, &owned))
      return NULL;
   owned.model = strdup(agent->model);
   owned.mutated = 1;
   cJSON *body = NULL;
   if (owned.model)
      body = is_chatgpt_provider(agent)     ? responses_backend_build(&owned)
             : is_anthropic_provider(agent) ? anthropic_backend_build(&owned)
                                            : openai_backend_build(&owned);
   aimee_request_free(&owned);
   return body;
}

/* Build the provider request for a plain (system, user) agent turn. Constructs the
 * typed IR and dispatches to the matching backend (anthropic / responses / openai),
 * with host prompt/cache/sampling preparation completed before rendering.
 * Translation preserves the established plain-turn wire layout. Returns a malloc'd cJSON
 * (caller frees), or NULL on build failure. */
cJSON *agent_build_request(const agent_t *agent, const char *system_prompt, const char *user_prompt,
                           int max_tokens, double temperature)
{
   if (!agent)
      return NULL;
   const int is_resp = is_chatgpt_provider(agent);
   const int is_anth = is_anthropic_provider(agent);

   /* openai /no_think shaping (qwen-local); anthropic/responses do not shape. */
   char *shaped =
       (!is_resp && !is_anth) ? agent_request_shape_user_prompt(agent, user_prompt) : NULL;
   if (!is_resp && !is_anth && !shaped)
      return NULL;
   const char *eff_user = shaped ? shaped : user_prompt;

   aimee_request_t ir = {0};
   ir.model = strdup(agent->model);
   if (!ir.model)
      goto fail;
   int cache_marking = is_anth && config_cache_shaping_enabled();
   if (cache_marking)
   {
      ir.frontend = AIMEE_WIRE_ANTHROPIC;
      if (anthropic_ir_set_host_system(&ir, system_prompt, 1, config_cache_min_chars()))
         goto fail;
   }
   else if (system_prompt && system_prompt[0])
   {
      ir.system = calloc(1, sizeof *ir.system);
      if (!ir.system)
         goto fail;
      ir.n_system = 1;
      ir.system[0].type = AIMEE_BLK_TEXT;
      ir.system[0].text = strdup(system_prompt);
      if (!ir.system[0].text)
         goto fail;
   }
   ir.messages = calloc(1, sizeof *ir.messages);
   if (!ir.messages)
      goto fail;
   ir.n_messages = 1;
   ir.messages[0].role = strdup("user");
   ir.messages[0].blocks = calloc(1, sizeof(aimee_block_t));
   if (!ir.messages[0].role || !ir.messages[0].blocks)
      goto fail;
   ir.messages[0].n_blocks = 1;
   ir.messages[0].blocks[0].type = AIMEE_BLK_TEXT;
   ir.messages[0].blocks[0].text = strdup(eff_user ? eff_user : "");
   if (!ir.messages[0].blocks[0].text)
      goto fail;
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
         ir.thinking = think;
         if (!think || !cJSON_AddStringToObject(think, "type", "adaptive"))
            goto fail;
      }
   }
   if (!is_resp && !thinking_on)
   {
      model_sampling_row_t sampling = model_sampling_resolve(agent, temperature);
      ir.temperature = sampling.temperature;
      ir.has_temperature = sampling.temperature >= 0;
      ir.top_p = sampling.top_p;
      ir.has_top_p = sampling.top_p >= 0;
      ir.top_k = sampling.top_k;
      ir.has_top_k = sampling.top_k >= 0;
      if (!is_anth)
      {
         ir.frontend = AIMEE_WIRE_OPENAI_CHAT;
         aimee_openai_host_profile_t profile = {.min_p = sampling.min_p,
                                                .repeat_penalty = sampling.repeat_penalty};
         if (openai_ir_set_host_profile(&ir, &profile))
            goto fail;
      }
   }
   cJSON *req = is_resp   ? responses_backend_build(&ir)
                : is_anth ? anthropic_backend_build_plain(&ir, cache_marking)
                          : openai_backend_build_plain(&ir);
   aimee_request_free(&ir);
   free(shaped);
   return req;
fail:
   aimee_request_free(&ir);
   free(shaped);
   return NULL;
}
