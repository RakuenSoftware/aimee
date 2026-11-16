/* Canonical host profiles and provider egress shared by plain, tool and native
 * execution. Callers retain original request custody; profiles mutate owned IR. */
#include "agent_request_build.h"

#include "agent.h"
#include "util.h"
#include "agent_protocol.h"        /* agent_request_max_tokens */
#include "agent_request_shaping.h" /* agent_request_shape_user_prompt */
#include "agent_config.h"          /* agent_catalog_provider */
#include "model_registry.h"        /* model_capability_get, MODEL_CAP_* */
#include "model_sampling.h"        /* typed sampling resolution */
#include "config.h"
#include <aimee/module-runtime/pre_llm_hook.h>
#include <aimee/ir/aimee_ir.h>
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
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

int agent_host_request_supported(const aimee_request_t *request, int use_tools)
{
   return !aimee_ir_request_admit(request) && request->n_messages > 0 && !request->model &&
          !request->stream &&
          (use_tools || (!request->n_tools && (request->tool_choice == AIMEE_TOOL_CHOICE_UNSET ||
                                               request->tool_choice == AIMEE_TOOL_CHOICE_AUTO ||
                                               request->tool_choice == AIMEE_TOOL_CHOICE_NONE)));
}

int agent_request_controls_supported(const agent_t *agent, const aimee_request_t *request)
{
   aimee_request_t owned = {0};
   if (!agent || aimee_ir_request_admit(request))
      return 0;
   /* Native/ACP execution owns its source, budget and control admission. A
    * provider label alone cannot select an HTTP contract for that boundary.
    * Adapters that resolve to HTTP still pass the final seeded render check. */
   if (!strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) ||
       !strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) ||
       !strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI))
      return 1;
   if (aimee_ir_request_project_model(request, &owned))
      return 0;
   aimee_wire_t wire = is_chatgpt_provider(agent)     ? AIMEE_WIRE_RESPONSES
                       : is_anthropic_provider(agent) ? AIMEE_WIRE_ANTHROPIC
                                                      : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *body = wire == AIMEE_WIRE_RESPONSES   ? responses_backend_build(&owned)
                 : wire == AIMEE_WIRE_ANTHROPIC ? anthropic_backend_build(&owned)
                                                : openai_backend_build(&owned);
   int supported = body && !aimee_backend_request_control_losses(request, wire, body);
   cJSON_Delete(body);
   aimee_request_free(&owned);
   return supported;
}

cJSON *agent_build_plain_request_ir(const agent_t *agent, const aimee_request_t *request)
{
   aimee_request_t owned = {0};
   if (!agent || !request || aimee_ir_request_admit(request) || request->n_tools ||
       request->stream || request->model ||
       (request->has_max_tokens &&
        agent_request_max_tokens(agent, request->max_tokens) != request->max_tokens) ||
       aimee_ir_request_project_model(request, &owned))
      return NULL;
   owned.model = strdup(agent->model);
   owned.mutated = 1;
   aimee_wire_t wire = is_chatgpt_provider(agent)     ? AIMEE_WIRE_RESPONSES
                       : is_anthropic_provider(agent) ? AIMEE_WIRE_ANTHROPIC
                                                      : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *body = owned.model ? agent_render_request_ir(agent, request, &owned, wire, 0, 0) : NULL;
   aimee_request_free(&owned);
   return body;
}

/* Hook callbacks inspect a text view; their context never replaces user IR. */
int agent_prepare_plain_request_hooks(const aimee_request_t *request, aimee_request_t *out)
{
   if (aimee_ir_request_admit(request))
      return -1;
   if (!plugin_chook_pre_llm_count())
      return 0;
   aimee_request_t visible = {0};
   if (aimee_ir_request_project_model(request, &visible))
      return -1;
   const aimee_message_t *source = NULL;
   for (int i = visible.n_messages - 1; i >= 0; i--)
      if (!strcmp(visible.messages[i].role, "user"))
      {
         source = &visible.messages[i];
         break;
      }
   if (!source)
   {
      aimee_request_free(&visible);
      return 0;
   }
   size_t capacity = 1;
   for (int i = 0; i < source->n_blocks; i++)
      if (source->blocks[i].type == AIMEE_BLK_TEXT && source->blocks[i].text)
         capacity += strlen(source->blocks[i].text) + 2;
   char *view = malloc(capacity);
   if (!view)
   {
      aimee_request_free(&visible);
      return -1;
   }
   aimee_ir_last_user_text(&visible, view, capacity);
   aimee_request_free(&visible);
   char context[4096] = "";
   int included = plugin_chook_run_pre_llm(view, context, sizeof context);
   free(view);
   if (included <= 0 || !context[0])
      return 0;
   if (aimee_ir_request_clone(request, out))
      return -1;
   if (!out->n_messages || strcmp(out->messages[out->n_messages - 1].role, "user") ||
       out->messages[out->n_messages - 1].n_blocks == AIMEE_IR_MAX_BLOCKS)
   {
      aimee_message_t *messages =
          realloc(out->messages, (size_t)(out->n_messages + 1) * sizeof *messages);
      if (!messages)
         goto fail;
      out->messages = messages;
      aimee_message_t *message = &messages[out->n_messages++];
      *message = (aimee_message_t){.role = strdup("user")};
      if (!message->role)
         goto fail;
   }
   aimee_message_t *message = &out->messages[out->n_messages - 1];
   aimee_block_t *blocks =
       realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      goto fail;
   message->blocks = blocks;
   message->content_scalar = 0;
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

/* One canonical host-profile owner for plain turns and tool/native defaults.
 * Plain profiles preserve their existing cap/thinking and wire-order policy;
 * the seeded caller merges defaults without replacing explicit controls. */
static int agent_is_local_llama_compat(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "llama_native") == 0 || strcmp(agent->provider, "ollama") == 0 ||
       strcmp(agent->provider, "llama-eval") == 0)
      return 1;
   return str_contains_ci(agent->name, "llama") || str_contains_ci(agent->model, "qwen") ||
          str_contains_ci(agent->model, ".gguf");
}
static int agent_is_mistral_vibe_model(const agent_t *agent)
{
   return agent && strcmp(agent->provider, "mistral") == 0 &&
          str_contains_ci(agent->model, "mistral-vibe-cli");
}
static const char *const openrouter_fallbacks[] = {
    "anthropic/claude-opus-4.7", "google/gemini-2.5-flash", "mistralai/mistral-large-2512"};

int agent_prepare_host_profile(const agent_t *agent, aimee_wire_t wire, const char *system_prompt,
                               int max_tokens, double temperature, int plain, aimee_request_t *ir)
{
   const int is_resp = wire == AIMEE_WIRE_RESPONSES;
   const int is_anth = wire == AIMEE_WIRE_ANTHROPIC;
   if (!is_resp && !is_anth && wire != AIMEE_WIRE_OPENAI_CHAT)
      return -1;
   if (!plain)
      ir->frontend = wire;
   free(ir->model);
   ir->model = strdup(agent->model);
   if (!ir->model)
      return -1;
   int count = ir->n_tools;
   if (!is_resp && !is_anth)
   {
      for (int m = ir->n_messages - 1; m >= 0; m--)
      {
         if (strcmp(ir->messages[m].role, "user"))
            continue;
         for (int i = 0; i < ir->messages[m].n_blocks; i++)
         {
            aimee_block_t *block = &ir->messages[m].blocks[i];
            if (block->type != AIMEE_BLK_TEXT)
               continue;
            char *shaped = agent_request_shape_user_prompt(agent, block->text);
            if (!shaped)
               return -1;
            free(block->text);
            block->text = shaped;
            break;
         }
         break;
      }
   }

   if (!plain)
   {
      if (is_anth)
      {
         int cache = config_cache_shaping_enabled();
         if (anthropic_ir_set_host_system(ir, system_prompt, cache,
                                          cache ? config_cache_min_chars() : 0))
            return -1;
      }
      else if (is_resp || (system_prompt && system_prompt[0]))
      {
         ir->system = calloc(1, sizeof *ir->system);
         if (!ir->system)
            return -1;
         ir->n_system = 1;
         ir->system[0].type = AIMEE_BLK_TEXT;
         ir->system[0].text = strdup(
             system_prompt && system_prompt[0] ? system_prompt : "You are an execution agent.");
         if (!ir->system[0].text)
            return -1;
      }
      if (is_resp)
         ir->stream = 1;
   }
   int cache_marking = is_anth && config_cache_shaping_enabled();
   if (plain && cache_marking && ir->n_system && ir->system[0].type == AIMEE_BLK_TEXT &&
       !ir->system[0].cache_control && !ir->system[0].cache_ttl && !ir->system[0].raw)
   {
      aimee_request_t cache = {.frontend = AIMEE_WIRE_ANTHROPIC};
      const char *system = ir->n_system ? ir->system[0].text : NULL;
      if (anthropic_ir_set_host_system(&cache, system, 1, config_cache_min_chars()))
         return -1;
      for (int i = 0; i < cache.n_system; i++)
      {
         cache.system[i].context = ir->system[0].context;
         cache.system[i].message_group = ir->system[0].message_group;
      }
      int original_count = ir->n_system;
      if (original_count > 1)
      {
         aimee_block_t *system =
             realloc(cache.system, (size_t)(cache.n_system + original_count - 1) * sizeof *system);
         if (!system)
         {
            aimee_request_free(&cache);
            return -1;
         }
         cache.system = system;
         for (int i = 1; i < original_count; i++)
         {
            cache.system[cache.n_system++] = ir->system[i];
            memset(&ir->system[i], 0, sizeof ir->system[i]);
         }
      }
      aimee_block_free_contents(&ir->system[0]);
      free(ir->system);
      ir->system = cache.system;
      ir->n_system = cache.n_system;
      cache.system = NULL;
      cache.n_system = 0;
      ir->system_scalar = cache.system_scalar;
      aimee_request_free(&cache);
      ir->frontend = AIMEE_WIRE_ANTHROPIC;
   }
   int has_top_p = ir->has_top_p, has_top_k = ir->has_top_k;
   double top_p = ir->top_p, top_k = ir->top_k;
   ir->has_max_tokens = ir->has_temperature = ir->has_top_p = ir->has_top_k = 0;
   if (!is_resp)
   {
      ir->max_tokens = agent_request_max_tokens(agent, max_tokens);
      ir->has_max_tokens = 1;
   }
   int thinking = plain && is_anth && ir->thinking;
   if (plain && is_anth && !ir->thinking && config_extended_thinking_enabled())
   {
      model_capability_t cap = {0};
      if (model_capability_get(agent_catalog_provider(agent), agent->model, &cap) &&
          (cap.flags & MODEL_CAP_THINKING_ADAPTIVE))
      {
         ir->thinking = cJSON_CreateObject();
         if (!ir->thinking || !cJSON_AddStringToObject(ir->thinking, "type", "adaptive"))
            return -1;
         thinking = 1;
      }
   }
   if (!is_resp && !thinking)
   {
      model_sampling_row_t sampling = model_sampling_resolve(agent, temperature);
      ir->temperature = sampling.temperature;
      ir->has_temperature = sampling.temperature >= 0;
      ir->top_p = plain && has_top_p ? top_p : sampling.top_p;
      ir->has_top_p = (plain && has_top_p) || sampling.top_p >= 0;
      ir->top_k = plain && has_top_k ? top_k : sampling.top_k;
      ir->has_top_k = (plain && has_top_k) || sampling.top_k >= 0;
      if (!is_anth)
      {
         int local = !plain && agent_is_local_llama_compat(agent);
         aimee_openai_host_profile_t profile = {
             .min_p = sampling.min_p,
             .repeat_penalty = sampling.repeat_penalty,
             .disable_thinking = local && !agent_request_prefers_no_think_prompt(agent),
             .split_reasoning = !plain && strstr(agent->model, "MiniMax-M2.7") != NULL};
         if (!plain && !strcmp(agent->provider, "openrouter"))
         {
            profile.fallback_models = openrouter_fallbacks;
            profile.n_fallback_models = sizeof openrouter_fallbacks / sizeof *openrouter_fallbacks;
         }
         ir->frontend = AIMEE_WIRE_OPENAI_CHAT;
         if (openai_ir_set_host_profile(ir, &profile))
            return -1;
      }
   }
   if (!plain && !is_resp && !is_anth && agent_is_mistral_vibe_model(agent))
      ir->reasoning_effort = AIMEE_EFFORT_HIGH;
   if (!plain && count)
   {
      int minimax = !strcmp(agent->provider, "minimax") ||
                    strstr(agent->endpoint, "api.minimax.") || strstr(agent->model, "MiniMax-M2.7");
      if (!is_resp && !is_anth)
      {
         ir->has_parallel_tool_calls = agent_is_local_llama_compat(agent) || minimax;
         ir->parallel_tool_calls = 0;
      }
      if (agent->require_initial_tool_call)
      {
         ir->tool_choice = AIMEE_TOOL_CHOICE_ANY;
         if (is_resp)
         {
            static const char *const preferred[] = {"code_search", "find_symbol", "grep",
                                                    "list_files",  "read_file",   NULL};
            for (int i = 0; preferred[i]; i++)
               if (aimee_ir_find_function(ir, NULL, preferred[i]))
               {
                  ir->tool_choice_name = strdup(preferred[i]);
                  if (!ir->tool_choice_name)
                     return -1;
                  ir->tool_choice = AIMEE_TOOL_CHOICE_TOOL;
                  break;
               }
            if (!ir->tool_choice_name)
               for (int i = 0; i < count; i++)
                  if (ir->tools[i].name && ir->tools[i].name[0])
                  {
                     ir->tool_choice_name = strdup(ir->tools[i].name);
                     if (!ir->tool_choice_name)
                        return -1;
                     ir->tool_choice = AIMEE_TOOL_CHOICE_TOOL;
                     break;
                  }
         }
      }
      else if (!is_resp && !is_anth && (!strcmp(agent->provider, "mistral") || minimax))
         ir->tool_choice = AIMEE_TOOL_CHOICE_AUTO;
   }
   ir->mutated = 1;
   return 0;
}

cJSON *agent_render_request_ir(const agent_t *agent, const aimee_request_t *source,
                               const aimee_request_t *prepared, aimee_wire_t wire, int host_profile,
                               int plain)
{
   cJSON *body =
       wire == AIMEE_WIRE_RESPONSES ? responses_backend_build(prepared)
       : wire == AIMEE_WIRE_ANTHROPIC
           ? (plain ? anthropic_backend_build_plain(prepared, config_cache_shaping_enabled())
                    : anthropic_backend_build(prepared))
           : (plain ? openai_backend_build_plain(prepared) : openai_backend_build(prepared));
   if (!body)
      return NULL;
   aimee_wire_projection_report_t projection;
   aimee_backend_projection_report(prepared, wire, &projection);
   int losses = aimee_backend_request_control_losses(source, wire, body);
   aimee_session_event_t report = {.kind = AIMEE_SESSION_PROJECTION,
                                   .projection_model = agent->name[0] ? agent->name : agent->model,
                                   .projection_boundary = "request",
                                   .projection_request_controls = losses,
                                   .projection_reasoning = projection.omitted_reasoning_blocks,
                                   .projection_opaque = projection.omitted_opaque_state_blocks,
                                   .projection_cache_controls = projection.omitted_cache_controls,
                                   .projection_empty_messages = projection.omitted_empty_messages};
   if ((losses && !host_profile) ||
       ((losses || projection.omitted_reasoning_blocks || projection.omitted_opaque_state_blocks ||
         projection.omitted_cache_controls || projection.omitted_empty_messages) &&
        aimee_ir_session_emit(&report)))
   {
      cJSON_Delete(body);
      return NULL;
   }
   return body;
}

cJSON *agent_build_host_request_ir(const agent_t *agent, const aimee_request_t *request)
{
   if (!agent || !agent_host_request_supported(request, 0))
      return NULL;
   aimee_request_t ir = {0};
   if (aimee_ir_request_project_model(request, &ir))
      return NULL;
   aimee_wire_t wire = is_chatgpt_provider(agent)     ? AIMEE_WIRE_RESPONSES
                       : is_anthropic_provider(agent) ? AIMEE_WIRE_ANTHROPIC
                                                      : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *body = NULL;
   if (ir.n_messages && !agent_prepare_host_profile(
                            agent, wire, NULL, request->has_max_tokens ? request->max_tokens : 0,
                            request->has_temperature ? request->temperature : -1, 1, &ir))
      body = agent_render_request_ir(agent, request, &ir, wire, 1, 1);
   aimee_request_free(&ir);
   return body;
}

int agent_request_append_host_policy(aimee_request_t *request, char *policy)
{
   if (!policy)
      return 0;
   if (request->n_system >= AIMEE_IR_MAX_BLOCKS)
   {
      free(policy);
      return -1;
   }
   aimee_block_t *blocks =
       realloc(request->system, (size_t)(request->n_system + 1) * sizeof *blocks);
   if (!blocks)
   {
      free(policy);
      return -1;
   }
   request->system = blocks;
   aimee_block_t *block = &blocks[request->n_system++];
   *block = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = policy};
   aimee_ir_block_set_context(block, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
   request->mutated = 1;
   return 0;
}

char *agent_render_gateway_request_ir(const agent_t *agent, aimee_request_t *request,
                                      aimee_wire_t wire)
{
   if (aimee_frontend_guard_state(request, NULL, 0) || aimee_ir_request_admit(request))
      return NULL;
   int controls = 0;
   char *body = aimee_backend_render_wire_profile(
       request, wire,
       agent && !strcmp(agent->provider, "chatgpt") ? AIMEE_RESPONSES_CODEX : AIMEE_RESPONSES_API,
       &controls);
   aimee_session_event_t event = {.kind = AIMEE_SESSION_PROJECTION,
                                  .projection_model = request->model,
                                  .projection_boundary = "request",
                                  .projection_request_controls = controls};
   if (body && controls && aimee_ir_session_emit(&event))
   {
      free(body);
      return NULL;
   }
   aimee_wire_projection_report_t projection;
   aimee_backend_projection_report(request, wire, &projection);
   if (body && aimee_ir_session_emit_projection_full(
                   request->model, projection.omitted_reasoning_blocks,
                   projection.omitted_opaque_state_blocks, projection.omitted_cache_controls,
                   projection.omitted_empty_messages, "request"))
   {
      free(body);
      return NULL;
   }
   /* A Chat provider's explicitly counted template extension stays at its
    * native boundary. It is never turned into model content or another wire. */
   const cJSON *kwargs =
       request->frontend == AIMEE_WIRE_OPENAI_CHAT && wire == AIMEE_WIRE_OPENAI_CHAT
           ? cJSON_GetObjectItemCaseSensitive(request->raw, "chat_template_kwargs")
           : NULL;
   if (cJSON_IsObject(kwargs))
   {
      cJSON *native = cJSON_Parse(body);
      free(body);
      cJSON_DeleteItemFromObjectCaseSensitive(native, "chat_template_kwargs");
      if (!cJSON_AddItemToObject(native, "chat_template_kwargs", cJSON_Duplicate(kwargs, 1)))
      {
         cJSON_Delete(native);
         return NULL;
      }
      body = cJSON_PrintUnformatted(native);
      cJSON_Delete(native);
   }
   return body;
}
