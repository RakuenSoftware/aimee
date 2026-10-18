/* anthropic_http.c: Anthropic Messages API ingress (POST /v1/messages).
 *
 * Registered into the server_http dispatch via server_http_set_messages_*
 * handlers at startup (anthropic_http_register). Lets Claude Code — which speaks
 * only the Anthropic Messages API and picks its endpoint from ANTHROPIC_BASE_URL
 * — drive the registered agent named by the standard `model` field. `aimee` or
 * an omitted model selects the configured primary.
 *
 * This is a STATELESS wire-format proxy: it does NOT run aimee's agent loop,
 * memory, persona, or toolset (those would corrupt the context Claude Code
 * builds). It resolves the primary agent, translates the request to the
 * provider's OpenAI-compatible wire format (see anthropic_ingress.c), makes a
 * RAW provider call (build_url + resolve_auth + agent_http_post[_stream], the
 * same primitives the agent runtime uses, minus the loop), and translates the
 * reply back into Anthropic shape. The client owns system/messages/tools and
 * executes tools itself. */
#include "aimee.h" /* size macros for agent_types.h */
#include "agent_config.h"
#include "aimee_errors.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include "agent_types.h"
#include "anthropic_ingress.h"
#include "cJSON.h"
#include <aimee/delegates/delegate_driver.h>
#include "wire_fence.h"
#include "http_retry.h"
#include "gateway_mutate_wire.h"
#include "server_http_identity.h"
#include <aimee/gateway/gateway_policy.h>
#include "gw_stage_governance.h"
#include "gw_stage_completion.h"
#include "agent_response_projection.h"
#include <aimee/ir/session_event.h>
#include "router_advise.h"   /* Canonical request routing. */
#include "aimee_ir_shadow.h" /* Read-only diagnostic observer. */
#include <aimee/ir/aimee_ir_metrics.h>
#include "aimee_ir_serve.h" /* Slice 5: IR live request-build */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h> /* Slice 3: openai_backend_parse (IR response path) */
#include <aimee/translation/aimee_ir_stream.h> /* Slice 5-wire: IR-delta incremental relay */
#include "ingress_preinject.h"
#include "json_fluent.h"
#include "log.h"
#include "server_http.h"
#include "session_compact.h"
#include "sse_parser.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Apply the same standard `model` selector as OpenAI ingress. A concrete value
 * names one registered agent exactly; only `aimee`/empty uses the default. */
static int resolve_requested_agent(agent_t *out, const char *model)
{
   return agent_registry_resolve_ingress_model(model, out);
}

/* Mint a "msg_<epoch>" id for the response/stream. */
static void mint_msg_id(char *buf, size_t n)
{
   snprintf(buf, n, "msg_%ld", (long)time(NULL));
}

/* Passthrough vs. translate is a property of the serving model, not a setting:
 * when the primary speaks the Anthropic Messages API natively (the anthropic
 * driver), the /v1/messages ingress is a transparent passthrough — inbound model
 * honored, pre-injection skipped, client anthropic-version/anthropic-beta
 * forwarded, count_tokens proxied upstream, upstream error status/body +
 * Retry-After relayed unchanged. When the primary speaks OpenAI, the request is
 * translated (and the model swapped to the configured one). Derived below from
 * driver_is_anthropic(); there is no config flag. */

/* Retry-After (seconds) the parity passthrough relays on its own response when it
 * forwarded an upstream 429/529 that carried one. Thread-local, reset on every
 * request by anthropic_http_capture_request_headers (so it never leaks across
 * requests or onto a non-ingress route on a reused worker thread); read by
 * send_response in server_http_response.inc via anthropic_http_response_retry_after. */
static __thread int g_response_retry_after = 0;

/* Append a header line to `buf`, inserting a '\n' separator (mirrors
 * agent_config.c's append_header_line; the upstream HTTP layer splits on '\n'). */
static void hdr_line(char *buf, size_t n, const char *line)
{
   size_t used;
   if (!buf || n == 0 || !line || !line[0])
      return;
   used = strlen(buf);
   if (used > 0 && buf[used - 1] != '\n' && used + 1 < n)
   {
      buf[used++] = '\n';
      buf[used] = '\0';
   }
   if (used < n - 1)
      snprintf(buf + used, n - used, "%s", line);
}

/* Build the upstream header block for the anthropic parity passthrough: forward
 * the client's anthropic-version (else the GA default) and its full anthropic-beta
 * set verbatim, so beta-gated behaviors (fine-grained tool streaming, 1h cache
 * TTL, interleaved thinking, fast mode, …) reach Anthropic exactly as Claude Code
 * requested them. */
static void build_anthropic_parity_headers(char *buf, size_t n)
{
   char line[640];
   const char *cver = anthropic_ingress_request_version();
   const char *cbeta = anthropic_ingress_request_beta();

   buf[0] = '\0';
   snprintf(line, sizeof(line), "anthropic-version: %s", (cver && cver[0]) ? cver : "2023-06-01");
   hdr_line(buf, n, line);
   if (cbeta && cbeta[0])
   {
      snprintf(line, sizeof(line), "anthropic-beta: %s", cbeta);
      hdr_line(buf, n, line);
   }
}

/* Serialize `obj` into resp (bounded by cap). Returns 200 on success, 500 if it
 * does not fit. Consumes neither — caller still owns obj. */
static int write_json(cJSON *obj, char *resp, int cap)
{
   char *out = cJSON_PrintUnformatted(obj);
   int rc = 500;
   if (out && (int)strlen(out) < cap)
   {
      memcpy(resp, out, strlen(out) + 1);
      rc = 200;
   }
   else
   {
      snprintf(resp, cap,
               "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
               "\"message\":\"response did not fit the buffer\"}}");
   }
   free(out);
   return rc;
}

/* Write an Anthropic-shaped error object into resp and return the wire `status`.
 * aimee_code is an aimee-specific error code (>=1000, see aimee_errors.h) when the
 * fault is aimee's own — it rides in the body's error.code and is logged — or 0
 * for a plain HTTP-semantic error (client 4xx, genuine upstream 5xx). The wire
 * status stays a standard 3-digit code regardless; the 4-digit aimee code never
 * touches the status line. */
static int write_error(char *resp, int cap, int status, const char *type, const char *message,
                       int aimee_code)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *e;
   cJSON_AddStringToObject(o, "type", "error");
   e = cJSON_AddObjectToObject(o, "error");
   cJSON_AddStringToObject(e, "type", type);
   cJSON_AddStringToObject(e, "message", message);
   if (aimee_code > 0)
   {
      cJSON_AddNumberToObject(e, "code", aimee_code);
      LOG_WARN("ingress", "aimee_err=%d (%s) status=%d: %s", aimee_code, aimee_err_slug(aimee_code),
               status, message ? message : "");
   }
   char *out = cJSON_PrintUnformatted(o);
   if (out)
   {
      snprintf(resp, cap, "%s", out);
      free(out);
   }
   cJSON_Delete(o);
   return status;
}

static int driver_is_anthropic(const delegate_driver_t *driver)
{
   return driver && driver->name && strcmp(driver->name, "anthropic") == 0;
}

/* Provider wire for the response-side IR shadow: compare_response re-parses the
 * provider reply through this backend, so it must match the wire the legacy path
 * parsed. driver_is_anthropic() is NULL-safe, so a NULL/absent driver yields
 * OpenAI-chat -- exactly the no-driver legacy branch, which parses with
 * agent_ir_parse_json_response with the openai wire (0). */
static aimee_wire_t shadow_provider_wire(const delegate_driver_t *driver)
{
   return driver_is_anthropic(driver) ? AIMEE_WIRE_ANTHROPIC : AIMEE_WIRE_OPENAI_CHAT;
}

/* Native response decoding owns the full typed result until policy and the
 * explicit client render boundary finish. Invalid frames never use a fallback. */
static int messages_decode_response(const char *frame, const delegate_driver_t *driver,
                                    int responses_wire, const aimee_anthropic_replay_t *replay,
                                    aimee_response_t *out)
{
   char error[160] = "";
   aimee_wire_t wire = responses_wire ? AIMEE_WIRE_RESPONSES : shadow_provider_wire(driver);
   if (aimee_backend_parse_wire(frame, frame ? strlen(frame) : 0, wire, out, error, sizeof error))
      return -1;
   if (wire == AIMEE_WIRE_ANTHROPIC && (!replay || aimee_anthropic_bind_response(replay, out)))
   {
      aimee_ir_session_fail_capture();
      return -1;
   }
   if (aimee_ir_session_emit_buffered_response(out, NULL))
      return -1;
   aimee_message_t message = {
       .role = "assistant", .blocks = out->content, .n_blocks = out->n_content};
   aimee_request_t view = {.messages = &message, .n_messages = 1};
   aimee_wire_projection_report_t projection;
   aimee_backend_projection_report(&view, AIMEE_WIRE_ANTHROPIC, &projection);
   if (aimee_ir_session_emit_projection(out->model, projection.omitted_reasoning_blocks,
                                        projection.omitted_opaque_state_blocks, "response"))
      return -1;
   if (projection.omitted_reasoning_blocks)
      LOG_INFO(
          "translation.projection",
          "wire=anthropic omitted_reasoning=%d omitted_opaque_state=%d canonical_response=retained",
          projection.omitted_reasoning_blocks, projection.omitted_opaque_state_blocks);
   return out->error_message ? -1 : 0;
}

static const char *messages_stop_reason(const aimee_response_t *response)
{
   return response->stop_reason == AIMEE_STOP_UNKNOWN && response->raw_stop_reason
              ? response->raw_stop_reason
              : aimee_stop_reason_name(response->stop_reason);
}

static cJSON *messages_render_response(const aimee_response_t *response, const char *id,
                                       const char *model)
{
   aimee_response_t view = *response;
   view.id = (char *)id;
   view.model = (char *)model;
   return anthropic_frontend_render(&view);
}

static int messages_emit_json(server_http_sse_event_emit emit, void *context, const char *event,
                              cJSON *payload)
{
   char *json = payload ? cJSON_PrintUnformatted(payload) : NULL;
   cJSON_Delete(payload);
   if (!json)
      return -1;
   if (emit)
      emit(context, event, json);
   free(json);
   return 0;
}

/* Buffered native output is replayed from typed blocks, without a fixed tool
 * array, truncated IDs, or flattening signed thinking into answer text. */
static int messages_emit_response(const aimee_response_t *response, const char *id,
                                  const char *model, server_http_sse_event_emit emit, void *context)
{
   anthropic_stream_state_t state = {0};
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START,
                          .usage_in = response->usage_in,
                          .usage_cache_read = response->usage_cache_read,
                          .usage_cache_write = response->usage_cache_write};
   if (anthropic_delta_emit(&delta, &state, id, model, emit, context) <= 0)
      return -1;
   int block_index = 0;
   for (int i = 0; i < response->n_content; i++)
   {
      const aimee_block_t *block = &response->content[i];
      if (!anthropic_ir_block_replayable(block))
         continue;
      delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_START,
                              .block_id = block_index,
                              .kind = block->type,
                              .tool_id = block->tool_id,
                              .tool_name = block->tool_name,
                              .redacted_content = block->thinking_redacted};
      if (block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT)
      {
         cJSON *start = cJSON_CreateObject();
         cJSON *native = anthropic_ir_render_block(block);
         if (!start || !native || !cJSON_AddStringToObject(start, "type", "content_block_start") ||
             !cJSON_AddNumberToObject(start, "index", block_index) ||
             !cJSON_AddItemToObject(start, "content_block", native))
         {
            cJSON_Delete(start);
            cJSON_Delete(native);
            return -1;
         }
         if (messages_emit_json(emit, context, "content_block_start", start))
            return -1;
      }
      else
      {
         if (anthropic_delta_emit(&delta, &state, id, model, emit, context) <= 0)
            return -1;
         delta.type = AIMEE_DELTA_BLOCK_DELTA;
         char *arguments = NULL;
         if (block->type == AIMEE_BLK_TOOL_USE)
         {
            arguments = cJSON_PrintUnformatted(block->tool_input);
            if (!arguments)
               return -1;
            delta.tool_args_delta = arguments;
         }
         else
            delta.text_delta = block->text;
         int failed = !block->thinking_redacted &&
                      anthropic_delta_emit(&delta, &state, id, model, emit, context) <= 0;
         free(arguments);
         if (failed)
            return -1;
         if (block->type == AIMEE_BLK_THINKING && block->thinking_signature)
         {
            delta.text_delta = NULL;
            delta.signature_delta = block->thinking_signature;
            if (anthropic_delta_emit(&delta, &state, id, model, emit, context) <= 0)
               return -1;
         }
      }
      delta.type = AIMEE_DELTA_BLOCK_STOP;
      if (anthropic_delta_emit(&delta, &state, id, model, emit, context) <= 0)
         return -1;
      block_index++;
   }
   cJSON *last = cJSON_CreateObject(), *stop = last ? cJSON_AddObjectToObject(last, "delta") : NULL;
   cJSON *usage = last ? cJSON_AddObjectToObject(last, "usage") : NULL;
   if (!last || !stop || !usage || !cJSON_AddStringToObject(last, "type", "message_delta") ||
       !cJSON_AddStringToObject(stop, "stop_reason", messages_stop_reason(response)) ||
       !cJSON_AddNullToObject(stop, "stop_sequence") ||
       !cJSON_AddNumberToObject(usage, "output_tokens", (double)response->usage_out))
   {
      cJSON_Delete(last);
      return -1;
   }
   if (messages_emit_json(emit, context, "message_delta", last))
      return -1;
   last = cJSON_CreateObject();
   if (!last || !cJSON_AddStringToObject(last, "type", "message_stop"))
   {
      cJSON_Delete(last);
      return -1;
   }
   return messages_emit_json(emit, context, "message_stop", last);
}

/* Request policy consumes admitted canonical fields. Native JSON is retained
 * only as an immutable replay sidecar for the explicit output boundary. */
static int anthropic_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static cJSON *messages_ir_history_json(const aimee_request_t *request)
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
static int messages_ir_history_replace(aimee_request_t *request, const cJSON *history)
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
static int messages_ir_append_policy(aimee_request_t *request, char *policy)
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
static int messages_resolve_model(aimee_request_t *request, const agent_t *agent, int parity)
{
   int selected_alias =
       request->model && (!strcmp(request->model, "aimee") || !strcmp(request->model, agent->name));
   if (!parity || !request->model || !request->model[0] || selected_alias ||
       config_gateway_pin_model())
   {
      char *model = strdup(agent->model);
      if (!model)
         return -1;
      free(request->model);
      request->model = model;
   }
   return 0;
}

static int messages_prepare_request(const char *frame, const agent_t *agent, int parity,
                                    aimee_request_t *request, char *error, size_t capacity)
{
   if (aimee_frontend_parse_wire(frame, frame ? strlen(frame) : 0, AIMEE_WIRE_ANTHROPIC, request,
                                 error, capacity) ||
       aimee_ir_request_admit(request))
      return -1;
   if (messages_resolve_model(request, agent, parity))
      return -1;
   if (!parity)
   {
      request->max_tokens = agent_request_max_tokens(agent, request->max_tokens);
      request->has_max_tokens = request->max_tokens > 0;
      if (!request->has_temperature)
      {
         request->temperature = 1.0;
         request->has_temperature = 1;
      }
   }
   gateway_policy_apply_ir(request);
   router_advise_ir(request);
   if (messages_ir_append_policy(request, gw_ir_request_tool_system_prompt(request, NULL)) ||
       messages_ir_append_policy(request, gw_ir_request_completion_system_prompt(request, NULL)))
      return -1;
   if (parity)
      aimee_ir_apply_native_request_stages(request);
   else
      aimee_ir_apply_gateway_request_stages(request);
   return aimee_ir_request_admit(request);
}

static int messages_mutate_request(aimee_request_t *request, int parity, cJSON **container,
                                   gw_mutate_ctx_t *mutation)
{
   if (!gw_mutate_upstream_ok(parity))
      return 0;
   cJSON *history = messages_ir_history_json(request);
   *container = cJSON_CreateObject();
   if (!history || !*container || !cJSON_AddItemToObject(*container, "messages", history))
   {
      cJSON_Delete(history);
      return -1;
   }
   gw_buffered_mutate(*container, "messages", request->model, "",
                      server_http_identity_session_hdr(), server_http_identity_bearer(),
                      server_http_identity_principal(), mutation);
   return mutation->mutated ? messages_ir_history_replace(
                                  request, cJSON_GetObjectItemCaseSensitive(*container, "messages"))
                            : 0;
}

/* Approximation consumes canonical messages; it never converts history through
 * another model protocol. Actual provider usage/count responses take priority. */
static int messages_estimate_tokens(const aimee_request_t *request)
{
   if (!request)
      return 0;
   cJSON *messages = cJSON_CreateArray();
   if (!messages)
      return -1;
   if (request->n_system)
   {
      aimee_message_t system = {
          .role = "system", .blocks = request->system, .n_blocks = request->n_system};
      cJSON *message = aimee_ir_message_json(&system);
      if (!message || !cJSON_AddItemToArray(messages, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(messages);
         return -1;
      }
   }
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = aimee_ir_message_json(&request->messages[i]);
      if (!message || !cJSON_AddItemToArray(messages, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(messages);
         return -1;
      }
   }
   int tokens = session_compact_estimate_tokens(messages);
   cJSON_Delete(messages);
   return tokens;
}

/* Rendering never reimports a second native projection. */
static char *anthropic_build_prov_body(aimee_request_t *request, aimee_wire_t wire, int stream)
{
   request->stream = stream;
   return aimee_ir_request_admit(request) ? NULL : aimee_backend_render_wire(request, wire);
}

static int messages_buffered(const char *body, char *resp, int cap)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   agent_t agbuf;
   agent_t *ag;
   cJSON *out = NULL;
   aimee_request_t policy = {0};
   char policy_error[128];
   cJSON *mutation = NULL;
   char *prov_body = NULL, *response = NULL;
   char url[MAX_ENDPOINT_LEN + 64];
   char auth[MAX_API_KEY_LEN + 32];
   char extra[512];
   char msg_id[48];
   const delegate_driver_t *driver;
   aimee_response_t parsed = {0};
   wire_fence_t *wire_snapshot = NULL;
   gw_mutate_ctx_t gwmc;
   int status, http_status, rc;
   const char *model;

   gw_mutate_ctx_init(&gwmc);

   if (!req)
      return write_error(resp, cap, 400, "invalid_request_error", "invalid JSON body", 0);
   model = jo_cstr(req, "model");

   /* Reject the two shapes that otherwise HANG. The Anthropic Messages API requires
    * max_tokens and a non-empty messages array; without them this request reached
    * the provider path and never came back — an empty body, `{}`, `{"model":"x"}`
    * and `{"messages":[]}` all sat forever instead of answering. A caller that omits
    * a required field must get a 400, not an open socket.
    *
    * Validated here, before anything else is allocated, so the cleanup is just the
    * parsed body — the same shape as the no-primary-agent branch below. This bounds
    * the malformed-input case only; it does NOT put a deadline on the provider call
    * itself, which is a separate concern. */
   {
      const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
      if (!cJSON_IsNumber(jmax) || jmax->valuedouble < 1)
      {
         cJSON_Delete(req);
         return write_error(resp, cap, 400, "invalid_request_error",
                            "max_tokens is required and must be a positive integer", 0);
      }
      const cJSON *jmsgs = cJSON_GetObjectItemCaseSensitive(req, "messages");
      if (!cJSON_IsArray(jmsgs) || cJSON_GetArraySize(jmsgs) == 0)
      {
         cJSON_Delete(req);
         return write_error(resp, cap, 400, "invalid_request_error",
                            "messages must be a non-empty array", 0);
      }
   }

   /* SHADOW (Slice 3): observe the IR round-trip on this live request. No-op unless
    * AIMEE_IR_SHADOW is set; never affects the response. */
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC);

   ag = resolve_requested_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      /* `model` points into req. Classify it before deleting the JSON tree;
       * optimized builds otherwise read freed storage and can misreport an
       * explicit unknown model as a missing default agent (503 vs 404). */
      int explicit_model = model && model[0] && strcmp(model, "aimee") != 0;
      cJSON_Delete(req);
      if (explicit_model)
         return write_error(resp, cap, 404, "not_found_error",
                            "the requested model is not available", 0);
      return write_error(resp, cap, 503, "api_error", "no primary agent configured",
                         AIMEE_ERR_NO_PRIMARY);
   }

   delegate_drivers_init();
   driver = delegate_driver_get(ag->provider);
   /* Exact-parity passthrough only applies to the Anthropic-native path. */
   int parity = driver_is_anthropic(driver);
   if (messages_prepare_request(body, ag, parity, &policy, policy_error, sizeof policy_error))
   {
      status = write_error(resp, cap, 400, "invalid_request_error",
                           "cannot admit canonical provider request", 0);
      goto cleanup;
   }
   if (messages_mutate_request(&policy, parity, &mutation, &gwmc))
   {
      status = write_error(resp, cap, 500, "api_error", "invalid canonical economizer history", 0);
      goto cleanup;
   }
   if (delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
   {
      /* Internal: we never reached the provider — the endpoint or credentials for
       * the primary couldn't be resolved (e.g. a codex-oauth seat with no token).
       * 503 (not 502) + an aimee code so this doesn't read as an upstream failure;
       * surface the explicit auth reason (D6) when one was set. */
      const char *why = agent_request_auth_error();
      char msg[256];
      snprintf(msg, sizeof(msg),
               "could not resolve endpoint or credentials for primary agent '%s'%s%s", ag->name,
               (why && why[0]) ? ": " : "", (why && why[0]) ? why : "");
      status = write_error(resp, cap, 503, "api_error", msg, AIMEE_ERR_ROUTE_UNRESOLVED);
      goto cleanup;
   }
   /* Codex/Responses (chatgpt wire) replies with raw SSE even to a buffered fetch
    * and requires stream=true; asking it for stream=false yields an empty body that
    * cJSON_Parse can't read, so the buffered reply came back empty. Keyed off the
    * resolved upstream shape (like the streaming path), not the provider label. */
   int responses_wire = strstr(url, "/responses") != NULL;
   if (parity)
      build_anthropic_parity_headers(extra, sizeof(extra));
   else
      agent_build_extra_headers(ag, extra, sizeof(extra));

   wire_fence_route_t wire_route = parity           ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                   : responses_wire ? WIRE_FENCE_OPENAI_RESPONSES
                                                    : WIRE_FENCE_OPENAI_CHAT;
   aimee_wire_t wire = parity           ? AIMEE_WIRE_ANTHROPIC
                       : responses_wire ? AIMEE_WIRE_RESPONSES
                                        : AIMEE_WIRE_OPENAI_CHAT;
   aimee_anthropic_replay_t replay = {0};
   for (int attempt = 0; attempt < 2; attempt++)
   {
      free(prov_body);
      prov_body = anthropic_build_prov_body(&policy, wire, responses_wire);
      if (!prov_body)
      {
         status = write_error(resp, cap, 500, "api_error",
                              "could not assemble canonical provider request", 0);
         goto cleanup;
      }
      wire_fence_destroy(wire_snapshot);
      wire_snapshot = NULL;
      wire_fence_bytes_t wire_body;
      if (wire_fence_select(econ_mode_current() != ECON_MODE_OFF, wire_route, prov_body,
                            strlen(prov_body), &wire_snapshot, &wire_body))
      {
         const char *error = wire_fence_last_error();
         status = write_error(resp, cap, wire_fence_error_http_status(error),
                              wire_fence_error_type(error), error, AIMEE_ERR_REQUEST_PIPELINE);
         goto cleanup;
      }
      if (wire == AIMEE_WIRE_ANTHROPIC &&
          aimee_anthropic_replay_prepare(wire_body.data, wire_body.len, &replay))
      {
         status = write_error(resp, cap, 500, "api_error", "cannot bind native replay prefix",
                              AIMEE_ERR_REQUEST_PIPELINE);
         goto cleanup;
      }
      http_status = wire_fence_post(url, auth, wire_body.data, wire_body.len, &response,
                                    ag->timeout_ms, extra[0] ? extra : NULL, 1, 0, 0, ag->provider,
                                    ag->model, NULL, wire_route);
      if (http_status == HTTP_RETRY_ADMISSION_REFUSED)
      {
         const char *error = wire_fence_last_error();
         status = write_error(resp, cap, wire_fence_error_http_status(error),
                              wire_fence_error_type(error), error, AIMEE_ERR_REQUEST_PIPELINE);
         goto cleanup;
      }
      if (attempt || !mutation ||
          gw_buffered_after_status(mutation, "messages", http_status, &gwmc) != GW_POST_RESEND)
         break;
      if (messages_ir_history_replace(&policy,
                                      cJSON_GetObjectItemCaseSensitive(mutation, "messages")))
      {
         status = write_error(resp, cap, 500, "api_error", "invalid restored canonical history", 0);
         goto cleanup;
      }
      free(response);
      response = NULL;
   }

   if (http_status != 200 || !response)
   {
      /* Exact parity: relay the upstream status + Anthropic error body verbatim
       * so Claude Code sees the real error type (rate_limit_error,
       * overloaded_error, …) and status code, not a wrapped 502. Also relay the
       * upstream Retry-After (send_response emits it on our response) so the
       * SDK's backoff matches api.anthropic.com on a 429/529. */
      if (parity && response && (int)strlen(response) < cap)
      {
         memcpy(resp, response, strlen(response) + 1);
         status = http_status;
         g_response_retry_after = agent_http_last_retry_after();
      }
      else
      {
         /* Genuine upstream failure: the provider was reached and returned
          * non-200. 502 with no aimee code — this really is an external problem. */
         status = write_error(resp, cap, 502, "api_error",
                              response ? response : "primary provider call failed", 0);
      }
      goto cleanup;
   }

   if (messages_decode_response(response, driver, responses_wire, &replay, &parsed) ||
       agent_response_govern(&parsed, anthropic_governance_enabled(),
                             gateway_prevent_subagents_enabled(), NULL, 0) < 0 ||
       gw_ir_response_run_completion(&parsed, &policy) < 0)
   {
      status = write_error(resp, cap, 502, "api_error", "invalid canonical provider response", 0);
      goto cleanup;
   }
   if (agent_ingress_accounting_enabled())
      agent_ingress_record_cost(ag->name, ag->model, model, messages_stop_reason(&parsed),
                                (int)parsed.usage_in, (int)parsed.usage_out,
                                (int)parsed.usage_cache_write, (int)parsed.usage_cache_read,
                                "anthropic-ingress", NULL);

   mint_msg_id(msg_id, sizeof(msg_id));
   out = messages_render_response(&parsed, msg_id, model);
   rc = write_json(out, resp, cap);
   status = rc;

cleanup:
   aimee_response_free(&parsed);
   wire_fence_destroy(wire_snapshot);
   gw_mutate_ctx_free(&gwmc);
   cJSON_Delete(out);
   free(response);
   free(prov_body);
   aimee_request_free(&policy);
   cJSON_Delete(mutation);
   cJSON_Delete(req);
   return status;
}

/* --- Streaming: POST /v1/messages (stream:true) ------------------------- */

/* Per-call streaming context: an SSE line parser over the provider's raw bytes,
 * feeding each `data:` payload into the Anthropic stream translator. */
typedef struct
{
   sse_parser_t parser;
   openai_stream_state_t ir_ost;
   anthropic_stream_state_t ir_ast;
   server_http_sse_event_emit emit;
   void *emit_ctx;
   const char *msg_id;
   const char *model;
   long ir_usage_out; /* tapped from the TURN_STOP delta for the cost row */
   int ir_failed;
} prov_stream_ctx_t;

typedef struct
{
   sse_parser_t parser;
   server_http_sse_event_emit emit;
   void *emit_ctx;
   char event[64];
   char *data;
   size_t data_len, data_cap;
   int emitted, saw_invalid_request, failed, saw_reasoning;
   anthropic_backend_stream_state_t backend;
   anthropic_stream_state_t frontend;
   aimee_ir_response_builder_t assembled;
   aimee_anthropic_replay_t replay;
   const char *model;
   char *message_id;
} native_ir_stream_ctx_t;

static int messages_publish_delta(const char *message_id, const aimee_delta_t *delta)
{
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA, .role = "assistant", .delta = delta};
   event.message_id = message_id && message_id[0] ? message_id : "native";
   return aimee_ir_session_emit(&event);
}

static int prov_line_cb(const char *line, size_t len, void *ud)
{
   prov_stream_ctx_t *c = (prov_stream_ctx_t *)ud;
   if (len >= 5 && strncmp(line, "data:", 5) == 0)
   {
      const char *p = line + 5;
      while (*p == ' ')
         p++;
      {
         if (strcmp(p, "[DONE]") == 0)
         {
            aimee_delta_t terminal;
            int n = openai_stream_finish(&c->ir_ost, &terminal, 1);
            if (n < 0)
            {
               c->ir_failed = 1;
               return -1;
            }
            if (n)
            {
               if (messages_publish_delta(c->msg_id, &terminal))
               {
                  c->ir_failed = 1;
                  return -1;
               }
               c->ir_usage_out = terminal.usage_out;
               anthropic_delta_emit(&terminal, &c->ir_ast, c->msg_id, c->model, c->emit,
                                    c->emit_ctx);
            }
            return 0;
         }
         cJSON *chunk = cJSON_Parse(p);
         if (chunk)
         {
            /* A finish chunk closes EVERY open block (reasoning + text + up to
             * AIMEE_STREAM_MAX_TOOLS tool blocks) then TURN_STOP in one call, and
             * a chunk carrying many tool_calls opens as many; size the buffer to
             * hold the worst case so the terminal TURN_STOP is never truncated
             * (which would hang the client's SSE reader). The +8 headroom covers a
             * reasoning block's START/DELTA/STOP on top of the text + tool worst
             * case, since one chunk may carry reasoning and content together. */
            aimee_delta_t deltas[3 * AIMEE_STREAM_MAX_TOOLS + 8];
            int n = openai_chunk_to_deltas(chunk, &c->ir_ost, deltas,
                                           (int)(sizeof deltas / sizeof deltas[0]));
            if (n < 0)
            {
               c->ir_failed = 1;
               cJSON_Delete(chunk);
               return -1;
            }
            for (int i = 0; i < n; i++)
            {
               if (messages_publish_delta(c->msg_id, &deltas[i]))
               {
                  c->ir_failed = 1;
                  cJSON_Delete(chunk);
                  return -1;
               }
               if (deltas[i].type == AIMEE_DELTA_TURN_STOP)
                  c->ir_usage_out = deltas[i].usage_out;
               anthropic_delta_emit(&deltas[i], &c->ir_ast, c->msg_id, c->model, c->emit,
                                    c->emit_ctx);
            }
            cJSON_Delete(chunk);
         }
         else
         {
            c->ir_failed = 1;
            return -1;
         }
      }
   }
   return 0;
}

static int prov_chunk_cb(const char *data, size_t len, void *ud)
{
   prov_stream_ctx_t *c = (prov_stream_ctx_t *)ud;
   return sse_parser_feed(&c->parser, data, len, prov_line_cb, c);
}

static int native_ir_append_data(native_ir_stream_ctx_t *c, const char *data)
{
   size_t add = strlen(data), sep = c->data_len ? 1 : 0;
   /* Bound an individual native frame before decoding it. */
   if (add + sep > 16 * 1024 * 1024 || c->data_len > 16 * 1024 * 1024 - add - sep)
      return -1;
   size_t need = c->data_len + sep + add + 1;
   if (need > c->data_cap)
   {
      size_t cap = c->data_cap ? c->data_cap : 4096;
      while (cap < need)
         cap *= 2;
      char *tmp = realloc(c->data, cap);
      if (!tmp)
         return -1;
      c->data = tmp;
      c->data_cap = cap;
   }
   if (sep)
      c->data[c->data_len++] = '\n';
   memcpy(c->data + c->data_len, data, add + 1);
   c->data_len += add;
   return 0;
}

static int native_ir_flush(native_ir_stream_ctx_t *c)
{
   if (c->failed)
      return -1;
   if (!c->data_len)
   {
      c->event[0] = 0;
      return 0;
   }
   const char *end = NULL;
   cJSON *payload = cJSON_ParseWithLengthOpts(c->data, c->data_len, &end, 0);
   if (end)
      while (end < c->data + c->data_len &&
             (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
         end++;
   const char *type = jo_cstr(payload, "type");
   const char *event = c->event[0] ? c->event : type;
   aimee_delta_t delta;
   int n = payload && aimee_ir_json_unique(payload) && end == c->data + c->data_len &&
                   (!type[0] || !strcmp(type, event))
               ? anthropic_stream_to_deltas(event, payload, &c->backend, &delta, 1)
               : -1;
   if (gw_stream_anthropic_error_is_invalid_request(c->data))
      c->saw_invalid_request = 1;
   if (n > 0 && delta.type == AIMEE_DELTA_TURN_START)
   {
      const cJSON *message = cJSON_GetObjectItemCaseSensitive(payload, "message");
      const char *id = jo_cstr(message, "id");
      c->message_id = strdup(id[0] ? id : "native");
      if (!c->message_id)
         n = -1;
   }
   if (n > 0 && aimee_ir_response_builder_apply(&c->assembled, &delta))
      n = -1;
   if (n > 0 && delta.type == AIMEE_DELTA_TURN_STOP)
   {
      if (aimee_anthropic_bind_response(&c->replay, &c->assembled.response))
         n = -1;
      if (n > 0)
      {
         aimee_message_t message = {.role = "assistant",
                                    .blocks = c->assembled.response.content,
                                    .n_blocks = c->assembled.response.n_content};
         aimee_session_event_t snapshot = {
             .kind = AIMEE_SESSION_MESSAGE, .message_id = c->message_id, .message = &message};
         if (aimee_ir_session_emit(&snapshot))
            n = -1;
      }
   }
   if (n > 0 && messages_publish_delta(c->message_id, &delta))
      n = -1;
   if (n > 0)
   {
      if (delta.kind == AIMEE_BLK_THINKING && (delta.text_delta || delta.redacted_content))
         c->saw_reasoning = 1;
      c->emitted +=
          anthropic_delta_emit(&delta, &c->frontend, c->message_id, c->model, c->emit, c->emit_ctx);
      if (delta.type == AIMEE_DELTA_ERROR)
         c->failed = 1;
   }
   if (n < 0)
      c->failed = 1;
   cJSON_Delete(payload);
   c->event[0] = 0;
   c->data_len = 0;
   c->data[0] = 0;
   return c->failed ? -1 : 0;
}

static int native_ir_line_cb(const char *line, size_t len, void *ud)
{
   native_ir_stream_ctx_t *c = ud;
   if (!len)
      return native_ir_flush(c);
   if (len >= 6 && !strncmp(line, "event:", 6))
   {
      const char *p = line + 6;
      if (*p == ' ')
         p++;
      if (strlen(p) >= sizeof c->event)
         return -1;
      snprintf(c->event, sizeof c->event, "%s", p);
   }
   else if (len >= 5 && !strncmp(line, "data:", 5))
   {
      const char *p = line + 5;
      if (*p == ' ')
         p++;
      return native_ir_append_data(c, p);
   }
   return 0;
}

static int native_ir_chunk_cb(const char *data, size_t len, void *ud)
{
   native_ir_stream_ctx_t *c = ud;
   int rc = sse_parser_feed(&c->parser, data, len, native_ir_line_cb, c);
   if (rc)
      c->failed = 1;
   return rc;
}

static void messages_stream_pipeline_error(server_http_sse_event_emit emit, void *ctx)
{
   const char *error = wire_fence_last_error();
   char frame[256];
   snprintf(frame, sizeof(frame),
            "{\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"%s\"}}",
            wire_fence_error_type(error), error);
   if (emit)
      emit(ctx, "error", frame);
}

/* P2c buffered-replay streaming: fetch the upstream reply to completion, police the
 * parsed struct, and replay it as a well-formed Anthropic SSE sequence. Used when
 * gateway_prevent_subagents is on, or the primary speaks the OpenAI Responses wire
 * (whose stream shape the incremental translator does not understand). Extracted
 * verbatim from messages_stream(); goto cleanup became return (the caller runs the
 * shared cleanup). */
static void messages_stream_buffered_replay(const char *url, const char *auth,
                                            const void *prov_body, size_t prov_body_len,
                                            const char *extra, agent_t *ag,
                                            const delegate_driver_t *driver, const char *model,
                                            const char *msg_id, int responses_wire,
                                            const aimee_request_t *policy,
                                            server_http_sse_event_emit emit, void *ctx)
{
   char *frame = NULL;
   aimee_response_t response = {0};
   wire_fence_route_t route = responses_wire                ? WIRE_FENCE_OPENAI_RESPONSES
                              : driver_is_anthropic(driver) ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                                            : WIRE_FENCE_OPENAI_CHAT;
   aimee_anthropic_replay_t replay = {0};
   if (route == WIRE_FENCE_ANTHROPIC_MESSAGES &&
       aimee_anthropic_replay_prepare(prov_body, prov_body_len, &replay))
   {
      aimee_delta_t failure = {.type = AIMEE_DELTA_ERROR,
                               .error_message = "cannot bind native replay prefix"};
      anthropic_stream_state_t state = {0};
      if (!messages_publish_delta(msg_id, &failure))
         anthropic_delta_emit(&failure, &state, msg_id, model, emit, ctx);
      return;
   }
   int status =
       wire_fence_post(url, auth, prov_body, prov_body_len, &frame, ag->timeout_ms,
                       extra[0] ? extra : NULL, 1, 0, 0, ag->provider, ag->model, NULL, route);
   if (status == HTTP_RETRY_ADMISSION_REFUSED)
      messages_stream_pipeline_error(emit, ctx);
   else if (status != 200 ||
            messages_decode_response(frame, driver, responses_wire, &replay, &response) ||
            agent_response_govern(&response, anthropic_governance_enabled(),
                                  gateway_prevent_subagents_enabled(), NULL, 0) < 0 ||
            gw_ir_response_run_completion(&response, policy) < 0 ||
            messages_emit_response(&response, msg_id, model, emit, ctx))
   {
      anthropic_stream_state_t state = {0};
      char reason[128];
      snprintf(reason, sizeof reason,
               status == 200 ? "invalid canonical provider response"
                             : "primary provider call failed with status %d",
               status);
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR, .error_message = reason};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
   }
   else if (agent_ingress_accounting_enabled())
      agent_ingress_record_cost(ag->name, ag->model, model, messages_stop_reason(&response),
                                (int)response.usage_in, (int)response.usage_out,
                                (int)response.usage_cache_write, (int)response.usage_cache_read,
                                "anthropic-ingress", NULL);
   aimee_response_free(&response);
   free(frame);
}

/* Native frames are admitted and persisted before the client renderer sees them. */
static int messages_stream_native_ir(const char *url, const char *auth, const void *prov_body,
                                     size_t prov_body_len, const char *extra, agent_t *ag,
                                     const char *model, server_http_sse_event_emit emit, void *ctx,
                                     int *invalid_frame_out)
{
   native_ir_stream_ctx_t stream = {.emit = emit, .emit_ctx = ctx, .model = model};
   if (aimee_anthropic_replay_prepare(prov_body, prov_body_len, &stream.replay))
   {
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "cannot bind native replay prefix"};
      if (!messages_publish_delta("native", &error))
         anthropic_delta_emit(&error, &stream.frontend, "native", model, emit, ctx);
      return -1;
   }
   sse_parser_init(&stream.parser);
   anthropic_backend_stream_state_init(&stream.backend);
   int status = wire_fence_post_stream(url, auth, prov_body, prov_body_len, native_ir_chunk_cb,
                                       &stream, ag->timeout_ms, extra[0] ? extra : NULL,
                                       ag->provider, ag->model, WIRE_FENCE_ANTHROPIC_MESSAGES);
   if (status == HTTP_RETRY_ADMISSION_REFUSED)
      messages_stream_pipeline_error(emit, ctx);
   else if (status != 200 || stream.failed || stream.parser.buf_len || stream.data_len ||
            !stream.backend.terminal_emitted)
   {
      if (!stream.backend.failed)
      {
         aimee_delta_t error = {
             .type = AIMEE_DELTA_ERROR,
             .error_message =
                 "Provider stream failed canonical admission or ended before completion"};
         if (!aimee_ir_session_capture_failed())
            messages_publish_delta(stream.message_id, &error);
         anthropic_delta_emit(&error, &stream.frontend, stream.message_id, model, emit, ctx);
      }
      status = -1;
   }
   if ((stream.backend.pending_usage_in || stream.backend.pending_usage_out) &&
       agent_ingress_accounting_enabled())
      agent_ingress_record_cost(
          ag->name, ag->model, model, NULL, (int)stream.backend.pending_usage_in,
          (int)stream.backend.pending_usage_out, (int)stream.backend.pending_cache_write,
          (int)stream.backend.pending_cache_read, "anthropic-ingress",
          status == 200 ? "realized" : "partial");
   if (stream.saw_reasoning)
      aimee_ir_metric_inc(status == 200 ? AIMEE_IR_M_REASONING_OBSERVED
                                        : AIMEE_IR_M_REASONING_INCOMPLETE,
                          AIMEE_WIRE_ANTHROPIC);
   if (invalid_frame_out)
      *invalid_frame_out = stream.saw_invalid_request;
   sse_parser_free(&stream.parser);
   free(stream.data);
   free(stream.message_id);
   aimee_ir_response_builder_free(&stream.assembled);
   return status;
}

/* Slice-5 IR-delta streaming relay: drive the OpenAI-chat -> Anthropic SSE relay
 * through the neutral IR-delta model, synthesize a clean close if the upstream
 * cut off early, and record cost. Extracted from messages_stream(). */
/* Returns the upstream HTTP status, so the caller can tell a payload the
 * provider rejected from a stream that merely ended. */
static int messages_stream_ir_relay(const char *url, const char *auth, const void *prov_body,
                                    size_t prov_body_len, const char *extra, agent_t *ag,
                                    const char *model, const char *msg_id, int input_est,
                                    server_http_sse_event_emit emit, void *ctx)
{
   prov_stream_ctx_t pc;
   /* Native chunks enter the canonical delta model before client rendering. */
   memset(&pc, 0, sizeof pc);
   sse_parser_init(&pc.parser);
   openai_stream_state_init(&pc.ir_ost);
   pc.emit = emit;
   pc.emit_ctx = ctx;
   pc.msg_id = msg_id;
   pc.model = model;
   int ir_status = wire_fence_post_stream(url, auth, prov_body, prov_body_len, prov_chunk_cb, &pc,
                                          ag->timeout_ms, extra[0] ? extra : NULL, ag->provider,
                                          ag->model, WIRE_FENCE_OPENAI_CHAT);
   sse_parser_free(&pc.parser);
   if (ir_status == HTTP_RETRY_ADMISSION_REFUSED)
   {
      messages_stream_pipeline_error(emit, ctx);
      return ir_status;
   }
   if (pc.ir_failed || !pc.ir_ost.stopped)
   {
      aimee_delta_t terminal;
      int n =
          pc.ir_failed || ir_status != 200 ? -1 : openai_stream_finish(&pc.ir_ost, &terminal, 1);
      if (n > 0)
      {
         if (messages_publish_delta(msg_id, &terminal))
         {
            terminal = (aimee_delta_t){.type = AIMEE_DELTA_ERROR,
                                       .error_message = "Canonical stream persistence failed"};
            ir_status = -1;
         }
         pc.ir_usage_out = terminal.usage_out;
         anthropic_delta_emit(&terminal, &pc.ir_ast, msg_id, model, emit, ctx);
      }
      else if (n < 0)
      {
         terminal =
             (aimee_delta_t){.type = AIMEE_DELTA_ERROR,
                             .error_message = "Provider stream ended before its completion event"};
         anthropic_delta_emit(&terminal, &pc.ir_ast, msg_id, model, emit, ctx);
         ir_status = -1;
      }
   }
   if ((input_est > 0 || pc.ir_usage_out > 0) && agent_ingress_accounting_enabled())
      agent_ingress_record_cost(ag->name, ag->model, model, NULL,
                                pc.ir_ost.usage_seen ? (int)pc.ir_ost.usage_in : input_est,
                                (int)pc.ir_usage_out, (int)pc.ir_ost.usage_cache_read,
                                (int)pc.ir_ost.usage_cache_write, "anthropic-ingress",
                                ir_status == 200 ? "realized" : "partial");
   return ir_status;
}

static int messages_stream(const char *body, server_http_sse_event_emit emit, void *ctx)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC); /* shadow (Slice 3), gated no-op */
   agent_t agbuf;
   const char *model = req ? jo_cstr(req, "model") : "";
   agent_t *ag = (req && resolve_requested_agent(&agbuf, model) == 0) ? &agbuf : NULL;
   aimee_request_t policy = {0};
   char policy_error[128];
   cJSON *mutation = NULL;
   char *prov_body = NULL;
   char url[MAX_ENDPOINT_LEN + 64];
   char auth[MAX_API_KEY_LEN + 32];
   char extra[512];
   char msg_id[48];
   const delegate_driver_t *driver;
   int input_est;
   int responses_wire = 0;
   wire_fence_t *wire_snapshot = NULL;
   gw_mutate_ctx_t gwmc;
   /* 200 unless a streaming helper reports otherwise. The native relay path
      jumps to cleanup without calling one, and it must not read as a rejection. */
   int stream_status = 200;
   /* Set only by the native relay, which is the one path that can see an
      upstream error frame; the translator paths leave it 0. */
   int stream_bad_frame = 0;
   const void *wire_prov_body = NULL;
   size_t wire_prov_body_len = 0;

   gw_mutate_ctx_init(&gwmc);

   mint_msg_id(msg_id, sizeof(msg_id));

   /* An explicit unknown model is an error. If the default is unavailable,
    * preserve the prior well-formed empty stream so a committed SSE response
    * still terminates cleanly rather than hanging. */
   if (!req || !ag)
   {
      if (req && model[0] && strcmp(model, "aimee") != 0)
      {
         char err[512];
         snprintf(err, sizeof(err),
                  "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\","
                  "\"message\":\"the requested model is not available\"}}");
         emit(ctx, "error", err);
         cJSON_Delete(req);
         return 0;
      }
      anthropic_stream_state_t state = {0};
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "Cannot initialize provider request"};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
      cJSON_Delete(req);
      return 0;
   }

   delegate_drivers_init();
   driver = delegate_driver_get(ag->provider);
   /* Exact-parity passthrough only applies to the Anthropic-native path. */
   int parity = driver_is_anthropic(driver);
   if (messages_prepare_request(body, ag, parity, &policy, policy_error, sizeof policy_error) ||
       messages_mutate_request(&policy, parity, &mutation, &gwmc))
   {
      anthropic_stream_state_t state = {0};
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "Cannot assemble canonical provider request"};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
      stream_status = 400;
      goto cleanup;
   }
   if (delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
      goto cleanup;
   /* The Responses replay path is keyed off the actual upstream shape, not the
    * provider label, so Codex aliases that still resolve to /responses stay in
    * the buffered replay path too. */
   responses_wire = strstr(url, "/responses") != NULL;
   if (parity)
      build_anthropic_parity_headers(extra, sizeof(extra));
   else
      agent_build_extra_headers(ag, extra, sizeof(extra));

   /* Whether we ask the UPSTREAM to stream depends on how we READ the reply, not on
    * what the client asked. The buffered-replay path (prevent-subagents, or a
    * /responses wire) fetches the reply to completion, so it must request a
    * NON-streaming body for the non-responses wires -- asking for SSE and then
    * cJSON_Parse'ing it is the "unparseable reply" bug. This applies to the ANTHROPIC
    * parity build too, not only the IR/openai build below. */
   /* Any provider may need the response completion stage after an edit. Buffer
    * only armed turns so the bounded completion behavior is independent of the
    * client/provider protocol pairing. Unarmed native Anthropic turns retain incremental
    * canonical relay. */
   int completion_buffered = gw_ir_response_completion_armed(&policy);
   int buffered_replay =
       gateway_prevent_subagents_enabled() || responses_wire || completion_buffered;
   int upstream_stream = responses_wire ? 1 : (buffered_replay ? 0 : 1);

   prov_body = anthropic_build_prov_body(&policy,
                                         parity           ? AIMEE_WIRE_ANTHROPIC
                                         : responses_wire ? AIMEE_WIRE_RESPONSES
                                                          : AIMEE_WIRE_OPENAI_CHAT,
                                         upstream_stream);

   if (!prov_body)
   {
      anthropic_stream_state_t state = {0};
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "could not assemble canonical provider request"};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
      stream_status = 500;
      goto cleanup;
   }
   const char *pristine_body = prov_body ? prov_body : "{}";
   int economizer_active = econ_mode_current() != ECON_MODE_OFF;
   wire_fence_route_t wire_route = parity           ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                   : responses_wire ? WIRE_FENCE_OPENAI_RESPONSES
                                                    : WIRE_FENCE_OPENAI_CHAT;
   wire_fence_bytes_t wire_body;
   if (wire_fence_select(economizer_active, wire_route, pristine_body, strlen(pristine_body),
                         &wire_snapshot, &wire_body) != 0)
   {
      const char *error = wire_fence_last_error();
      char frame[256];
      snprintf(frame, sizeof(frame),
               "{\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"%s\"}}",
               wire_fence_error_type(error), error);
      emit(ctx, "error", frame);
      stream_status = wire_fence_error_http_status(error);
      goto cleanup;
   }
   wire_prov_body = (const char *)wire_body.data;
   wire_prov_body_len = wire_body.len;

   /* P2c streaming: when gateway_prevent_subagents is ON, or the primary
    * speaks the OpenAI Responses API (`chatgpt` / Codex), the streaming
    * path becomes buffered — we fetch the upstream reply to completion,
    * run the police function on the parsed struct (same logic as the
    * buffered /v1/messages path), and replay the policed reply as a
    * well-formed Anthropic SSE sequence via emit_message_as_sse. The
    * chatgpt special-case is necessary because its stream format is
    * response.output_* / response.completed, not the OpenAI chat chunk
    * shape that today's incremental translator understands. Off (the
    * default) falls through to today's incremental relay/translator. */
   if (buffered_replay)
   {
      messages_stream_buffered_replay(url, auth, wire_prov_body, wire_prov_body_len, extra, ag,
                                      driver, model, msg_id, responses_wire, &policy, emit, ctx);
      goto cleanup;
   }

   if (driver_is_anthropic(driver))
   {
      stream_status = messages_stream_native_ir(url, auth, wire_prov_body, wire_prov_body_len,
                                                extra, ag, model, emit, ctx, &stream_bad_frame);
      goto cleanup;
   }

   input_est = messages_estimate_tokens(&policy);
   if (input_est < 0)
   {
      anthropic_stream_state_t state = {0};
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "cannot estimate canonical request"};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
      stream_status = 500;
      goto cleanup;
   }

   stream_status = messages_stream_ir_relay(url, auth, wire_prov_body, wire_prov_body_len, extra,
                                            ag, model, msg_id, input_est, emit, ctx);
cleanup:
   /* THE STREAMING HALF OF THE SAFETY NET. Bytes are already with the client, so
    * nothing can be restored or resent -- the breaker is all that is left, and
    * tripping it is what stops the next turn rebuilding the same bad payload.
    *
    * Only the payload-class statuses count. 401/403/404/429 are auth, not-found
    * and rate-limit; disabling the lever on those would switch reduction off for
    * ordinary throttling. */
   if (gw_status_is_invalid_request(stream_status) || stream_bad_frame)
      gw_stream_disable(&gwmc, "stream_invalid_request");
   wire_fence_destroy(wire_snapshot);
   gw_mutate_ctx_free(&gwmc);
   free(prov_body);
   aimee_request_free(&policy);
   cJSON_Delete(mutation);
   cJSON_Delete(req);
   return 0;
}

/* --- POST /v1/messages/count_tokens ------------------------------------- */

static int count_tokens(const char *body, char *resp, int cap)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   if (!req)
      return write_error(resp, cap, 400, "invalid_request_error", "invalid JSON body", 0);
   if (!cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      return write_error(resp, cap, 400, "invalid_request_error",
                         "request body must be a JSON object", 0);
   }
   cJSON *request_messages = cJSON_GetObjectItemCaseSensitive(req, "messages");
   if (!cJSON_IsArray(request_messages))
   {
      cJSON_Delete(req);
      return write_error(resp, cap, 400, "invalid_request_error",
                         "messages is required and must be an array", 0);
   }
   aimee_request_t policy = {0};
   char error[128];
   if (aimee_frontend_parse_wire(body, body ? strlen(body) : 0, AIMEE_WIRE_ANTHROPIC, &policy,
                                 error, sizeof error) ||
       aimee_ir_request_admit(&policy))
   {
      aimee_request_free(&policy);
      cJSON_Delete(req);
      return write_error(resp, cap, 400, "invalid_request_error", "invalid canonical count request",
                         0);
   }
   const char *model = jo_cstr(req, "model");

   /* Proxy to Anthropic's real /v1/messages/count_tokens so Claude Code's
    * context-budget math matches api.anthropic.com, not a local estimate — only
    * when the primary speaks the Anthropic API; otherwise fall through to the
    * estimate. */
   {
      agent_t agbuf;
      agent_t *ag = resolve_requested_agent(&agbuf, model) == 0 ? &agbuf : NULL;
      const delegate_driver_t *driver;
      delegate_drivers_init();
      driver = ag ? delegate_driver_get(ag->provider) : NULL;
      if (ag && driver_is_anthropic(driver))
      {
         char url[MAX_ENDPOINT_LEN + 64];
         char ct_url[MAX_ENDPOINT_LEN + 96];
         char auth[MAX_API_KEY_LEN + 32];
         char extra[640];
         char *response = NULL;
         if (delegate_build_url(driver, ag, url, sizeof(url)) == 0 &&
             agent_resolve_auth(ag, auth, sizeof(auth)) == 0)
         {
            int hs;
            snprintf(ct_url, sizeof(ct_url), "%s/count_tokens", url); /* url ends in /messages */
            build_anthropic_parity_headers(extra, sizeof(extra));
            char *native = messages_resolve_model(&policy, ag, 1) == 0
                               ? aimee_backend_render_wire(&policy, AIMEE_WIRE_ANTHROPIC)
                               : NULL;
            if (!native)
            {
               aimee_request_free(&policy);
               cJSON_Delete(req);
               return write_error(resp, cap, 400, "invalid_request_error",
                                  "cannot render canonical count request", 0);
            }
            hs = agent_http_post(ct_url, auth, native, &response, ag->timeout_ms,
                                 extra[0] ? extra : NULL);
            free(native);
            if (response && (int)strlen(response) < cap)
            {
               /* Relay upstream status + body verbatim (200 with the real count,
                * or the real error). */
               memcpy(resp, response, strlen(response) + 1);
               free(response);
               aimee_request_free(&policy);
               cJSON_Delete(req);
               return hs;
            }
            free(response); /* unreachable/oversized: fall through to the estimate */
         }
      }
   }

   int n = messages_estimate_tokens(&policy);
   aimee_request_free(&policy);
   if (n < 0)
   {
      cJSON_Delete(req);
      return write_error(resp, cap, 500, "api_error", "cannot estimate canonical request", 0);
   }
   cJSON *out = cJSON_CreateObject();
   int status;

   cJSON_AddNumberToObject(out, "input_tokens", n);
   status = write_json(out, resp, cap);

   cJSON_Delete(out);
   cJSON_Delete(req);
   return status;
}

int anthropic_http_response_retry_after(void)
{
   return g_response_retry_after;
}

/* Capture the inbound anthropic-version / anthropic-beta headers for the parity
 * passthrough, and reset the per-request response Retry-After. Lives here (not in
 * the pure anthropic_ingress translation unit, nor inline in the size-capped
 * server_http.c) because it bridges http_header (server_http.h) and the
 * thread-local stores. Set on every request — empty/0 when absent — so nothing
 * leaks across requests on a reused worker thread. */
void anthropic_http_capture_request_headers(const char *raw_request)
{
   char a_ver[64] = "", a_beta[512] = "";
   if (raw_request)
   {
      http_header(raw_request, "Anthropic-Version", a_ver, sizeof(a_ver));
      http_header(raw_request, "Anthropic-Beta", a_beta, sizeof(a_beta));
   }
   anthropic_ingress_set_request_headers(a_ver, a_beta);
   g_response_retry_after = 0;
}

void anthropic_http_register(void)
{
   server_http_set_messages_handler(messages_buffered);
   server_http_set_messages_stream_handler(messages_stream);
   server_http_set_count_tokens_handler(count_tokens);
}
