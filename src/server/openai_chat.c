#include "module_commands.h"
#include "json_fluent.h"
/* openai_chat.c: inference-backed handlers for the OpenAI-compatible
 * /v1/chat/completions and /v1/completions routes.
 *
 * Registered into the server_http dispatch via server_http_set_*_handler at
 * server startup (openai_chat_register). Keeping the inference here — rather
 * than in server_http.c — keeps the agent/provider dependency closure out of
 * the server_http translation unit and its unit test (which injects a stub
 * handler instead).
 *
 * /v1/chat/completions supports SSE streaming via chat_stream_handler (the
 * server_http stream seam); /v1/completions and /v1/responses are still
 * non-streaming and reject "stream":true. Streaming computes the full
 * completion, then emits it as chat.completion.chunk deltas (provider-
 * incremental token streaming is a follow-up). The handler runs on the HTTP
 * listener thread, which serves one connection at a time, so a completion
 * blocks that listener for its duration — acceptable for the local
 * single-owner surface this first cut targets. */
#include "server_http.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include <aimee/gateway/gateway_pipeline.h> /* gw_request_t + gw_pipeline_run_request — shared seam */
#include "ir_host_bindings.h"
#include "gw_stage_registry.h"                  /* Slice 7: config-driven stage catalog */
#include "gw_stage_governance.h"                /* response seam Slice 2: togglable governance */
#include "gw_stage_completion.h"                /* bounded incomplete-repair continuation */
#include "dogfood.h"                            /* dogfood_autolabel_next_turn_live */
#include "modules/learning/learning_implicit.h" /* learning_implicit_detect_turn */
#include "retrieval_outcome_bridge.h"           /* retrieval_outcome_bridge_on_autolabel */
#include "openai_runs_store.h"                  /* GET /v1/runs/{id} record store */
#include "aimee.h" /* EMBED_MAX_DIM, MAX_PATH_LEN (used by agent_types.h below) */
#include "log.h"   /* LOG_WARN: name the provider's error instead of discarding it */
#include "aimee_errors.h"
#include "config.h" /* legacy_config_record, legacy_config_read */
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_protocol.h" /* message_history_repair */
#include "agent_response_projection.h"
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "model_registry.h"                  /* configured/catalog context for model discovery */
#include <aimee/delegates/delegate_driver.h> /* single provider step for the Codex proxy */
#include "http_retry.h"
#include "wire_fence.h"
#include "gateway_mutate_wire.h"
#include "server_http_identity.h"
#include "cJSON.h"
#include <aimee/tools/agent_tools.h> /* agent_tools_set_tool_event_cb — /v1/runs tool events */
#include "agent_types.h"
#include <aimee/gateway/gateway_policy.h> /* gateway_policy_apply_request — tool-policing stage */
#include "router_advise.h"                /* gw_stage_router — the request->workflow seam */
#include "aimee_ir_serve.h"               /* IR-routed /v1/responses parse */
#include "request_context.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "platform_random.h"
#include "aimee_sha256.h"
#include "response_dedup.h"
#include "token_tracker.h"
#include "token_audit.h" /* db1_token_audit_insert for the avoided-turn row */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENAI_CHAT_MAX_TOKENS  2048
#define OPENAI_CHAT_TEMPERATURE 0.7

/* Legacy /v1/runs still accepts the Responses token-cap spelling. */
static int responses_request_max_tokens(const char *body)
{
   int legacy = openai_request_int(body, "max_tokens", 0, 32768);
   return openai_request_int(body, "max_output_tokens", legacy, 32768);
}

/* Record a dedup cache hit as a non-billable usage_kind=avoided audit row: the
 * provider call was skipped, so the avoided estimated cost is logged separately
 * and never added to spend totals (see db1_token_audit_spend_breakdown). Tokens
 * are zero because no real call happened. */
static void record_avoided_turn(const char *model, double avoided_cost)
{
   /* Carry the request-context attribution (#3) so an avoided row is attributed
    * to the same principal/request as a realized one. The avoided cost is a
    * special value (the prior realized cost replayed), so this writes directly
    * rather than via agent_record_token_audit (which recomputes cost from tokens
    * that are zero here); db1_token_audit_insert still applies the idempotency
    * guard on (source, request_id, attempt). */
   const request_context_t *rc = request_context_get();
   db1_token_audit_row_t row = {
       /* Attribute the avoided saving to the same source/session boundary as a
        * realized request: a trusted per-client source when stamped, else the
        * ingress origin; session_id mirrors the realized path's session(). */
       .session_id = session_id(),
       .tool_name = model && model[0] ? model : "aimee",
       .role = "",
       .model = model && model[0] ? model : "aimee",
       .source = (rc && rc->source[0]) ? rc->source : "openai-ingress",
       .requested_model = model ? model : "",
       .usage_kind = "avoided",
       .request_id = rc ? rc->request_id : "",
       .idempotency_key = rc ? rc->idempotency_key : "",
       .principal = rc ? rc->principal : "",
       .estimated_cost_usd = avoided_cost,
   };
   (void)db1_token_audit_insert(&row);
}

/* Dedup eligibility for the buffered completion path (§4): only when the flag is
 * on and the client supplied an explicit Idempotency-Key. The caller has already
 * rejected streaming, and this path runs no tools — both v1 requirements. The key
 * is built from the RESOLVED backend (ag), so it is computed after agent/config
 * resolution and two requests resolving to a different backend never collide.
 * Returns -1 when the required response-composition module cannot produce a key. */
static int dedup_eligible(char *key, size_t key_cap, const agent_t *ag, const char *endpoint,
                          const char *body, const char *context, int *ttl_out)
{
   if (!config_present() || !config_dedup_enabled())
      return 0;
   const char *idem = request_context_idempotency_key();
   if (!idem || !idem[0])
      return 0;
   const request_context_t *rc = request_context_get();
   /* Behaviour-affecting config flags that can change generation for this path. */
   char flags[96];
   snprintf(flags, sizeof(flags), "cs%d rc%d pi%d dw%d", config_cache_shaping_enabled(),
            config_reasoning_cap_enabled(), config_ingress_preinject_enabled(),
            config_dedup_window_seconds());
   response_dedup_key_inputs_t in = {
       .principal = request_context_principal(),
       .source = rc ? rc->source : "",
       .provider = ag ? ag->provider : "",
       .model = ag ? ag->model : "",
       .endpoint = endpoint,
       .stream = 0,
       .idempotency_key = idem,
       .body = body,
       .context = context,
       .behavior_flags = flags,
   };
   if (response_dedup_key(&in, key, key_cap) != 0)
      return -1;
   if (ttl_out)
      *ttl_out = config_dedup_window_seconds() > 0 ? config_dedup_window_seconds()
                                                   : RESPONSE_DEDUP_TTL_SECONDS;
   return 1;
}

/* Defined below, after the gateway pipeline they depend on; the chat tool-relay
 * branch in run_completion is their first caller. */
static int agent_execute_messages(const agent_t *agent, const char *body, aimee_wire_t wire,
                                  int max_tokens, double temperature, aimee_response_t *out,
                                  char *error, size_t capacity, int preserve_messages);
static int agent_execute_ir_request(const agent_t *agent, aimee_request_t *request, int max_tokens,
                                    double temperature, aimee_response_t *out, char *error,
                                    size_t capacity, int preserve_messages);
static cJSON *openai_ir_history_json(const aimee_request_t *request);
static int openai_ir_history_replace(aimee_request_t *request, const cJSON *history);
static int openai_governance_enabled(void);

/* The provider result remains owned IR through policy and the client boundary. */
static cJSON *openai_render_ir(const aimee_response_t *response, const char *id, const char *model,
                               long created, int responses)
{
   aimee_response_t view = *response;
   view.id = (char *)id;
   view.model = (char *)model;
   cJSON *root = responses ? responses_frontend_render(&view) : openai_frontend_render(&view);
   if (!root)
      return NULL;
   cJSON_AddNumberToObject(root, responses ? "created_at" : "created", (double)created);
   if (responses)
   {
      cJSON *item;
      int index = 0;
      cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "output"))
      {
         if (!cJSON_GetObjectItemCaseSensitive(item, "id"))
         {
            size_t capacity = strlen(id) + 40;
            char *item_id = malloc(capacity);
            if (!item_id)
            {
               cJSON_Delete(root);
               return NULL;
            }
            snprintf(item_id, capacity, "%s-item-%d", id, index);
            cJSON_AddStringToObject(item, "id", item_id);
            free(item_id);
         }
         cJSON_AddStringToObject(item, "status", "completed");
         index++;
      }
   }
   return root;
}
static int openai_ir_buffered(const aimee_response_t *response, const char *id, const char *model,
                              long created, int responses, char *buffer, int capacity)
{
   cJSON *root = openai_render_ir(response, id, model, created, responses);
   int length =
       root && cJSON_PrintPreallocated(root, buffer, capacity, 0) ? (int)strlen(buffer) : -1;
   cJSON_Delete(root);
   return length;
}
static int openai_ir_emit_event(server_http_sse_event_emit emit, void *context, const char *event,
                                cJSON *payload)
{
   if (!payload || !cJSON_AddStringToObject(payload, "type", event))
   {
      cJSON_Delete(payload);
      return -1;
   }
   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;
   emit(context, event, json);
   free(json);
   return 0;
}
static int openai_ir_chat_emit(const aimee_response_t *response, const char *id, const char *model,
                               long created, server_http_sse_emit emit, void *context)
{
   cJSON *root = openai_render_ir(response, id, model, created, 0);
   if (!root)
      return -1;
   cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
   cJSON *message = cJSON_DetachItemFromObjectCaseSensitive(choice, "message");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls"), *call;
   int index = 0;
   cJSON_ArrayForEach(call, calls) cJSON_AddNumberToObject(call, "index", index++);
   cJSON_AddItemToObject(choice, "delta", message);
   cJSON *finish = cJSON_DetachItemFromObjectCaseSensitive(choice, "finish_reason");
   cJSON_AddNullToObject(choice, "finish_reason");
   cJSON_ReplaceItemInObjectCaseSensitive(root, "object",
                                          cJSON_CreateString("chat.completion.chunk"));
   cJSON *usage = cJSON_DetachItemFromObjectCaseSensitive(root, "usage");
   char *json = cJSON_PrintUnformatted(root);
   if (!json)
   {
      cJSON_Delete(finish);
      cJSON_Delete(usage);
      cJSON_Delete(root);
      return -1;
   }
   emit(context, json);
   free(json);
   cJSON_ReplaceItemInObjectCaseSensitive(choice, "delta", cJSON_CreateObject());
   cJSON_ReplaceItemInObjectCaseSensitive(choice, "finish_reason", finish);
   cJSON_AddItemToObject(root, "usage", usage);
   json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   emit(context, json);
   free(json);
   return 0;
}
static cJSON *openai_ir_item_event(const cJSON *item, int index)
{
   cJSON *event = cJSON_CreateObject();
   if (!event || !cJSON_AddNumberToObject(event, "output_index", index) ||
       !cJSON_AddStringToObject(event, "item_id",
                                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"))))
   {
      cJSON_Delete(event);
      return NULL;
   }
   return event;
}
static int openai_ir_responses_emit(const aimee_response_t *response, const char *id,
                                    const char *model, long created,
                                    server_http_sse_event_emit emit, void *context)
{
   cJSON *root = openai_render_ir(response, id, model, created, 1);
   if (!root)
      return -1;
   cJSON *item;
   int index = 0, rc = -1;
   cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "output"))
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      cJSON *start = cJSON_Duplicate(item, 1);
      cJSON *event = openai_ir_item_event(item, index);
      if (!start || !event)
      {
         cJSON_Delete(start);
         cJSON_Delete(event);
         goto done;
      }
      cJSON_ReplaceItemInObjectCaseSensitive(start, "status", cJSON_CreateString("in_progress"));
      if (!strcmp(type, "message"))
         cJSON_ReplaceItemInObjectCaseSensitive(start, "content", cJSON_CreateArray());
      else if (!strcmp(type, "function_call"))
         cJSON_ReplaceItemInObjectCaseSensitive(start, "arguments", cJSON_CreateString(""));
      else if (!strcmp(type, "reasoning"))
      {
         cJSON_ReplaceItemInObjectCaseSensitive(start, "summary", cJSON_CreateArray());
         cJSON_DeleteItemFromObjectCaseSensitive(start, "encrypted_content");
      }
      cJSON_AddItemToObject(event, "item", start);
      if (openai_ir_emit_event(emit, context, "response.output_item.added", event))
         goto done;
      if (!strcmp(type, "function_call"))
      {
         const char *args =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "arguments"));
         event = openai_ir_item_event(item, index);
         cJSON_AddStringToObject(event, "delta", args);
         if (openai_ir_emit_event(emit, context, "response.function_call_arguments.delta", event))
            goto done;
         event = openai_ir_item_event(item, index);
         cJSON_AddStringToObject(event, "arguments", args);
         if (openai_ir_emit_event(emit, context, "response.function_call_arguments.done", event))
            goto done;
      }
      else
      {
         int reasoning = !strcmp(type, "reasoning");
         const char *parts_name = reasoning ? "summary" : "content";
         cJSON *part;
         int part_index = 0;
         cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(item, parts_name))
         {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part, "text"));
            const char *position = reasoning ? "summary_index" : "content_index";
            const char *added =
                reasoning ? "response.reasoning_summary_part.added" : "response.content_part.added";
            const char *delta =
                reasoning ? "response.reasoning_summary_text.delta" : "response.output_text.delta";
            const char *text_done =
                reasoning ? "response.reasoning_summary_text.done" : "response.output_text.done";
            const char *part_done =
                reasoning ? "response.reasoning_summary_part.done" : "response.content_part.done";
            cJSON *empty = cJSON_Duplicate(part, 1);
            cJSON_ReplaceItemInObjectCaseSensitive(empty, "text", cJSON_CreateString(""));
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddItemToObject(event, "part", empty);
            if (openai_ir_emit_event(emit, context, added, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddStringToObject(event, "delta", text ? text : "");
            if (openai_ir_emit_event(emit, context, delta, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddStringToObject(event, "text", text ? text : "");
            if (openai_ir_emit_event(emit, context, text_done, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddItemToObject(event, "part", cJSON_Duplicate(part, 1));
            if (openai_ir_emit_event(emit, context, part_done, event))
               goto done;
            part_index++;
         }
      }
      event = openai_ir_item_event(item, index);
      cJSON_AddItemToObject(event, "item", cJSON_Duplicate(item, 1));
      if (openai_ir_emit_event(emit, context, "response.output_item.done", event))
         goto done;
      index++;
   }
   cJSON *terminal = cJSON_CreateObject();
   cJSON_AddItemToObject(terminal, "response", cJSON_Duplicate(root, 1));
   rc = openai_ir_emit_event(emit, context,
                             response->stop_reason == AIMEE_STOP_MAX_TOKENS ? "response.incomplete"
                                                                            : "response.completed",
                             terminal);
done:
   cJSON_Delete(root);
   return rc;
}

/* Shared body for both endpoints. chat != 0 selects the chat.completion shape
 * (and chat-request parse); otherwise the legacy text_completion shape. */
/* A connector that packs/counts chat messages must keep that exact role shape.
 * Opt in explicitly; legacy text callers retain their existing assembly path. */
static int openai_request_preserves_messages(const char *body)
{
   cJSON *root = cJSON_Parse(body ? body : "");
   int preserve = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "preserve_messages"));
   cJSON_Delete(root);
   return preserve;
}

static int openai_request_has_assistant(const char *body)
{
   cJSON *root = body ? cJSON_Parse(body) : NULL;
   cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "messages") : NULL;
   if (!cJSON_IsArray(items))
      items = root ? cJSON_GetObjectItemCaseSensitive(root, "input") : NULL;
   int found = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, items)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "role"));
      if (role && strcmp(role, "assistant") == 0)
      {
         found = 1;
         break;
      }
   }
   cJSON_Delete(root);
   return found;
}

/* Persona for the LEGACY chat path only.
 *
 * When the IR path is live it owns persona placement (ir_stage_persona_instructions
 * puts it on the first user message), and both share one per-session delivery
 * claim -- so calling this unguarded would let the flat path win the claim and
 * silently move placement off the IR on the DEFAULT path. Guarding keeps each
 * mode delivering through its own seam, still exactly once. */
static char *legacy_persona_text(const char *text, int conversation_established)
{
   if (aimee_ir_path_enabled())
      return NULL;
   return aimee_ir_prepend_active_persona_text(text, conversation_established);
}

static int run_completion(int chat, const char *body, char *resp, int cap)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   int prc = chat ? openai_parse_chat_request(body, model, sizeof(model), &prompt, &stream)
                  : openai_parse_completion_request(body, model, sizeof(model), &prompt, &stream);
   if (prc != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          chat ? "invalid chat completion request: expected messages[] with content"
                               : "invalid completion request: expected a non-empty prompt");
      return 400;
   }
   if (stream)
   {
      free(prompt);
      openai_format_error(resp, cap, "invalid_request_error",
                          "streaming responses are not yet supported on this endpoint");
      return 400;
   }

   /* Resolve the backend BEFORE dedup so the key carries the resolved
    * provider/model — two requests resolving to a different backend must not
    * collide even with an identical body. Honour the requested model: "aimee"
    * (or empty) means the default agent; any other value selects a configured
    * agent by name, and is rejected with model_not_found when it matches none
    * (never silently falls back to a different model). */
   agent_t agbuf;
   agent_t *ag = agent_registry_resolve_ingress_model(model, &agbuf) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      free(prompt);
      if (model[0] && strcmp(model, "aimee") != 0)
      {
         openai_format_error(resp, cap, "model_not_found", "the requested model is not available");
         return 404;
      }
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }

   /* Tool relay: when the caller supplies `tools`, this endpoint must return the
    * model's tool_calls for the CALLER to execute — the stateless half of the
    * OpenAI contract that every agentic client depends on. Without it a client
    * sees a provider that cannot call tools and refuses to run at all.
    *
    * This does NOT make the endpoint agentic: aimee still executes nothing and
    * holds no loop state (that remains /v1/runs). agent_execute_messages is the
    * same passthrough the /v1/responses ingress uses, so inbound tools go
    * through the shared tool-policing stage rather than around it.
    *
    * Dedup is skipped here: its key does not cover `tools`, so a cached
    * tool-free reply must never be replayed for a tool-bearing request. */
   if (chat)
   {
      char *instructions = NULL;
      cJSON *messages = NULL, *tools = NULL;
      if (openai_split_chat_request(body, &instructions, &messages, &tools) == 0 &&
          (tools || openai_request_preserves_messages(body)))
      {
         aimee_response_t parsed;
         char upstream_error[512];
         int preserve = openai_request_preserves_messages(body) && !tools;
         int trc = agent_execute_messages(
             ag, body, AIMEE_WIRE_OPENAI_CHAT,
             openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768),
             openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0), &parsed,
             upstream_error, sizeof upstream_error, preserve);
         free(instructions);
         free(prompt);

         if (trc != 0)
         {
            aimee_response_free(&parsed);
            cJSON_Delete(messages);
            cJSON_Delete(tools);
            openai_format_error(resp, cap, wire_fence_error_type(upstream_error), upstream_error);
            return wire_fence_error_http_status(upstream_error);
         }

         if (agent_ingress_accounting_enabled())
            agent_ingress_record_cost(
                ag->name, ag->model, model,
                parsed.raw_stop_reason ? parsed.raw_stop_reason
                                       : aimee_stop_reason_name(parsed.stop_reason),
                (int)parsed.usage_in, (int)parsed.usage_out, (int)parsed.usage_cache_write,
                (int)parsed.usage_cache_read, "openai-ingress", NULL);

         long tcreated = (long)time(NULL);
         char tid[64];
         snprintf(tid, sizeof tid, "chatcmpl-%ld", tcreated);
         int tlen = openai_ir_buffered(&parsed, tid, model, tcreated, 0, resp, cap);
         aimee_response_free(&parsed);
         cJSON_Delete(messages);
         cJSON_Delete(tools);
         if (tlen < 0)
         {
            openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
            return 500;
         }
         return 200;
      }
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
   }

   /* Plain-text serving owns this envelope; the structured tool path above
    * runs memory/persona stages during final provider assembly instead.
    * Build it before dedup: it is part of the model input,
    * so the §4 dedup key must hash it (a stale reply must not be replayed after
    * the injected memory/context changes). On a cache miss it is reused for the
    * provider call below; on a hit it is freed unused. */
   int first_turn = !chat || !openai_request_has_assistant(body);
   char *pi_env = server_ir_plan_text("memory.runtime", "gateway-plan", "text", prompt);
   char *persona_txt = legacy_persona_text(prompt, !first_turn);
   if (persona_txt)
   {
      free(prompt);
      prompt = persona_txt;
   }

   /* §4 short-window dedup: serve a re-sent identical request (same account/source
    * boundary, RESOLVED provider+model, endpoint, stream mode, idempotency key,
    * body, pre-injected context, AND behaviour-config flags) from the TTL cache
    * without paying for the provider call again. Buffered + non-stream + tool-free
    * here. The key is built from the resolved agent above. */
   char dedup_key[288] = "";
   int dedup_ttl = RESPONSE_DEDUP_TTL_SECONDS;
   const char *endpoint = chat ? "/v1/chat/completions" : "/v1/completions";
   int dedup_on =
       dedup_eligible(dedup_key, sizeof(dedup_key), ag, endpoint, body, pi_env, &dedup_ttl);
   if (dedup_on < 0)
   {
      free(pi_env);
      free(prompt);
      openai_format_error(resp, cap, "server_error", "response composition unavailable");
      return 503;
   }
   if (dedup_on)
   {
      char *cached = NULL;
      double avoided = 0.0;
      if (response_dedup_get(dedup_key, (long)time(NULL), &cached, &avoided))
      {
         int n = snprintf(resp, cap, "%s", cached);
         free(cached);
         free(pi_env);
         free(prompt);
         if (n < 0 || n >= cap)
         {
            openai_format_error(resp, cap, "server_error",
                                "cached response did not fit the buffer");
            return 500;
         }
         if (agent_ingress_accounting_enabled())
            record_avoided_turn(model, avoided);
         return 200;
      }
   }

   /* Honour the OpenAI sampling params when present, else fall back to the
    * endpoint defaults. temperature clamps to [0, 2]; max_tokens to [1, hi]. */
   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* pi_env (the <aimee-context> envelope) was built up front for the dedup key;
    * reuse it as the system prompt here. */
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   if (erc != 0 || !result.response)
   {
      openai_format_error(resp, cap, wire_fence_error_type(result.error),
                          result.error[0] ? result.error : "completion failed");
      free(result.response);
      return wire_fence_error_http_status(result.error);
   }

   /* Cost accounting for the OpenAI-compatible ingress: this handler runs the
    * provider call directly (no agent_log_call), so record the turn's spend. */
   snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
   if (agent_ingress_accounting_enabled())
      agent_record_token_audit(&result, "", "openai-ingress");

   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "%s-%ld", chat ? "chatcmpl" : "cmpl", created);

   int len = chat ? openai_format_chat_completion(id, model, result.response, created,
                                                  result.prompt_tokens, result.completion_tokens,
                                                  resp, cap)
                  : openai_format_text_completion(id, model, result.response, created,
                                                  result.prompt_tokens, result.completion_tokens,
                                                  resp, cap);
   free(result.response);
   if (len < 0)
   {
      openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
      return 500;
   }

   /* Cache this successful, tool-free 200 so an immediate identical retry under
    * the same idempotency key is served without a second provider call. The
    * stored cost is the realized spend, replayed as the avoided cost on a hit. */
   if (dedup_on)
   {
      token_usage_t usage = {.input_tokens = result.prompt_tokens,
                             .output_tokens = result.completion_tokens,
                             .cache_write_tokens = result.cache_write_tokens,
                             .cache_read_tokens = result.cache_read_tokens};
      const char *bill_model = result.model[0] ? result.model : model;
      double cost = token_estimate_cost(bill_model, &usage);
      response_dedup_put(dedup_key, resp, cost, (long)time(NULL), dedup_ttl);
   }
   return 200;
}

static int chat_completions_handler(const char *body, char *resp, int cap)
{
   return run_completion(1, body, resp, cap);
}

static int completions_handler(const char *body, char *resp, int cap)
{
   return run_completion(0, body, resp, cap);
}

/* GET /v1/models provider: copy each configured agent name (capped at max) so
 * clients can target a specific (provider,model) binding via the `model`
 * field. Names are copied into the caller's slots — no escaping pointers. */
static int models_provider(char ids[][SERVER_HTTP_MODEL_ID_MAX], int max)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return 0;
   int k = 0;
   for (int i = 0; i < acfg.agent_count && k < max; i++)
   {
      if (!acfg.agents[i].name[0] || strcmp(acfg.agents[i].name, "aimee") == 0)
         continue; /* "aimee" is already advertised by the route */
      if (!agent_name_valid(acfg.agents[i].name))
         continue; /* never surface junk/over-long names as models (defensive) */
      snprintf(ids[k], SERVER_HTTP_MODEL_ID_MAX, "%s", acfg.agents[i].name);
      k++;
   }
   return k;
}

/* Append one Codex model-discovery entry (the proprietary schema Codex's model
 * manager expects). slug is what Codex echoes in the request `model` field to
 * select this primary-agent model. The full field set mirrors Codex's own
 * models cache so its deserializer accepts the entry. */
static void codex_add_model(cJSON *models, const char *slug, const char *description,
                            int context_window)
{
   cJSON *m = cJSON_CreateObject();
   if (!m)
      return;
   cJSON_AddStringToObject(m, "slug", slug);
   cJSON_AddStringToObject(m, "display_name", slug);
   cJSON_AddStringToObject(m, "description",
                           description && description[0] ? description : "Aimee primary agent.");
   cJSON_AddStringToObject(m, "default_reasoning_level", "medium");
   cJSON *levels = cJSON_AddArrayToObject(m, "supported_reasoning_levels");
   static const char *const effs[] = {"low", "medium", "high"};
   for (int i = 0; levels && i < 3; i++)
   {
      cJSON *lv = cJSON_CreateObject();
      cJSON_AddStringToObject(lv, "effort", effs[i]);
      cJSON_AddStringToObject(lv, "description", effs[i]);
      cJSON_AddItemToArray(levels, lv);
   }
   cJSON_AddStringToObject(m, "shell_type", "shell_command");
   cJSON_AddStringToObject(m, "visibility", "list");
   cJSON_AddBoolToObject(m, "supported_in_api", 1);
   cJSON_AddNumberToObject(m, "priority", 1);
   cJSON_AddItemToObject(m, "additional_speed_tiers", cJSON_CreateArray());
   cJSON_AddItemToObject(m, "service_tiers", cJSON_CreateArray());
   cJSON *nux = cJSON_AddObjectToObject(m, "availability_nux");
   if (nux)
      cJSON_AddStringToObject(nux, "message", "");
   cJSON_AddNullToObject(m, "upgrade");
   cJSON_AddStringToObject(m, "base_instructions", "");
   cJSON *mm = cJSON_AddObjectToObject(m, "model_messages");
   if (mm)
   {
      cJSON_AddStringToObject(mm, "instructions_template", "");
      cJSON_AddObjectToObject(mm, "instructions_variables");
   }
   cJSON_AddBoolToObject(m, "supports_reasoning_summaries", 0);
   cJSON_AddStringToObject(m, "default_reasoning_summary", "none");
   cJSON_AddBoolToObject(m, "support_verbosity", 0);
   cJSON_AddStringToObject(m, "default_verbosity", "low");
   cJSON_AddStringToObject(m, "apply_patch_tool_type", "freeform");
   cJSON_AddStringToObject(m, "web_search_tool_type", "text_and_image");
   cJSON *trunc = cJSON_AddObjectToObject(m, "truncation_policy");
   if (trunc)
   {
      cJSON_AddStringToObject(trunc, "mode", "tokens");
      cJSON_AddNumberToObject(trunc, "limit", 10000);
   }
   cJSON_AddBoolToObject(m, "supports_parallel_tool_calls", 1);
   cJSON_AddBoolToObject(m, "supports_image_detail_original", 0);
   cJSON_AddNumberToObject(m, "context_window", context_window);
   cJSON_AddNumberToObject(m, "max_context_window", context_window);
   cJSON_AddNumberToObject(m, "effective_context_window_percent", 95);
   cJSON_AddItemToObject(m, "experimental_supported_tools", cJSON_CreateArray());
   cJSON *mods = cJSON_AddArrayToObject(m, "input_modalities");
   if (mods)
      cJSON_AddItemToArray(mods, cJSON_CreateString("text"));
   cJSON_AddBoolToObject(m, "supports_search_tool", 0);
   cJSON_AddItemToArray(models, m);
}

/* GET /v1/models (raw): emit the Codex `{models:[…]}` discovery schema from the
 * registered primary-agent models, alongside the OpenAI `{object,data}` list for
 * other clients. One `models` entry per agent (plus the default "aimee"); the
 * slug is the id Codex puts in the request `model` field to switch models. */
#define CODEX_FALLBACK_CONTEXT_WINDOW 32768

/* Codex uses this advertised value to budget and truncate the ordinary API
 * session.  It therefore has to describe the selected delegate, not Codex's own
 * historical default.  Prefer the operator's per-agent override, then the model
 * catalog, and finally a conservative compatibility fallback. */
static int codex_agent_context_window(const agent_t *agent)
{
   if (agent && agent->middleware.context_window > 0)
      return agent->middleware.context_window;
   if (agent)
   {
      model_capability_t cap;
      memset(&cap, 0, sizeof cap);
      if (model_capability_get(agent_catalog_provider(agent), agent->model, &cap) &&
          cap.context_window > 0)
         return cap.context_window;
   }
   return CODEX_FALLBACK_CONTEXT_WINDOW;
}

static int codex_models_raw(char *resp, int cap)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "object", "list");
   cJSON *data = cJSON_AddArrayToObject(root, "data");
   cJSON *models = cJSON_AddArrayToObject(root, "models");
   if (!data || !models)
   {
      cJSON_Delete(root);
      return -1;
   }

   /* Always advertise the default "aimee" model, then each named agent. */
   int seen_aimee = 0;
   agent_t *primary = agent_default_primary(&acfg);
   codex_add_model(models, "aimee", "Aimee primary agent.", codex_agent_context_window(primary));
   cJSON *de = cJSON_CreateObject();
   cJSON_AddStringToObject(de, "id", "aimee");
   cJSON_AddStringToObject(de, "object", "model");
   cJSON_AddStringToObject(de, "owned_by", "aimee");
   cJSON_AddItemToArray(data, de);

   for (int i = 0; i < acfg.agent_count; i++)
   {
      const char *nm = acfg.agents[i].name;
      if (!nm || !nm[0] || strcmp(nm, "aimee") == 0)
      {
         if (nm && strcmp(nm, "aimee") == 0)
            seen_aimee = 1;
         continue;
      }
      codex_add_model(models, nm, acfg.agents[i].model,
                      codex_agent_context_window(&acfg.agents[i]));
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "id", nm);
      cJSON_AddStringToObject(e, "object", "model");
      cJSON_AddStringToObject(e, "owned_by", "aimee");
      cJSON_AddItemToArray(data, e);
   }
   (void)seen_aimee;

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* POST /v1/embeddings: embed each input via the configured embedder
 * (the shared Go embedding command) and shape the OpenAI embeddings list. Returns 502 when
 * the embedder is unavailable (e.g. the sidecar isn't running). */
static int embeddings_handler(const char *body, char *resp, int cap)
{
   char model[64] = "";
   char **inputs = NULL;
   int n = 0;
   if (openai_parse_embeddings_request(body, model, sizeof(model), &inputs, &n) != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          "invalid embeddings request: expected `input` string or array");
      return 400;
   }
   const char *cmd = config_embedder_command_current(NULL);

   float **vecs = calloc((size_t)n, sizeof(float *));
   int *dims = calloc((size_t)n, sizeof(int));
   if (!vecs || !dims)
   {
      free(vecs);
      free(dims);
      openai_free_inputs(inputs, n);
      openai_format_error(resp, cap, "server_error", "out of memory");
      return 500;
   }

   int ok = 1;
   int prompt_chars = 0;
   for (int i = 0; i < n; i++)
   {
      vecs[i] = malloc(sizeof(float) * EMBED_MAX_DIM);
      if (!vecs[i])
      {
         ok = 0;
         break;
      }
      cJSON *embed_0_args = cJSON_CreateObject(), *embed_0_reply = NULL;
      cJSON_AddStringToObject(embed_0_args, "base_url", cmd);
      cJSON_AddStringToObject(embed_0_args, "input_type", "document");
      cJSON_AddStringToObject(embed_0_args, "text", inputs[i]);
      cJSON_AddNumberToObject(embed_0_args, "max_dim", EMBED_MAX_DIM);
      (void)aimee_module_commands_dispatch_internal("memory.embed_text", embed_0_args,
                                                    &embed_0_reply);
      cJSON_Delete(embed_0_args);
      int d = jo_float_array(cJSON_GetObjectItemCaseSensitive(embed_0_reply, "vector"), vecs[i],
                             EMBED_MAX_DIM);
      cJSON_Delete(embed_0_reply);
      if (d <= 0)
      {
         ok = 0;
         break;
      }
      dims[i] = d;
      prompt_chars += (int)strlen(inputs[i]);
   }

   int status;
   if (!ok)
   {
      openai_format_error(resp, cap, "upstream_error",
                          "embedding generation failed (is the embedder available?)");
      status = 502;
   }
   else
   {
      /* ~4 chars/token is the usual OpenAI rule of thumb. */
      int len = openai_format_embeddings(model, (const float *const *)vecs, dims, n,
                                         prompt_chars / 4, resp, cap);
      if (len < 0)
      {
         openai_format_error(resp, cap, "server_error",
                             "embeddings response did not fit the buffer");
         status = 500;
      }
      else
         status = 200;
   }

   for (int i = 0; i < n; i++)
      free(vecs[i]);
   free(vecs);
   free(dims);
   openai_free_inputs(inputs, n);
   return status;
}

/* Unique /v1/responses id (the listener serves one connection at a time, so the
 * counter needs no lock); distinct ids let a client chain turns even within the
 * same wall-clock second. Shared by the unary and streaming handlers. */
static int responses_mint_id(long created, char *buffer, size_t capacity)
{
   (void)created;
   char random[33];
   if (capacity < 38 || platform_random_hex(random, 32))
      return -1;
   snprintf(buffer, capacity, "resp_%s", random);
   return 0;
}

/* Responses continuation is a canonical checkpoint in the existing durable
 * session log. The authenticated actor and, when supplied, parent session scope
 * must match on every load; an unknown id never becomes an empty conversation. */
static int responses_history_scope(char scope[80])
{
   const char *session = server_http_identity_session_hdr();
   if (!session || !session[0])
   {
      snprintf(scope, 80, "responses");
      return 0;
   }
   char hash[65];
   if (aimee_sha256_hex(session, strlen(session), hash))
      return -1;
   snprintf(scope, 80, "responses:%s", hash);
   return 0;
}
static int responses_restore_history(aimee_request_t *request, char *error, size_t capacity)
{
   const cJSON *previous = cJSON_GetObjectItemCaseSensitive(request->raw, "previous_response_id");
   if (!previous || cJSON_IsNull(previous))
      return 0;
   const char *id = cJSON_GetStringValue(previous);
   const char *actor = server_http_identity_principal();
   char scope[80], *json = NULL;
   if (!id || strncmp(id, "resp_", 5) || strlen(id) != 37 ||
       strspn(id + 5, "0123456789abcdef") != 32 || !actor || !actor[0] ||
       responses_history_scope(scope))
   {
      snprintf(error, capacity, "invalid or unauthorized previous_response_id");
      return -1;
   }
   int status = db1_session_ir_history_load(id, scope, actor, "", &json);
   if (status)
   {
      snprintf(error, capacity, "%s",
               status > 0 ? "previous_response_id was not found in this session scope"
                          : "durable Responses history is unavailable");
      return -1;
   }
   cJSON *history = cJSON_Parse(json);
   free(json);
   cJSON *current = openai_ir_history_json(request);
   if (!cJSON_IsArray(history) || !current)
   {
      cJSON_Delete(history);
      cJSON_Delete(current);
      return -1;
   }
   while (cJSON_GetArraySize(current))
   {
      cJSON *message = cJSON_DetachItemFromArray(current, 0);
      if (!cJSON_AddItemToArray(history, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(history);
         cJSON_Delete(current);
         return -1;
      }
   }
   cJSON_Delete(current);
   int rc = openai_ir_history_replace(request, history);
   cJSON_Delete(history);
   return rc;
}
static int responses_store_history(const char *id, const aimee_request_t *request,
                                   const aimee_response_t *response)
{
   const char *actor = server_http_identity_principal();
   char scope[80];
   if (!actor || !actor[0] || responses_history_scope(scope))
      return -1;
   cJSON *history = openai_ir_history_json(request);
   aimee_message_t assistant = {
       .role = "assistant", .blocks = response->content, .n_blocks = response->n_content};
   cJSON *message = aimee_ir_message_json(&assistant);
   if (!history || !message || !cJSON_AddItemToArray(history, message))
   {
      cJSON_Delete(history);
      cJSON_Delete(message);
      return -1;
   }
   char *json = cJSON_PrintUnformatted(history);
   cJSON_Delete(history);
   int rc = json && !db1_server_session_create(id, "responses", actor)
                ? db1_session_ir_history_save(id, "response", scope, actor, json)
                : -1;
   free(json);
   return rc;
}
static int responses_admit_request(const char *body, aimee_request_t *request, char *error,
                                   size_t capacity)
{
   if (aimee_frontend_parse_wire(body, body ? strlen(body) : 0, AIMEE_WIRE_RESPONSES, request,
                                 error, capacity))
      return -1;
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(request->raw, "input");
   if ((!cJSON_IsString(input) && !cJSON_IsArray(input)) || aimee_ir_request_admit(request))
   {
      snprintf(error, capacity, "invalid canonical Responses input");
      return -1;
   }
   if (responses_restore_history(request, error, capacity) || aimee_ir_request_admit(request))
      return -1;
   return 0;
}
static void responses_account(const agent_t *agent, const char *model,
                              const aimee_response_t *response)
{
   if (agent_ingress_accounting_enabled())
      agent_ingress_record_cost(
          agent->name, agent->model, model,
          response->raw_stop_reason ? response->raw_stop_reason
                                    : aimee_stop_reason_name(response->stop_reason),
          (int)response->usage_in, (int)response->usage_out, (int)response->usage_cache_write,
          (int)response->usage_cache_read, "openai-ingress", NULL);
}
static int responses_handler(const char *body, char *buffer, int capacity)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid Responses request";
   int status = 400;
   if (responses_admit_request(body, &request, error, sizeof error))
      goto failed;
   if (request.stream)
   {
      snprintf(error, sizeof error, "streaming request requires the streaming route");
      goto failed;
   }
   agent_t agent;
   if (agent_registry_resolve_ingress_model(request.model ? request.model : "", &agent))
   {
      status = 404;
      snprintf(error, sizeof error, "the requested model is not available");
      goto failed;
   }
   char *model = strdup(request.model ? request.model : agent.model);
   if (!model)
   {
      status = 500;
      goto failed;
   }
   if (agent_execute_ir_request(&agent, &request, 0, -1, &response, error, sizeof error, 0))
   {
      status = wire_fence_error_http_status(error);
      free(model);
      goto failed;
   }
   responses_account(&agent, model, &response);
   char id[64];
   if (responses_mint_id((long)time(NULL), id, sizeof id) ||
       responses_store_history(id, &request, &response))
   {
      status = 503;
      snprintf(error, sizeof error, "durable Responses history could not be committed");
      free(model);
      goto failed;
   }
   int length = openai_ir_buffered(&response, id, model, (long)time(NULL), 1, buffer, capacity);
   free(model);
   aimee_request_free(&request);
   aimee_response_free(&response);
   if (length >= 0)
      return 200;
   openai_format_error(buffer, capacity, "server_error",
                       "canonical response did not fit the buffer");
   return 500;
failed:
   openai_format_error(buffer, capacity, status == 400 ? "invalid_request_error" : "server_error",
                       error);
   aimee_request_free(&request);
   aimee_response_free(&response);
   return status;
}

/* SSE streaming for POST /v1/chat/completions (stream:true). First cut:
 * compute the full completion (non-incremental) and emit it as OpenAI
 * chat.completion.chunk frames — a role frame, the content split into deltas,
 * and a terminal finish frame. server_http wraps each frame as `data: …` and
 * appends `data: [DONE]`. Provider-incremental token streaming is a follow-up;
 * this already lets OpenAI streaming clients work end-to-end. */
#define OPENAI_STREAM_CHUNK 80

static void emit_chunk(server_http_sse_emit emit, void *ctx, const char *id, const char *model,
                       long created, int role, const char *content, int finish)
{
   char frame[1024];
   if (openai_format_chat_chunk(id, model, created, role, content, finish, frame, sizeof(frame)) >
       0)
      emit(ctx, frame);
}

static void emit_text_chunk(server_http_sse_emit emit, void *ctx, const char *id, const char *model,
                            long created, const char *text, int finish)
{
   char frame[1024];
   if (openai_format_text_chunk(id, model, created, text, finish, frame, sizeof(frame)) > 0)
      emit(ctx, frame);
}

/* Fills `out` with the selected agent; returns 0 on success.
 *
 * Took an agent_config_t* and returned a pointer into it, which meant every
 * caller put 350,968 bytes on the stack of a request thread to obtain one
 * 16,720-byte agent. It now asks the registry for just that agent. */
static int stream_pick_agent(agent_t *out, const char *model)
{
   return agent_registry_resolve_ingress_model(model, out);
}

static int chat_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "chatcmpl-%ld", created);

   if (openai_parse_chat_request(body, model, sizeof(model), &prompt, &stream) != 0)
   {
      emit_chunk(emit, ctx, id, model, created, 1, "invalid chat completion request", 0);
      emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
      free(prompt);
      return 0;
   }

   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      emit_chunk(emit, ctx, id, model, created, 1, "no agent configured", 0);
      emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
      free(prompt);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   /* Tool relay, streaming half. Agentic clients stream by default, so without
    * this the buffered relay in run_completion would never be reached by them.
    * Same statelessness contract as that branch. This handler already computes
    * the whole reply before chunking it, so the calls ship in a single delta
    * followed by a tool_calls finish frame. */
   {
      char *instructions = NULL;
      cJSON *messages = NULL, *tools = NULL;
      if (openai_split_chat_request(body, &instructions, &messages, &tools) == 0 && tools)
      {
         aimee_response_t parsed;
         char upstream_error[512];
         int trc = agent_execute_messages(ag, body, AIMEE_WIRE_OPENAI_CHAT, max_tokens, temperature,
                                          &parsed, upstream_error, sizeof(upstream_error), 0);
         free(instructions);
         free(prompt);

         emit_chunk(emit, ctx, id, model, created, 1, NULL, 0); /* role frame */
         if (trc != 0)
         {
            char error_frame[1024];
            openai_format_error(error_frame, sizeof(error_frame),
                                wire_fence_error_type(upstream_error), upstream_error);
            emit(ctx, error_frame);
            emit(ctx, "[DONE]");
            aimee_response_free(&parsed);
            cJSON_Delete(messages);
            cJSON_Delete(tools);
            return 0;
         }

         if (agent_ingress_accounting_enabled())
            agent_ingress_record_cost(
                ag->name, ag->model, model,
                parsed.raw_stop_reason ? parsed.raw_stop_reason
                                       : aimee_stop_reason_name(parsed.stop_reason),
                (int)parsed.usage_in, (int)parsed.usage_out, (int)parsed.usage_cache_write,
                (int)parsed.usage_cache_read, "openai-ingress", NULL);

         if (openai_ir_chat_emit(&parsed, id, model, created, emit, ctx))
         {
            char failure[256];
            openai_format_error(failure, sizeof failure, "server_error",
                                "canonical response rendering failed");
            emit(ctx, failure);
         }
         emit(ctx, "[DONE]");
         aimee_response_free(&parsed);
         cJSON_Delete(messages);
         cJSON_Delete(tools);
         return 0;
      }
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   int first_turn = !openai_request_has_assistant(body);
   char *pi_env = server_ir_plan_text("memory.runtime", "gateway-plan", "text", prompt);
   char *persona_txt = legacy_persona_text(prompt, !first_turn);
   if (persona_txt)
   {
      free(prompt);
      prompt = persona_txt;
   }
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   emit_chunk(emit, ctx, id, model, created, 1, NULL, 0); /* role frame */
   if (erc != 0 || !result.response)
   {
      emit_chunk(emit, ctx, id, model, created, 0,
                 result.error[0] ? result.error : "completion failed", 0);
   }
   else
   {
      /* Cost accounting: streaming chat/completions runs the provider call
       * directly (no agent_log_call). */
      snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&result, "", "openai-ingress");
      text_sanitize_utf8(result.response);
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n;)
      {
         char seg[OPENAI_STREAM_CHUNK + 1];
         size_t take = text_utf8_chunk_len(txt + off, n - off, OPENAI_STREAM_CHUNK);
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         emit_chunk(emit, ctx, id, model, created, 0, seg, 0);
         off += take;
      }
   }
   emit_chunk(emit, ctx, id, model, created, 0, NULL, 1); /* finish frame */
   free(result.response);
   return 0;
}

/* SSE streaming for POST /v1/completions (legacy text_completion). Same
 * compute-then-chunk shape as chat, emitting text_completion chunk frames. */
static int completion_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "cmpl-%ld", created);

   if (openai_parse_completion_request(body, model, sizeof(model), &prompt, &stream) != 0)
   {
      emit_text_chunk(emit, ctx, id, model, created, "invalid completion request", 0);
      emit_text_chunk(emit, ctx, id, model, created, "", 1);
      free(prompt);
      return 0;
   }

   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      emit_text_chunk(emit, ctx, id, model, created, "no agent configured", 0);
      emit_text_chunk(emit, ctx, id, model, created, "", 1);
      free(prompt);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   char *pi_env = server_ir_plan_text("memory.runtime", "gateway-plan", "text", prompt);
   char *persona_txt = legacy_persona_text(prompt, 0);
   if (persona_txt)
   {
      free(prompt);
      prompt = persona_txt;
   }
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   if (erc != 0 || !result.response)
   {
      emit_text_chunk(emit, ctx, id, model, created,
                      result.error[0] ? result.error : "completion failed", 0);
   }
   else
   {
      /* Cost accounting: streaming completions runs the provider call directly. */
      snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&result, "", "openai-ingress");
      text_sanitize_utf8(result.response);
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n;)
      {
         char seg[OPENAI_STREAM_CHUNK + 1];
         size_t take = text_utf8_chunk_len(txt + off, n - off, OPENAI_STREAM_CHUNK);
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         emit_text_chunk(emit, ctx, id, model, created, seg, 0);
         off += take;
      }
   }
   emit_text_chunk(emit, ctx, id, model, created, "", 1); /* finish frame */
   free(result.response);
   return 0;
}

/* ---- OpenAI-ingress gateway request pipeline (universal-gateway) -------------
 * The /v1/responses ingress runs the same ordered [memory, tool_policing] stages
 * over a gw_request_t as the Anthropic ingress (anthropic_http.c), so both APIs
 * aimee presents share the one audited pipeline seam. The OpenAI request reaches
 * the provider step DECOMPOSED (an instructions string + a tools array) rather
 * than as a single inbound cJSON, so `raw` is a small IR object assembled from
 * those two mutable fields; the stages mutate it and the caller reads the results
 * back. Behavior is identical to the prior inline memory block + the P2b tool
 * strip it replaces. */

static cJSON *openai_ir_history_json(const aimee_request_t *request)
{
   cJSON *history = cJSON_CreateArray();
   if (!history)
      return NULL;
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = aimee_ir_message_json(&request->messages[i]);
      if (!message || !cJSON_AddItemToArray(history, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(history);
         return NULL;
      }
   }
   return history;
}
static int openai_ir_history_replace(aimee_request_t *request, const cJSON *history)
{
   int count = cJSON_GetArraySize(history);
   if (!cJSON_IsArray(history) || count > AIMEE_IR_MAX_BLOCKS)
      return -1;
   aimee_request_t replacement = {0};
   replacement.messages = count ? calloc((size_t)count, sizeof *replacement.messages) : NULL;
   if (count && !replacement.messages)
      return -1;
   replacement.n_messages = count;
   for (int i = 0; i < count; i++)
      if (aimee_ir_message_parse(cJSON_GetArrayItem(history, i), &replacement.messages[i]))
      {
         aimee_request_free(&replacement);
         return -1;
      }
   for (int i = 0; i < request->n_messages; i++)
      aimee_message_free(&request->messages[i]);
   free(request->messages);
   request->messages = replacement.messages;
   request->n_messages = replacement.n_messages;
   request->mutated = 1;
   return 0;
}
static int openai_ir_append_policy(aimee_request_t *request, char *policy)
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
static char *openai_build_ir_body(const aimee_request_t *request, aimee_wire_t wire)
{
   if (aimee_ir_request_admit(request))
      return NULL;
   char *body = aimee_backend_render_wire(request, wire);
   if (!body)
      return NULL;
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
static int openai_prepare_ir_request(aimee_request_t *request, const agent_t *agent, int max_tokens,
                                     double temperature, int preserve_messages)
{
   if (aimee_ir_request_admit(request))
      return -1;
   char *model = strdup(agent->model);
   if (!model)
      return -1;
   free(request->model);
   request->model = model; /* Registered agent selection resolves to its actual provider model. */
   request->stream = 0;    /* This transport buffers; the provider profile may require SSE. */
   if (max_tokens > 0)
   {
      request->max_tokens = agent_request_max_tokens(agent, max_tokens);
      request->has_max_tokens = 1;
   }
   if (!request->has_max_tokens && !strcmp(agent->provider, "anthropic"))
   {
      request->max_tokens = agent_request_max_tokens(agent, 0);
      request->has_max_tokens = request->max_tokens > 0;
   }
   if (!request->has_temperature && temperature >= 0)
   {
      request->temperature = temperature;
      request->has_temperature = 1;
   }
   gateway_policy_apply_ir(request);
   router_advise_ir(request);
   if (!preserve_messages)
   {
      if (openai_ir_append_policy(request, gw_ir_request_tool_system_prompt(request, NULL)) ||
          openai_ir_append_policy(request, gw_ir_request_completion_system_prompt(request, NULL)))
         return -1;
      aimee_ir_apply_gateway_request_stages(request);
   }
   return aimee_ir_request_admit(request);
}
static int openai_post_ir(const agent_t *agent, const aimee_request_t *request, aimee_wire_t wire,
                          const char *url, const char *auth, const char *headers, char **reply,
                          char *error, size_t capacity)
{
   char *body = openai_build_ir_body(request, wire);
   if (!body)
   {
      snprintf(error, capacity, "the selected provider cannot represent the canonical request");
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   wire_fence_route_t route = wire == AIMEE_WIRE_ANTHROPIC   ? WIRE_FENCE_ANTHROPIC_MESSAGES
                              : wire == AIMEE_WIRE_RESPONSES ? WIRE_FENCE_OPENAI_RESPONSES
                                                             : WIRE_FENCE_OPENAI_CHAT;
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t bytes;
   if (wire_fence_select(econ_mode_current() != ECON_MODE_OFF, route, body, strlen(body), &snapshot,
                         &bytes))
   {
      snprintf(error, capacity, "%s", wire_fence_last_error());
      free(body);
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   int attempts =
       config_retry_max_attempts() > 0 ? config_retry_max_attempts() : HTTP_RETRY_MAX_ATTEMPTS;
   int base = config_retry_base_ms() > 0 ? config_retry_base_ms() : HTTP_RETRY_BASE_MS;
   int maximum = config_retry_max_ms() > 0 ? config_retry_max_ms() : HTTP_RETRY_MAX_MS;
   int status =
       wire_fence_post(url, auth, bytes.data, bytes.len, reply, agent->timeout_ms, headers,
                       attempts, base, maximum, agent->provider, agent->model, NULL, route);
   wire_fence_destroy(snapshot);
   free(body);
   if (status == HTTP_RETRY_ADMISSION_REFUSED)
      snprintf(error, capacity, "%s", wire_fence_last_error());
   return status;
}
static int agent_execute_ir_request(const agent_t *agent, aimee_request_t *request, int max_tokens,
                                    double temperature, aimee_response_t *out, char *error,
                                    size_t capacity, int preserve_messages)
{
   memset(out, 0, sizeof *out);
   snprintf(error, capacity, "canonical provider request failed");
   if (!agent || !request ||
       openai_prepare_ir_request(request, agent, max_tokens, temperature, preserve_messages))
      return -1;
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   char url[MAX_ENDPOINT_LEN + 64], auth[MAX_API_KEY_LEN + 32], headers[512];
   if (!driver || delegate_build_url(driver, agent, url, sizeof url))
   {
      snprintf(error, capacity, "upstream model provider or endpoint is unavailable");
      return -1;
   }
   if (agent_resolve_auth(agent, auth, sizeof auth))
   {
      snprintf(error, capacity, "upstream model credentials are unavailable");
      return -1;
   }
   agent_build_extra_headers(agent, headers, sizeof headers);
   aimee_wire_t wire = !strcmp(driver->name, "anthropic") ? AIMEE_WIRE_ANTHROPIC
                       : strstr(url, "/responses")        ? AIMEE_WIRE_RESPONSES
                                                          : AIMEE_WIRE_OPENAI_CHAT;
   gw_mutate_ctx_t mutation;
   gw_mutate_ctx_init(&mutation);
   cJSON *container = NULL;
   if (!preserve_messages && gw_mutate_upstream_ok(wire == AIMEE_WIRE_ANTHROPIC))
   {
      container = cJSON_CreateObject();
      cJSON *history = openai_ir_history_json(request);
      if (!container || !history || !cJSON_AddItemToObject(container, "input", history))
      {
         cJSON_Delete(history);
         cJSON_Delete(container);
         return -1;
      }
      gw_buffered_mutate(container, "input", agent->model, "", server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(),
                         &mutation);
      if (mutation.mutated &&
          openai_ir_history_replace(request, cJSON_GetObjectItemCaseSensitive(container, "input")))
         goto refused;
   }
   char *reply = NULL;
   int status = openai_post_ir(agent, request, wire, url, auth, headers, &reply, error, capacity);
   if (status != HTTP_RETRY_ADMISSION_REFUSED && container &&
       gw_buffered_after_status(container, "input", status, &mutation) == GW_POST_RESEND)
   {
      free(reply);
      reply = NULL;
      if (openai_ir_history_replace(request, cJSON_GetObjectItemCaseSensitive(container, "input")))
         goto refused;
      status = openai_post_ir(agent, request, wire, url, auth, headers, &reply, error, capacity);
   }
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   if (status != 200 || !reply)
   {
      if (status != HTTP_RETRY_ADMISSION_REFUSED)
         openai_upstream_error_message(status, reply, error, capacity);
      free(reply);
      return -1;
   }
   int rc = aimee_backend_parse_wire(reply, strlen(reply), wire, out, error, capacity);
   free(reply);
   if (rc || out->error_message || aimee_ir_session_emit_buffered_response(out, NULL))
   {
      if (!rc)
         snprintf(error, capacity, "%s",
                  out->error_message ? out->error_message
                                     : "canonical response persistence failed");
      return -1;
   }
   rc = agent_response_govern(out, openai_governance_enabled(), gateway_prevent_subagents_enabled(),
                              NULL, 0);
   if (rc >= 0 && !preserve_messages)
      rc = gw_ir_response_run_completion(out, request);
   if (rc < 0)
   {
      snprintf(error, capacity, "canonical response policy or persistence failed");
      return -1;
   }
   return 0;
refused:
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   snprintf(error, capacity, "canonical economizer history is invalid");
   return -1;
}
static int agent_execute_messages(const agent_t *agent, const char *body, aimee_wire_t wire,
                                  int max_tokens, double temperature, aimee_response_t *out,
                                  char *error, size_t capacity, int preserve_messages)
{
   memset(out, 0, sizeof *out);
   aimee_request_t request = {0};
   if (aimee_frontend_parse_wire(body, body ? strlen(body) : 0, wire, &request, error, capacity))
      return -1;
   int rc = agent_execute_ir_request(agent, &request, max_tokens, temperature, out, error, capacity,
                                     preserve_messages);
   aimee_request_free(&request);
   return rc;
}

/* SSE streaming for POST /v1/responses — full Codex parity. Converts the Codex
 * Responses request into the OpenAI chat shape (instructions -> system, the
 * message / function_call / function_call_output history -> chat messages,
 * `function` tools passed through), runs ONE provider step on the selected
 * primary agent (NO internal tool loop), and relays the result as Responses-API
 * events: either a message item (text) or function_call items that Codex
 * executes locally and feeds back as function_call_output next turn. The Codex
 * parser requires output_item.added before any delta, so every turn is framed
 * created -> item.added -> delta(s) -> item.done -> completed. Compute-then-chunk. */
/* Resolve the governance response toggle: config-store `modules.governance` (canonical) ->
 * deprecated env default. Cached legacy_config_read; keeps gw_stage_governance config-free. */
static int openai_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static int responses_stream_handler(const char *body, server_http_sse_event_emit emit,
                                    void *context)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid Responses request", id[64], frame[2048];
   long created = (long)time(NULL);
   const char *model = "";
   char *model_copy = NULL;
   if (responses_mint_id(created, id, sizeof id))
   {
      openai_format_error(frame, sizeof frame, "server_error",
                          "response identity could not be created");
      emit(context, "error", frame);
      return 0;
   }
   int admitted = responses_admit_request(body, &request, error, sizeof error) == 0;
   model = request.model ? request.model : "";
   if (openai_format_responses_created(id, model, created, frame, sizeof frame) > 0)
      emit(context, "response.created", frame);
   if (!admitted)
      goto failed;
   agent_t agent;
   if (stream_pick_agent(&agent, model))
   {
      snprintf(error, sizeof error, "the requested model is not available");
      goto failed;
   }
   model_copy = strdup(model[0] ? model : agent.model);
   if (!model_copy)
      goto failed;
   model = model_copy;
   if (agent_execute_ir_request(&agent, &request, 0, -1, &response, error, sizeof error, 0))
      goto failed;
   responses_account(&agent, model, &response);
   if (responses_store_history(id, &request, &response))
   {
      snprintf(error, sizeof error, "durable Responses history could not be committed");
      goto failed;
   }
   if (openai_ir_responses_emit(&response, id, model, created, emit, context))
   {
      snprintf(error, sizeof error, "canonical response rendering failed");
      goto failed;
   }
   goto done;
failed:
   if (openai_format_responses_failed(id, model, created,
                                      admitted ? "server_error" : "invalid_request_error", error,
                                      frame, sizeof frame) > 0)
      emit(context, "response.failed", frame);
done:
   free(model_copy);
   aimee_response_free(&response);
   aimee_request_free(&request);
   return 0;
}

/* Async /v1/runs execution. POST enqueues a background worker and returns the
 * `queued` run immediately; the worker runs inference off the listener thread,
 * publishes live events + status into the runs store, and honors cancellation
 * at the step boundary. There is no provider-incremental token streaming yet, so
 * output deltas are emitted in one batch once the (blocking) step returns —
 * still off-thread, and preceded by a live `response.in_progress` event so
 * subscribers see progress before completion. */
#define RUN_JSON_CAP (256 * 1024)
#define RUN_DELTA    80

typedef struct
{
   char run_id[64];
   char model[64];
   char *prompt; /* heap; owned by the worker, freed there */
   double temperature;
   int max_tokens;
   long created;
   /* Request context captured at enqueue time (#3): the worker runs on a
    * separate thread, so the request-thread's context is snapshotted here and
    * re-established on the worker before the run, giving the run's audit rows the
    * originating request id / principal / source. */
   request_context_t reqctx;
   int has_reqctx;
} run_job_t;

/* Build a run object with no output text at the given status into buf. Returns
 * the pointer (buf) on success or "{}" if it does not fit. */
static const char *run_status_json(const run_job_t *j, const char *status, char *buf, int cap)
{
   return openai_format_run(j->run_id, j->model, "", j->created, 0, 0, 0, status, buf, cap) > 0
              ? buf
              : "{}";
}

/* Tool-event hook for /v1/runs: append a `tool_call.started` /
 * `tool_call.completed` event (with the tool name) to the run's event stream as
 * each tool fires during the (blocking) turn. ud is the run_id. */
static void runs_tool_event_cb(const char *phase, const char *name, void *ud)
{
   const char *run_id = (const char *)ud;
   char ev[48];
   snprintf(ev, sizeof(ev), "tool_call.%s", phase ? phase : "");
   cJSON *f = cJSON_CreateObject();
   cJSON_AddStringToObject(f, "type", ev);
   cJSON_AddStringToObject(f, "name", name ? name : "");
   char *frame = cJSON_PrintUnformatted(f);
   if (frame)
   {
      openai_runs_store_append_event(run_id, ev, frame);
      free(frame);
   }
   cJSON_Delete(f);
}

static void *run_job_worker(void *arg)
{
   run_job_t *j = (run_job_t *)arg;
   char *buf = (char *)malloc(RUN_JSON_CAP);
   if (!buf)
   {
      free(j->prompt);
      free(j);
      return NULL;
   }

   /* Cancelled before we even start. */
   if (openai_runs_store_cancel_requested(j->run_id))
   {
      openai_runs_store_append_event(j->run_id, "response.completed",
                                     run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_CANCELLED,
                                 run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      goto done;
   }

   openai_runs_store_set_status(j->run_id, OPENAI_RUN_IN_PROGRESS);
   openai_runs_store_update_json(j->run_id, run_status_json(j, "in_progress", buf, RUN_JSON_CAP));
   openai_runs_store_append_event(j->run_id, "response.in_progress",
                                  run_status_json(j, "in_progress", buf, RUN_JSON_CAP));

   /* Registry selection validates the requested model, but the tool loop
    * routes using a complete configuration. Load it before handing it off. */
   agent_config_t acfg = {0};
   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, j->model) == 0 ? &agbuf : NULL;
   if (agent_load_config(&acfg) != 0 || !ag)
   {
      openai_runs_store_append_event(j->run_id, "error", "{\"error\":\"no agent configured\"}");
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 run_status_json(j, "failed", buf, RUN_JSON_CAP));
      goto done;
   }

   /* A worker cannot borrow session_id()'s process-parent fallback: the
    * governed-action journal requires a durable session owned by this caller.
    * Allocate independently per run, including concurrent callers. */
   if (j->has_reqctx && j->reqctx.principal[0])
   {
      char sid[33];
      if (platform_random_hex(sid, 32) != 0 ||
          db1_server_session_create(sid, "openai-run", j->reqctx.principal) != 0)
      {
         openai_runs_store_append_event(j->run_id, "error",
                                        "{\"error\":\"run session owner unavailable\"}");
         openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                    run_status_json(j, "failed", buf, RUN_JSON_CAP));
         goto done;
      }
      session_id_set_override(sid);
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* /v1/runs is the agentic endpoint (vs. the stateless /v1/chat/completions):
    * run tool-enabled so the run can use aimee's tools and emit tool_call events
    * (AC #7). agent_run_with_tools self-preps the provider catalog and drives the
    * tool loop; the tool-event hook fires from dispatch_tool_call_ctx within it.
    * (ag was validated above as a configured agent; agent_run_with_tools routes
    * by role within the same config — the model field is informational here.) */
   (void)ag;
   agent_tools_set_tool_event_cb(runs_tool_event_cb, (void *)j->run_id);
   /* Re-establish the originating request context on this worker thread (#3) so
    * the run's audit rows carry the request id / principal captured at enqueue.
    * Prefer the request-supplied source over the constant when the proxy stamped
    * one. */
   if (j->has_reqctx)
      request_context_set(&j->reqctx);
   /* /v1/runs drives the agent loop (which logs via agent_log_call); tag that
    * row as ingress so the run's spend is distinguishable from internal agent
    * execution, without writing a second row. */
   agent_set_ingress_source((j->has_reqctx && j->reqctx.source[0]) ? j->reqctx.source
                                                                   : "openai-ingress");
   int erc = agent_run_with_tools(&acfg, "execute", NULL, j->prompt, j->max_tokens, &result);
   agent_set_ingress_source("");
   if (j->has_reqctx)
   {
      ingress_preinject_finish_sources();
      request_context_clear();
   }
   agent_tools_set_tool_event_cb(NULL, NULL);

   /* Honor a cancel requested while the (blocking) step ran. */
   if (openai_runs_store_cancel_requested(j->run_id))
   {
      free(result.response);
      openai_runs_store_append_event(j->run_id, "response.completed",
                                     run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_CANCELLED,
                                 run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      goto done;
   }

   if (erc != 0 || !result.response)
   {
      /* Owner/provider diagnostics can contain quotes, newlines and Unicode.
       * Serialize the complete string rather than interpolating/truncating JSON. */
      cJSON *failure = cJSON_CreateObject();
      cJSON_AddStringToObject(failure, "error", result.error[0] ? result.error : "run failed");
      char *failure_json = cJSON_PrintUnformatted(failure);
      openai_runs_store_append_event(j->run_id, "error",
                                     failure_json ? failure_json : "{\"error\":\"run failed\"}");
      free(failure_json);
      cJSON_Delete(failure);
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 run_status_json(j, "failed", buf, RUN_JSON_CAP));
      free(result.response);
      goto done;
   }

   /* Success: emit the output as delta events, then a terminal completed frame. */
   {
      char item_id[160];
      snprintf(item_id, sizeof(item_id), "%s-msg", j->run_id);
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n; off += RUN_DELTA)
      {
         char seg[RUN_DELTA + 1];
         size_t take = (n - off < RUN_DELTA) ? (n - off) : RUN_DELTA;
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         char frame[RUN_DELTA * 6 + 256];
         if (openai_format_responses_delta(item_id, seg, frame, sizeof(frame)) > 0)
            openai_runs_store_append_event(j->run_id, "response.output_text.delta", frame);
      }
      /* message.completed: the full assistant message is now available (the AC's
       * terminal message event, distinct from the run-level response.completed). */
      cJSON *mc = cJSON_CreateObject();
      cJSON_AddStringToObject(mc, "type", "message.completed");
      cJSON_AddStringToObject(mc, "item_id", item_id);
      cJSON *msg = cJSON_AddObjectToObject(mc, "message");
      cJSON_AddStringToObject(msg, "role", "assistant");
      cJSON_AddStringToObject(msg, "content", txt);
      char *mframe = cJSON_PrintUnformatted(mc);
      if (mframe)
      {
         openai_runs_store_append_event(j->run_id, "message.completed", mframe);
         free(mframe);
      }
      cJSON_Delete(mc);
   }
   if (openai_format_run(j->run_id, j->model, result.response, j->created, result.prompt_tokens,
                         result.completion_tokens, result.cache_read_tokens, "completed", buf,
                         RUN_JSON_CAP) > 0)
   {
      openai_runs_store_append_event(j->run_id, "response.completed", buf);
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_COMPLETED, buf);
   }
   else
   {
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 "{\"object\":\"run\",\"status\":\"failed\"}");
   }
   free(result.response);

done:
   session_id_clear_override();
   free(buf);
   free(j->prompt);
   free(j);
   return NULL;
}

/* POST /v1/runs: parse the request, create a `queued` run, spawn the background
 * worker, and return the queued run object immediately (the listener thread is
 * never blocked on inference). GET /v1/runs/{id}/events streams the worker's
 * live events; POST /v1/runs/{id}/stop requests cancellation. */
static int runs_handler(const char *body, char *resp, int cap)
{
   char model[64] = "";
   char prev_id[128] = "";
   char *prompt = NULL;
   int stream = 0;
   if (openai_parse_responses_request(body, model, sizeof(model), &prompt, prev_id, sizeof(prev_id),
                                      &stream) != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          "invalid run request: expected non-empty `input`");
      return 400;
   }

   /* Validate an agent is configured synchronously so callers still get 503 fast. */
   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      free(prompt);
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = responses_request_max_tokens(body);

   long created = (long)time(NULL);
   static atomic_ulong g_run_seq = 0; /* connections run concurrently; atomic */
   char id[64];
   snprintf(id, sizeof(id), "run_%ld_%lu", created, atomic_fetch_add(&g_run_seq, 1) + 1);

   /* Queued snapshot returned to the caller and stored for GET /v1/runs/{id}. */
   int len = openai_format_run(id, model, "", created, 0, 0, 0, "queued", resp, cap);
   if (len < 0)
   {
      free(prompt);
      openai_format_error(resp, cap, "server_error", "run did not fit the buffer");
      return 500;
   }
   openai_runs_store_create(id, resp);

   run_job_t *j = (run_job_t *)calloc(1, sizeof(*j));
   if (!j)
   {
      free(prompt);
      openai_runs_store_finalize(id, OPENAI_RUN_FAILED, resp);
      return 200; /* resp still holds the queued object */
   }
   snprintf(j->run_id, sizeof(j->run_id), "%s", id);
   snprintf(j->model, sizeof(j->model), "%s", model);
   j->prompt = prompt; /* ownership moves to the worker */
   j->temperature = temperature;
   j->max_tokens = max_tokens;
   j->created = created;
   /* Snapshot the request context now, on the request thread, so the worker can
    * re-establish it (capture-at-enqueue, #3). */
   {
      const request_context_t *rc = request_context_get();
      if (rc)
      {
         j->reqctx = *rc;
         j->has_reqctx = 1;
      }
   }

   pthread_t th;
   if (pthread_create(&th, NULL, run_job_worker, j) != 0)
   {
      free(j->prompt);
      free(j);
      openai_runs_store_finalize(id, OPENAI_RUN_FAILED, resp);
      return 200;
   }
   pthread_detach(th);
   return 200; /* resp already holds the queued run object */
}

void openai_chat_register(void)
{
   server_http_set_chat_handler(chat_completions_handler);
   server_http_set_runs_handler(runs_handler);
   server_http_set_chat_stream_handler(chat_stream_handler);
   server_http_set_completion_stream_handler(completion_stream_handler);
   server_http_set_responses_stream_handler(responses_stream_handler);
   server_http_set_completion_handler(completions_handler);
   server_http_set_embeddings_handler(embeddings_handler);
   server_http_set_responses_handler(responses_handler);
   server_http_set_models_provider(models_provider);
   server_http_set_models_raw_provider(codex_models_raw);
}
