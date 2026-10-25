/* agent_request_build.{c,h}: build the provider request body for a plain
 * canonical agent turn at the provider boundary. Split out
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
#include <aimee/module-runtime/pre_llm_hook.h>
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

/* Hook callbacks inspect a text view; their context never replaces user IR. */
int agent_prepare_plain_request_hooks(const aimee_request_t *request, aimee_request_t *out)
{
   if (aimee_ir_request_admit(request) || request->n_messages != 1 ||
       strcmp(request->messages[0].role, "user"))
      return -1;
   if (!plugin_chook_pre_llm_count())
      return 0;
   size_t capacity = 1;
   const aimee_message_t *source = &request->messages[0];
   for (int i = 0; i < source->n_blocks; i++)
      if (source->blocks[i].type == AIMEE_BLK_TEXT && source->blocks[i].text)
         capacity += strlen(source->blocks[i].text) + 2;
   char *view = malloc(capacity);
   if (!view)
      return -1;
   aimee_ir_last_user_text(request, view, capacity);
   char context[4096] = "";
   int included = plugin_chook_run_pre_llm(view, context, sizeof context);
   free(view);
   if (included <= 0 || !context[0])
      return 0;
   if (aimee_ir_request_clone(request, out))
      return -1;
   aimee_message_t *message = &out->messages[0];
   aimee_block_t *blocks =
       realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      goto fail;
   message->blocks = blocks;
   aimee_block_t *added = &blocks[message->n_blocks++];
   *added = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(context)};
   if (!added->text)
      goto fail;
   aimee_ir_block_set_context(added, AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   out->mutated = 1;
   if (aimee_ir_request_admit(out))
      goto fail;
   return 1;
fail:
   aimee_request_free(out);
   return -1;
}

/* Apply the host plain-turn profile to an owned IR clone, then render only
 * at this provider boundary. The caller's message and context remain intact. */
cJSON *agent_build_host_request_ir(const agent_t *agent, const aimee_request_t *request)
{
   if (!agent || !agent_request_with_tools_supported(request) || request->model ||
       request->stream || request->n_messages != 1 || strcmp(request->messages[0].role, "user"))
      return NULL;
   for (int i = 0; i < request->messages[0].n_blocks; i++)
      if (request->messages[0].blocks[i].type != AIMEE_BLK_TEXT ||
          request->messages[0].blocks[i].raw || request->messages[0].blocks[i].cache_control ||
          request->messages[0].blocks[i].cache_ttl)
         return NULL;
   aimee_request_t ir = {0};
   if (aimee_ir_request_clone(request, &ir))
      return NULL;
   const int is_resp = is_chatgpt_provider(agent);
   const int is_anth = is_anthropic_provider(agent);
   ir.model = strdup(agent->model);
   ir.mutated = 1;
   if (!ir.model)
      goto fail;
   if (!is_resp && !is_anth)
   {
      char *shaped = agent_request_shape_user_prompt(agent, ir.messages[0].blocks[0].text);
      if (!shaped)
         goto fail;
      free(ir.messages[0].blocks[0].text);
      ir.messages[0].blocks[0].text = shaped;
   }
   int cache_marking = is_anth && config_cache_shaping_enabled();
   if (cache_marking)
   {
      aimee_request_t cache = {.frontend = AIMEE_WIRE_ANTHROPIC};
      const char *system = ir.n_system ? ir.system[0].text : NULL;
      if (anthropic_ir_set_host_system(&cache, system, 1, config_cache_min_chars()))
         goto fail;
      for (int i = 0; i < cache.n_system; i++)
      {
         cache.system[i].context = ir.system[0].context;
         cache.system[i].message_group = ir.system[0].message_group;
      }
      for (int i = 0; i < ir.n_system; i++)
         aimee_block_free_contents(&ir.system[i]);
      free(ir.system);
      ir.system = cache.system;
      ir.n_system = cache.n_system;
      cache.system = NULL;
      cache.n_system = 0;
      if (cache.raw)
      {
         cJSON *native = cJSON_DetachItemFromObjectCaseSensitive(cache.raw, "system");
         if (!ir.raw)
            ir.raw = cJSON_CreateObject();
         cJSON_DeleteItemFromObjectCaseSensitive(ir.raw, "system");
         if (!ir.raw || !native || !cJSON_AddItemToObject(ir.raw, "system", native))
         {
            cJSON_Delete(native);
            aimee_request_free(&cache);
            goto fail;
         }
      }
      aimee_request_free(&cache);
      ir.frontend = AIMEE_WIRE_ANTHROPIC;
   }
   ir.has_temperature = ir.has_top_p = ir.has_top_k = 0;
   ir.has_max_tokens = 0;
   /* responses omits max_tokens by design (codex 400s); openai/anthropic emit the cap. */
   if (!is_resp)
   {
      ir.max_tokens =
          agent_request_max_tokens(agent, request->has_max_tokens ? request->max_tokens : 0);
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
      model_sampling_row_t sampling =
          model_sampling_resolve(agent, request->has_temperature ? request->temperature : -1);
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
   return req;
fail:
   aimee_request_free(&ir);
   return NULL;
}
