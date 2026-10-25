/* agent_bridge.c: Provider layer — LLM request/response protocol, HTTP client, tunnel lifecycle */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "util.h"
#include "agent_request_shaping.h"
#include "agent_protocol.h"
#include "agent_request_build.h"
#include <aimee/gateway/gateway_delegate.h>
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "config.h"
#include "log.h"
#include "model_sampling.h"
#include "model_registry.h"
#include "agent_config.h" /* agent_catalog_provider */
#include "cJSON.h"
#include <string.h>
/* From agent_http.c */
#include "agent_exec.h"
#include <time.h>
/* From agent_tunnel.c */
#include "agent_tunnel.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* ================================================================
 * From: agent_protocol.c
 * ================================================================ */

/* --- Request builders --- */
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

/* Reject the gross misconfiguration where the OUTPUT limit alone would consume
 * the entire context window, leaving no room for any prompt at all.
 *
 * Scope, precisely: this function does NOT know the request's prompt size, so it
 * cannot and does not guarantee `prompt + output <= window`. It only bounds the
 * case that is invalid regardless of prompt — an output cap at or above the
 * whole window. Half the window is a policy choice for that case, not a derived
 * bound. agent_exec_context_budget_chars() clamps its own PROMPT arithmetic, but
 * that is a local calculation; without this the REQUEST still carried the
 * oversized limit, so a 200k-window agent pinned at 300k asked for 300k output.
 *
 * The window is the agent's effective ceiling: the operator's policy value when
 * set, else the model catalog. Consulting only middleware.context_window left a
 * catalogued 8k model with no override accepting a pinned 300k. */
static int agent_clamp_to_context(const agent_t *agent, int tokens)
{
   if (!agent || tokens <= 0)
      return tokens;
   int window = agent->middleware.context_window;
   if (window <= 0)
   {
      model_capability_t cap;
      if (model_capability_get(agent_catalog_provider(agent), agent->model, &cap))
         window = cap.context_window;
   }
   if (window > 1 && tokens >= window)
      return window / 2;
   return tokens;
}

int agent_request_max_tokens(const agent_t *agent, int requested)
{
   if (requested > 0)
      return agent_clamp_to_context(agent, requested); /* caller pinned a budget */
   if (agent && agent->max_tokens > 0)
      return agent_clamp_to_context(agent, agent->max_tokens); /* agents.json cap */
   /* No explicit cap: use the model's own output ceiling, never a hardcoded one. */
   /* Catalog identity, not the wire provider: a third-party vendor on another
    * vendor's API otherwise resolves the wrong capability row and gets the
    * non-reasoning 8192 output ceiling instead of its real one. */
   return agent_clamp_to_context(
       agent,
       model_max_output(agent ? agent_catalog_provider(agent) : NULL, agent ? agent->model : NULL));
}

/* Shape only the owned request copy, before policy and translation. Media and
 * tool-result-only messages retain the compatibility builder's behavior. */
static int agent_shape_ir_user(const agent_t *agent, aimee_request_t *ir)
{
   if (!agent_request_prefers_no_think_prompt(agent))
      return 0;
   for (int i = ir->n_messages - 1; i >= 0; i--)
   {
      aimee_message_t *message = &ir->messages[i];
      if (!message->role || strcmp(message->role, "user"))
         continue;
      int first_text = -1, other = 0, has_tool_use = 0, has_text = 0;
      for (int j = 0; j < message->n_blocks; j++)
      {
         aimee_block_t *block = &message->blocks[j];
         if (!aimee_ir_block_model_visible(block))
            continue;
         if (block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT)
            return 0;
         if (block->type != AIMEE_BLK_TOOL_RESULT)
            other++;
         has_tool_use |= block->type == AIMEE_BLK_TOOL_USE;
         has_text |= block->type == AIMEE_BLK_TEXT && block->text && block->text[0];
         if (block->type == AIMEE_BLK_TEXT && first_text < 0)
            first_text = j;
      }
      if (!other && message->n_blocks)
         continue;
      if (has_tool_use && !has_text)
         return 0; /* The native renderer uses null content for a bare tool call. */
      if (first_text < 0)
      {
         aimee_block_t *blocks =
             realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
         if (!blocks)
            return -1;
         message->blocks = blocks;
         first_text = message->n_blocks++;
         message->blocks[first_text] = (aimee_block_t){.type = AIMEE_BLK_TEXT};
      }
      size_t length = 0;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].type == AIMEE_BLK_TEXT && message->blocks[j].text &&
             aimee_ir_block_model_visible(&message->blocks[j]))
            length += strlen(message->blocks[j].text);
      char *joined = calloc(length + 1, 1);
      if (!joined)
         return -1;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].type == AIMEE_BLK_TEXT && message->blocks[j].text &&
             aimee_ir_block_model_visible(&message->blocks[j]))
            strcat(joined, message->blocks[j].text);
      char *shaped = agent_request_shape_user_prompt(agent, joined);
      int changed = shaped && strcmp(joined, shaped);
      free(joined);
      if (!shaped)
         return -1;
      free(shaped);
      if (changed)
      {
         aimee_block_t *block = &message->blocks[first_text];
         char *text = agent_request_shape_user_prompt(agent, block->text);
         if (!text)
            return -1;
         free(block->text);
         block->text = text;
      }
      return 0;
   }
   return 0;
}

static int agent_openai_profile_ir(const agent_t *agent, const char *system_prompt, int max_tokens,
                                   double temperature, aimee_request_t *ir)
{
   ir->frontend = AIMEE_WIRE_OPENAI_CHAT;
   ir->model = strdup(agent->model);
   if (!ir->model)
      return -1;
   ir->max_tokens = agent_request_max_tokens(agent, max_tokens);
   ir->has_max_tokens = 1;
   model_sampling_row_t sampling = model_sampling_resolve(agent, temperature);
   ir->temperature = sampling.temperature;
   ir->has_temperature = sampling.temperature >= 0;
   ir->top_p = sampling.top_p;
   ir->has_top_p = sampling.top_p >= 0;
   ir->top_k = sampling.top_k;
   ir->has_top_k = sampling.top_k >= 0;
   if (agent_is_mistral_vibe_model(agent))
      ir->reasoning_effort = AIMEE_EFFORT_HIGH;
   if (system_prompt && system_prompt[0])
   {
      ir->system = calloc(1, sizeof *ir->system);
      if (!ir->system)
         return -1;
      ir->n_system = 1;
      ir->system[0].type = AIMEE_BLK_TEXT;
      ir->system[0].text = strdup(system_prompt);
      if (!ir->system[0].text)
         return -1;
   }
   int count = ir->n_tools;
   if (count)
   {
      int minimax = !strcmp(agent->provider, "minimax") ||
                    strstr(agent->endpoint, "api.minimax.") || strstr(agent->model, "MiniMax-M2.7");
      ir->has_parallel_tool_calls = agent_is_local_llama_compat(agent) || minimax;
      ir->parallel_tool_calls = 0;
      if (agent->require_initial_tool_call)
         ir->tool_choice = AIMEE_TOOL_CHOICE_ANY;
      else if (!strcmp(agent->provider, "mistral") || minimax)
         ir->tool_choice = AIMEE_TOOL_CHOICE_AUTO;
   }
   aimee_openai_host_profile_t profile = {
       .min_p = sampling.min_p,
       .repeat_penalty = sampling.repeat_penalty,
       .disable_thinking =
           agent_is_local_llama_compat(agent) && !agent_request_prefers_no_think_prompt(agent),
       .split_reasoning = strstr(agent->model, "MiniMax-M2.7") != NULL};
   if (!strcmp(agent->provider, "openrouter"))
   {
      profile.fallback_models = openrouter_fallbacks;
      profile.n_fallback_models = sizeof openrouter_fallbacks / sizeof *openrouter_fallbacks;
   }
   return openai_ir_set_host_profile(ir, &profile);
}

/* Host profile controls and the registry-owned catalogue remain canonical
 * until the explicit provider egress render. */
static int agent_responses_profile_ir(const agent_t *agent, const char *system_prompt,
                                      aimee_request_t *ir)
{
   ir->frontend = AIMEE_WIRE_RESPONSES;
   ir->stream = 1;
   ir->model = strdup(agent->model);
   ir->system = calloc(1, sizeof *ir->system);
   if (!ir->model || !ir->system)
      return -1;
   ir->n_system = 1;
   ir->system[0].type = AIMEE_BLK_TEXT;
   ir->system[0].text =
       strdup(system_prompt && system_prompt[0] ? system_prompt : "You are an execution agent.");
   if (!ir->system[0].text)
      return -1;
   int count = ir->n_tools;
   if (!agent->require_initial_tool_call || !count)
      return 0;
   static const char *const preferred[] = {"code_search", "find_symbol", "grep",
                                           "list_files",  "read_file",   NULL};
   const char *selected = NULL;
   for (int p = 0; preferred[p] && !selected; p++)
   {
      const aimee_tool_t *tool = aimee_ir_find_function(ir, NULL, preferred[p]);
      if (tool)
         selected = tool->name;
   }
   for (int i = 0; i < ir->n_tools && !selected; i++)
      if (ir->tools[i].name && ir->tools[i].name[0])
         selected = ir->tools[i].name;
   if (selected)
   {
      ir->tool_choice = AIMEE_TOOL_CHOICE_TOOL;
      ir->tool_choice_name = strdup(selected);
      if (!ir->tool_choice_name)
         return -1;
   }
   return 0;
}

static int agent_anthropic_profile_ir(const agent_t *agent, const char *system_prompt,
                                      int max_tokens, double temperature, aimee_request_t *ir)
{
   ir->frontend = AIMEE_WIRE_ANTHROPIC;
   ir->model = strdup(agent->model);
   if (!ir->model)
      return -1;
   ir->max_tokens = agent_request_max_tokens(agent, max_tokens);
   ir->has_max_tokens = 1;
   model_sampling_row_t sampling = model_sampling_resolve(agent, temperature);
   ir->temperature = sampling.temperature;
   ir->has_temperature = sampling.temperature >= 0;
   ir->top_p = sampling.top_p;
   ir->has_top_p = sampling.top_p >= 0;
   ir->top_k = sampling.top_k;
   ir->has_top_k = sampling.top_k >= 0;
   int cache = config_cache_shaping_enabled();
   if (anthropic_ir_set_host_system(ir, system_prompt, cache, cache ? config_cache_min_chars() : 0))
      return -1;
   int count = ir->n_tools;
   if (count && agent->require_initial_tool_call)
      ir->tool_choice = AIMEE_TOOL_CHOICE_ANY;
   return 0;
}

/* Host profiles start in IR; compatibility catalogs enter their typed parser.
 * Policy and translation own the complete output and dispatch catalog. */
/* Execution entry controls belong with canonical request profiles. The loop's
 * economizer must not own provider cache configuration. */
int agent_request_with_tools_supported(const aimee_request_t *request)
{
   if (aimee_ir_request_admit(request) || request->n_messages < 1 || request->stream)
      return 0;
   /* Structural admission is independent of later host catalog/grant checks.
    * Retain valid controls and content rather than imposing text adapter shape. */
   for (int i = 0; i < request->n_messages; ++i)
      if (request->messages[i].n_blocks)
         return 1;
   return 0;
}

static int prepare_host_profile(const agent_t *agent, const aimee_request_t *catalog,
                                const char *system_prompt, int max_tokens, double temperature,
                                aimee_wire_t wire, int is_delegate, aimee_request_t *out)
{
   if (!agent || !out)
      return -1;
   aimee_request_t ir = {0};
   aimee_request_t tool_view = {.tools = catalog ? catalog->tools : NULL,
                                .n_tools = catalog ? catalog->n_tools : 0};
   if (aimee_ir_request_admit(&tool_view) || aimee_ir_request_clone(&tool_view, &ir))
      return -1;
   if (wire == AIMEE_WIRE_RESPONSES)
   {
      if (agent_responses_profile_ir(agent, system_prompt, &ir))
         goto fail;
   }
   else if (wire == AIMEE_WIRE_OPENAI_CHAT)
   {
      if (agent_openai_profile_ir(agent, system_prompt, max_tokens, temperature, &ir))
         goto fail;
   }
   else if (wire == AIMEE_WIRE_ANTHROPIC)
   {
      if (agent_anthropic_profile_ir(agent, system_prompt, max_tokens, temperature, &ir))
         goto fail;
   }
   else
      goto fail;
   /* Policy may remove catalog entries; it must not sanitize an unadmitted
    * host request into admissible transport by hiding an invalid input. */
   if (aimee_ir_request_admit(&ir))
      goto fail;
   ir.mutated = 1;
   if (aimee_ir_request_admit(&ir))
      goto fail;
   aimee_request_free(out);
   *out = ir;
   return 0;
fail:
   aimee_request_free(&ir);
   return -1;
}

int agent_prepare_seeded_request_ir(const agent_t *agent, const aimee_request_t *seed,
                                    const cJSON *messages, const aimee_request_t *catalog,
                                    const aimee_message_t *host_context, int max_tokens,
                                    double temperature, aimee_wire_t wire, int is_delegate,
                                    aimee_request_t *out)
{
   if (seed && !agent_request_with_tools_supported(seed))
      return -1;
   if (host_context && (!host_context->role || strcmp(host_context->role, "system")))
      return -1;
   aimee_request_t profile = {0}, ir = {0}, context_copy = {0};
   int simple_policy = host_context && host_context->n_blocks == 1 &&
                       host_context->blocks[0].type == AIMEE_BLK_TEXT &&
                       host_context->blocks[0].context.authority == AIMEE_CTX_AUTH_POLICY &&
                       !host_context->blocks[0].raw && !host_context->blocks[0].cache_control &&
                       !host_context->blocks[0].cache_ttl;
   int rc =
       prepare_host_profile(agent, catalog, simple_policy ? host_context->blocks[0].text : NULL,
                            max_tokens, temperature, wire, is_delegate, &profile) ||
       (seed && aimee_ir_request_clone(seed, &ir));
   if (rc)
      goto fail;
   if (ir.frontend == AIMEE_WIRE_UNKNOWN)
      ir.frontend = profile.frontend;
   if (!ir.raw && ir.frontend == profile.frontend)
   {
      ir.raw = profile.raw;
      profile.raw = NULL;
   }
   free(ir.model);
   ir.model = profile.model;
   profile.model = NULL;
   for (int i = 0; i < ir.n_messages; ++i)
      aimee_message_free(&ir.messages[i]);
   free(ir.messages);
   ir.messages = NULL;
   ir.n_messages = 0;
   int count = cJSON_GetArraySize(messages);
   ir.messages = count ? calloc((size_t)count, sizeof *ir.messages) : NULL;
   if ((count && !ir.messages) || !cJSON_IsArray(messages))
      goto fail;
   for (int i = 0; i < count; ++i)
   {
      ++ir.n_messages;
      if (aimee_ir_message_parse(cJSON_GetArrayItem(messages, i), &ir.messages[i]))
         goto fail;
   }
   if (simple_policy || !host_context || !host_context->n_blocks)
   {
      for (int i = 0; i < profile.n_system; ++i)
      {
         if (simple_policy)
         {
            profile.system[i].context = host_context->blocks[0].context;
            profile.system[i].message_group = host_context->blocks[0].message_group;
         }
         else
            aimee_ir_block_set_context(&profile.system[i], AIMEE_CTX_ORIGIN_PLATFORM,
                                       AIMEE_CTX_AUTH_POLICY, AIMEE_CTX_TRUST_VERIFIED,
                                       AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
      }
      context_copy.system = profile.system;
      context_copy.n_system = profile.n_system;
      profile.system = NULL;
      profile.n_system = 0;
   }
   else
   {
      aimee_request_t view = {.system = host_context->blocks, .n_system = host_context->n_blocks};
      if (aimee_ir_request_clone(&view, &context_copy))
         goto fail;
   }
   if (ir.n_system > AIMEE_IR_MAX_BLOCKS - context_copy.n_system)
      goto fail;
   if (context_copy.n_system)
   {
      aimee_block_t *blocks =
          realloc(ir.system, (size_t)(ir.n_system + context_copy.n_system) * sizeof *blocks);
      if (!blocks)
         goto fail;
      ir.system = blocks;
      memcpy(blocks + ir.n_system, context_copy.system,
             (size_t)context_copy.n_system * sizeof *blocks);
      ir.n_system += context_copy.n_system;
      free(context_copy.system);
      context_copy.system = NULL;
      context_copy.n_system = 0;
   }
   if (ir.n_tools)
   {
      aimee_request_t requested = {.tools = ir.tools, .n_tools = ir.n_tools};
      aimee_request_t hosted = {.tools = profile.tools, .n_tools = profile.n_tools};
      if (!aimee_ir_request_wire_equal(&requested, &hosted))
         goto fail; /* A caller catalog cannot add or change host execution grants. */
   }
   else
   {
      ir.tools = profile.tools;
      ir.n_tools = profile.n_tools;
      profile.tools = NULL;
      profile.n_tools = 0;
   }
   if (ir.tool_choice == AIMEE_TOOL_CHOICE_UNSET)
   {
      ir.tool_choice = profile.tool_choice;
      free(ir.tool_choice_name);
      ir.tool_choice_name = profile.tool_choice_name;
      profile.tool_choice_name = NULL;
   }
   if (ir.tool_choice == AIMEE_TOOL_CHOICE_TOOL &&
       !aimee_ir_find_function(&ir, NULL, ir.tool_choice_name))
      goto fail;
   if (!seed)
      ir.stream = profile.stream;
   if (!ir.has_parallel_tool_calls)
   {
      ir.has_parallel_tool_calls = profile.has_parallel_tool_calls;
      ir.parallel_tool_calls = profile.parallel_tool_calls;
   }
   if (!ir.has_max_tokens)
   {
      ir.max_tokens = profile.max_tokens;
      ir.has_max_tokens = profile.has_max_tokens;
   }
   if (!ir.has_temperature)
   {
      ir.temperature = profile.temperature;
      ir.has_temperature = profile.has_temperature;
   }
   if (!ir.has_top_p)
   {
      ir.top_p = profile.top_p;
      ir.has_top_p = profile.has_top_p;
   }
   if (!ir.has_top_k)
   {
      ir.top_k = profile.top_k;
      ir.has_top_k = profile.has_top_k;
   }
   if (!ir.thinking)
   {
      ir.thinking = profile.thinking;
      profile.thinking = NULL;
   }
   if (!ir.reasoning_effort)
      ir.reasoning_effort = profile.reasoning_effort;
   if (!ir.reasoning_summary)
   {
      ir.reasoning_summary = profile.reasoning_summary;
      profile.reasoning_summary = NULL;
   }
   if (ir.tool_choice == AIMEE_TOOL_CHOICE_NONE)
   {
      if (agent->require_initial_tool_call)
         goto fail;
      for (int i = 0; i < ir.n_tools; ++i)
         aimee_tool_free(&ir.tools[i]);
      free(ir.tools);
      ir.tools = NULL;
      ir.n_tools = 0;
   }
   if (wire == AIMEE_WIRE_OPENAI_CHAT && agent_shape_ir_user(agent, &ir))
      goto fail;
   aimee_ir_classify_context(&ir);
   ir.mutated = 1;
   if (aimee_ir_request_admit(&ir) || gateway_delegate_run_request_pipeline(&ir, is_delegate) < 0)
      goto fail;
   aimee_request_free(out);
   *out = ir;
   aimee_request_free(&profile);
   aimee_request_free(&context_copy);
   return 0;
fail:
   aimee_request_free(&profile);
   aimee_request_free(&context_copy);
   aimee_request_free(&ir);
   return -1;
}

cJSON *agent_build_seeded_request_ir(const agent_t *agent, const aimee_request_t *seed,
                                     const cJSON *messages, const aimee_request_t *catalog,
                                     const aimee_message_t *host_context, int max_tokens,
                                     double temperature, aimee_wire_t wire, int is_delegate,
                                     int host_profile, aimee_request_t *dispatch_catalog)
{
   aimee_request_t ir = {0}, projected = {0};
   cJSON *rendered = NULL;
   if (agent_prepare_seeded_request_ir(agent, seed, messages, catalog, host_context, max_tokens,
                                       temperature, wire, is_delegate, &ir) ||
       agent_request_project_model(&ir, &projected) ||
       aimee_backend_preserve_host_extensions(&ir, &projected, wire))
      goto done;
   int omitted_controls = 0;
   if (host_profile && wire == AIMEE_WIRE_RESPONSES)
   {
      omitted_controls = projected.has_max_tokens + projected.has_temperature;
      projected.has_max_tokens = projected.has_temperature = 0;
   }
   if (omitted_controls)
   {
      aimee_session_event_t report = {.kind = AIMEE_SESSION_PROJECTION,
                                      .projection_model = agent->name,
                                      .projection_boundary = "request",
                                      .projection_request_controls = omitted_controls};
      if (aimee_ir_session_emit(&report))
         goto done;
   }
   aimee_wire_projection_report_t projection;
   aimee_backend_projection_report(&projected, wire, &projection);
   if (aimee_ir_session_emit_projection_controls(
           projected.model, projection.omitted_reasoning_blocks,
           projection.omitted_opaque_state_blocks, projection.omitted_cache_controls, "request"))
      goto done;
   rendered = wire == AIMEE_WIRE_ANTHROPIC   ? anthropic_backend_build(&projected)
              : wire == AIMEE_WIRE_RESPONSES ? responses_backend_build(&projected)
                                             : openai_backend_build(&projected);
   if (rendered && dispatch_catalog)
   {
      aimee_request_free(dispatch_catalog);
      *dispatch_catalog = (aimee_request_t){.tools = projected.tools, .n_tools = projected.n_tools};
      projected.tools = NULL;
      projected.n_tools = 0;
   }
done:
   aimee_request_free(&ir);
   aimee_request_free(&projected);
   return rendered;
}

/* --- Message utilities --- */

/* Core history is canonical. Legacy provider formats are admitted only by
 * their explicit translation/import boundary, never detected inside execution. */
int message_history_repair(cJSON *messages)
{
   if (!messages)
      return 0;
   if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) > AIMEE_IR_MAX_BLOCKS)
      return -1;
   const cJSON *item;
   cJSON_ArrayForEach(item, messages)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(item, &message))
         return -1;
      aimee_message_free(&message);
   }
   return aimee_ir_history_repair(messages);
}

/* ================================================================
 * Gemini request building and response parsing
 *
 * Gemini uses a different wire format from OpenAI/Anthropic:
 *   - Role "assistant" becomes "model"
 *   - Tool calls:  parts[{functionCall: {name, args}}]
 *   - Tool results: role "user", parts[{functionResponse: {name, response}}]
 *   - System prompt: separate "systemInstruction" field
 *
 * These helpers are also used by delegate_gemini.c so the driver vtable
 * and the direct agent-loop code share the same implementation.
 * ================================================================ */

/* ================================================================
 * From: agent_http.c
 * ================================================================ */

/* agent_http.c: HTTP client platform abstraction + shared provider health tracking +
 * provider prompt cache lifecycle (Gemini cached-content).
 * HTTP implementation is in posix/agent_http.c (Linux/macOS) and
 * windows/agent_http.c (Windows). */

/* ================================================================
 * Provider health tracking (shared across platforms)
 * ================================================================ */

#define MAX_TRACKED_PROVIDERS MAX_AGENTS
static struct
{
   char name[64];
   provider_health_t health;
} s_provider_health[MAX_TRACKED_PROVIDERS];
static int s_provider_health_count;
static pthread_mutex_t s_provider_health_lock = PTHREAD_MUTEX_INITIALIZER;

provider_err_class_t provider_classify_error(int http_status)
{
   if (http_status < 0)
      return PROVIDER_ERR_NETWORK;
   if (http_status == 401 || http_status == 403)
      return PROVIDER_ERR_AUTH;
   if (http_status == 429)
      return PROVIDER_ERR_RATE_LIMIT;
   if (http_status >= 500 && http_status < 600)
      return PROVIDER_ERR_SERVER;
   if (http_status >= 400 && http_status < 500)
      return PROVIDER_ERR_CLIENT;
   if (http_status >= 200 && http_status < 300)
      return PROVIDER_ERR_NONE;
   return PROVIDER_ERR_UNKNOWN;
}
const char *provider_error_message(provider_err_class_t cls)
{
   switch (cls)
   {
   case PROVIDER_ERR_NONE:
      return "ok";
   case PROVIDER_ERR_NETWORK:
      return "unreachable (connection failed or local HTTP client unavailable). Check network "
             "connection and server HTTP/SSL initialization.";
   case PROVIDER_ERR_AUTH:
      return "authentication failed. Check delegate provider credentials.";
   case PROVIDER_ERR_RATE_LIMIT:
      return "rate limited. Retry later.";
   case PROVIDER_ERR_SERVER:
      return "server error. Retry later.";
   case PROVIDER_ERR_CLIENT:
      return "client error. Check request parameters.";
   case PROVIDER_ERR_UNKNOWN:
      return "unknown error.";
   }
   return "unknown error.";
}
static provider_health_t *find_or_create_health(const char *provider_name)
{
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
         return &s_provider_health[i].health;
   }
   if (s_provider_health_count >= MAX_TRACKED_PROVIDERS)
      return NULL; /* Never attribute another registration's failure to the first. */
   int idx = s_provider_health_count++;
   snprintf(s_provider_health[idx].name, sizeof(s_provider_health[idx].name), "%s", provider_name);
   s_provider_health[idx].health.available = -1;
   s_provider_health[idx].health.last_http_status = -1;
   return &s_provider_health[idx].health;
}
void provider_health_update(const char *provider_name, int http_status)
{
   if (!provider_name || !provider_name[0])
      return;
   pthread_mutex_lock(&s_provider_health_lock);
   provider_health_t *h = find_or_create_health(provider_name);
   if (!h)
   {
      pthread_mutex_unlock(&s_provider_health_lock);
      return;
   }
   h->last_http_status = http_status;
   h->last_check_ms = (int64_t)time(NULL) * 1000;

   provider_err_class_t cls = provider_classify_error(http_status);
   if (cls == PROVIDER_ERR_NONE)
   {
      h->available = 1;
      h->error[0] = '\0';
   }
   else
   {
      h->available = 0;
      snprintf(h->error, sizeof(h->error), "%s", provider_error_message(cls));
   }
   pthread_mutex_unlock(&s_provider_health_lock);
}
const provider_health_t *provider_health_get(const char *provider_name)
{
   static _Thread_local provider_health_t snapshot;
   if (!provider_name || !provider_name[0])
      return NULL;
   pthread_mutex_lock(&s_provider_health_lock);
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
      {
         snapshot = s_provider_health[i].health;
         pthread_mutex_unlock(&s_provider_health_lock);
         return &snapshot;
      }
   }
   pthread_mutex_unlock(&s_provider_health_lock);
   return NULL;
}

/* ================================================================
 * From: agent_tunnel.c
 * ================================================================ */

/* agent_tunnel.c: reverse SSH tunnel lifecycle for NAT piercing */
/* Agent tunnel implementation is in posix/agent_tunnel.c (Linux/macOS)
 * and windows/agent_tunnel.c (Windows). */
