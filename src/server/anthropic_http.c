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
#include <aimee/gateway/gateway_pipeline.h>
#include "gw_stage_registry.h"
#include "gw_stage_governance.h"
#include "gw_stage_completion.h"
#include "router_advise.h"   /* gw_stage_router — the request->workflow seam */
#include "aimee_ir_shadow.h" /* Slice 3: IR shadow-mode observer */
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

/* Slice 3: parse the provider JSON response into `parsed`. When AIMEE_IR_RESP_PATH is
 * on and the primary is OPENAI-WIRE, route through the canonical IR
 * (openai_backend_parse -> IR -> transitional adapter); legacy driver->parse_response
 * on flag-off, anthropic wire, or any IR-parse failure. `raw` is the original response
 * text handed to the legacy parser. */
static void ir_or_legacy_parse_response(cJSON *provider_resp, const char *raw,
                                        const delegate_driver_t *driver, parsed_response_t *parsed)
{
   if (aimee_ir_resp_path_enabled() && !driver_is_anthropic(driver))
   {
      aimee_response_t ir_resp;
      char ir_err[128];
      memset(&ir_resp, 0, sizeof(ir_resp));
      if (openai_backend_parse(provider_resp, &ir_resp, ir_err, sizeof(ir_err)) == 0)
      {
         aimee_ir_response_to_parsed(&ir_resp, parsed);
         aimee_response_free(&ir_resp);
         return;
      }
      aimee_response_free(&ir_resp);
   }
   if (driver && driver->parse_response)
      driver->parse_response(provider_resp, raw, parsed);
   else
      /* No driver: default to the openai wire via the IR (zeroed *parsed on failure). */
      agent_ir_parse_json_response(provider_resp, 0 /*openai*/, -1, NULL, parsed);
}

static int driver_consumes_system_prompt(const delegate_driver_t *driver, const agent_t *ag)
{
   driver_caps_t caps;

   delegate_get_caps(driver, ag, &caps);
   return (caps.capability_flags & DRIVER_CAP_SYSTEM_MSG) != 0;
}

/* Translate an Anthropic request into the selected provider's message/tool
 * shape. Anthropic itself receives the original Messages wire shape; the
 * OpenAI-family providers receive the inverse delegate-driver conversion.
 * *out_system is a malloc'd flattened system prompt (caller frees). */
static void translate_request(const cJSON *req, const delegate_driver_t *driver, const agent_t *ag,
                              cJSON **out_messages, cJSON **out_tools, char **out_system)
{
   *out_system = anthropic_system_to_text(req);
   if (driver_is_anthropic(driver))
   {
      const cJSON *messages = cJSON_GetObjectItemCaseSensitive(req, "messages");
      const cJSON *tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
      *out_messages = cJSON_IsArray(messages) ? cJSON_Duplicate(messages, 1) : NULL;
      *out_tools = cJSON_IsArray(tools) ? cJSON_Duplicate(tools, 1) : NULL;
      return;
   }
   *out_messages =
       anthropic_messages_to_openai(cJSON_GetObjectItemCaseSensitive(req, "messages"),
                                    driver_consumes_system_prompt(driver, ag) ? NULL : *out_system);
   *out_tools = driver && strcmp(driver->name, "chatgpt") == 0
                    ? anthropic_tools_to_responses(cJSON_GetObjectItemCaseSensitive(req, "tools"))
                    : anthropic_tools_to_openai(cJSON_GetObjectItemCaseSensitive(req, "tools"));
}

/* ---- Gateway request pipeline (universal-gateway P2a) -------------------------
 * The two inline request transforms (memory/context injection, tool policing) are
 * run as ordered stages over a `gw_request_t` rather than hand-sequenced at each
 * call site, so the seam is one shared, testable pipeline. translate_request stays
 * a TERMINAL render step after the pipeline (it produces the provider shape rather
 * than mutating `raw`). Behavior is identical to the previous inline prelude. */

/* Memory/context pre-injection is now the shared gw_stage_memory (gw_stage_memory.c)
 * with mem_target = GW_MEM_ANTHROPIC_MESSAGES; the `if (!r->parity)` parity gate
 * (only memory injection is parity-gated, so the Anthropic-native passthrough does
 * not perturb Claude Code's cached prefix) lives in that stage's Anthropic arm. */

/* Tool-policing stage: strip subagent-spawning tools etc. Returns the number of
 * tools stripped (already the contract of gateway_policy_apply_request). */
static int gw_stage_tool_policing(gw_request_t *r, void *ud)
{
   (void)ud;
   return gateway_policy_apply_request(r->raw, 0 /* Anthropic tool shape */);
}

/* Model-pin stage (P2b): force the served model to the configured primary's model
 * when the policy is on (no-op by default). Resolves the P1 single-model-shim
 * regression — an operator running a fixed-model Anthropic-compatible shim enables
 * it so an arbitrary client model name is not forwarded and rejected upstream.
 * Returns 1 if the model was changed. */
static int gw_stage_model_pin(gw_request_t *r, void *ud)
{
   const agent_t *ag = (const agent_t *)r->ag;
   (void)ud;
   return gateway_policy_pin_model(r->raw, ag ? ag->model : NULL);
}

/* Run the request pipeline over an inbound Anthropic /v1/messages request, in
 * place, with the same stages and order as the prior inline prelude (memory →
 * policy), plus the model-pin policy. Returns total interventions (≥0) or <0 on a
 * stage error. */
/* Resolve the governance response toggle: config-store `modules.governance` (canonical) ->
 * deprecated env default. Cached legacy_config_read, so an operator toggle applies without a
 * restart; keeps gw_stage_governance config-free. */
static int anthropic_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static int messages_run_request_pipeline(cJSON *req, const delegate_driver_t *driver,
                                         const agent_t *ag, int parity, int stream)
{
   /* P5 (§2.3): opt-in to inject the envelope on the Anthropic-native passthrough.
    * Read config here so gw_stage_memory stays config-free. */
   int allow_anthropic_inject =
       (config_present() && config_ingress_preinject_anthropic_enabled()) ? 1 : 0;

   gw_request_t r = {
       .raw = req,
       .driver = driver,
       .ag = ag,
       /* parity == driver_is_anthropic(driver), so serving_api follows it (the
        * client is always Anthropic /v1/messages here) — no second predicate call. */
       .serving_api = parity ? GW_API_ANTHROPIC : GW_API_OPENAI,
       .mem_target = GW_MEM_ANTHROPIC_MESSAGES,
       .parity = parity,
       .stream = stream,
       .allow_anthropic_inject = allow_anthropic_inject,
   };
   /* Memory PORTED to the IR transform seam (aimee_ir_apply_request_stages): it now
    * fires once on the typed IR after frontend_parse, so it is no longer a pre-IR
    * wire-anchored stage here. This catalog keeps the stages that still act on the raw
    * wire request (tool policing, routing, model pin). */
   const gw_stage_slot_t slots[] = {
       {"tool_policing", gw_stage_tool_policing, NULL, 1},
       {"router", gw_stage_router, NULL, 1}, /* S1/S2: unified request->workflow seam */
       {"model_pin", gw_stage_model_pin, NULL, 1},
   };
   gw_stage_t stages[8];
   int nstages = gw_stage_registry_build(slots, sizeof(slots) / sizeof(slots[0]), stages, 8);
   if (nstages < 0)
      return -1; /* misconfigured stage catalog: fail closed rather than run partial */
   return gw_pipeline_run_request(&r, stages, (size_t)nstages);
}

/* Anthropic-native ingress is a near-passthrough: Claude Code already speaks
 * Anthropic Messages, so preserve request fields such as system cache_control,
 * tool_choice, thinking, stop_sequences, top_p, and top_k. By default the
 * configured primary model wins over the inbound model name; under parity the
 * inbound model is honored verbatim (falling back to the primary only when the
 * client omitted it) so Claude Code's per-task model selection survives. */
static char *build_anthropic_provider_body(const cJSON *in, const agent_t *ag, int stream,
                                           int parity, const char *original_frame)
{
   cJSON *req;
   char *body;

   req = cJSON_Duplicate((cJSON *)in, 1);
   if (!req)
      return NULL;
   if (!parity)
   {
      cJSON *model;
      cJSON_DeleteItemFromObjectCaseSensitive(req, "model");
      model = cJSON_CreateString(ag && ag->model[0] ? ag->model : "claude");
      if (model)
         cJSON_AddItemToObject(req, "model", model);
   }
   else if (!cJSON_GetObjectItemCaseSensitive(req, "model") && ag && ag->model[0])
   {
      /* Parity, but the client sent no model: give the upstream call one. */
      cJSON_AddStringToObject(req, "model", ag->model);
   }
   const cJSON *native_stream = cJSON_GetObjectItemCaseSensitive(req, "stream");
   if (stream && !cJSON_IsTrue(native_stream))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(req, "stream");
      cJSON_AddBoolToObject(req, "stream", 1);
   }
   else if (!stream && cJSON_IsTrue(native_stream))
      cJSON_DeleteItemFromObjectCaseSensitive(req, "stream");
   aimee_request_t ir = {0};
   char error[128];
   if (aimee_frontend_parse_native_frame(req, original_frame,
                                         original_frame ? strlen(original_frame) : 0,
                                         AIMEE_WIRE_ANTHROPIC, &ir, error, sizeof error))
   {
      cJSON_Delete(req);
      return NULL;
   }
   aimee_ir_apply_native_request_stages(&ir);
   body = aimee_backend_render_wire(&ir, AIMEE_WIRE_ANTHROPIC);
   aimee_request_free(&ir);
   cJSON_Delete(req);
   return body;
}

/* Host guidance is a new system block. Replacing the client's system with a
 * flattened string would erase its cache breakpoints and native extensions. */
static int messages_add_host_guidance(cJSON *request, const cJSON *messages, const cJSON *tools)
{
   char *tool = gw_request_tool_system_prompt(tools, NULL);
   char *completion = gw_request_completion_system_prompt(messages, tool);
   const char *policy = completion ? completion : tool;
   if (!policy)
      return 0;
   const cJSON *old = cJSON_GetObjectItemCaseSensitive(request, "system");
   cJSON *system = cJSON_IsArray(old) ? cJSON_Duplicate(old, 1) : cJSON_CreateArray();
   cJSON *prefix = NULL, *entry = cJSON_CreateObject();
   int ok = system && entry;
   if (ok && cJSON_IsString(old))
   {
      prefix = cJSON_CreateObject();
      ok = prefix && cJSON_AddStringToObject(prefix, "type", "text") &&
           cJSON_AddStringToObject(prefix, "text", old->valuestring) &&
           cJSON_AddItemToArray(system, prefix);
      if (ok)
         prefix = NULL;
   }
   ok = ok && cJSON_AddStringToObject(entry, "type", "text") &&
        cJSON_AddStringToObject(entry, "text", policy) && cJSON_AddItemToArray(system, entry);
   if (ok)
   {
      entry = NULL;
      ok = old ? cJSON_ReplaceItemInObjectCaseSensitive(request, "system", system)
               : cJSON_AddItemToObject(request, "system", system);
      if (ok)
         system = NULL;
   }
   cJSON_Delete(prefix);
   cJSON_Delete(entry);
   cJSON_Delete(system);
   free(completion);
   free(tool);
   return ok ? 0 : -1;
}

/* --- Buffered: POST /v1/messages (stream:false) ------------------------- */

/* Build the upstream provider body from the current `req`. Every provider request
 * enters canonical admission and renders at its selected output boundary.
 * Re-callable so the gateway-mutation 4xx
 * restore path can rebuild the body from the restored (pristine) req. Returns a
 * malloc'd JSON string (caller frees) or NULL. */
static char *anthropic_build_prov_body(cJSON *req, const delegate_driver_t *driver,
                                       const agent_t *ag, int parity, int stream,
                                       const char *original_frame)
{
   char *prov_body = NULL;
   if (driver_is_anthropic(driver))
      prov_body = build_anthropic_provider_body(req, ag, stream, parity, original_frame);
   else
   {
      prov_body = aimee_ir_build_provider_body(
          req, driver ? driver->name : NULL, ag->model,
          agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)), stream);
   }
   return prov_body;
}

static int messages_buffered(const char *body, char *resp, int cap)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   agent_t agbuf;
   agent_t *ag;
   cJSON *messages = NULL, *tools = NULL, *provider_resp = NULL, *out = NULL;
   char *system_text = NULL, *prov_body = NULL, *response = NULL;
   char url[MAX_ENDPOINT_LEN + 64];
   char auth[MAX_API_KEY_LEN + 32];
   char extra[512];
   char msg_id[48];
   const delegate_driver_t *driver;
   parsed_response_t parsed = {0}; /* freed only on the success path, but init defensively */
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
   /* Request stages (memory injection, tool policing) over the canonical IR, then
    * translate as the terminal render step. Same order/behavior as the prior
    * inline prelude; no-op stages when nothing is configured. A <0 return is a
    * stage that hard-failed: abort rather than forward a half-altered request (no
    * stage returns <0 today; the intervention count is plumbed for P2b's audit). */
   if (messages_run_request_pipeline(req, driver, ag, parity, 0 /* buffered */) < 0)
   {
      status = write_error(resp, cap, 500, "api_error", "gateway request pipeline failed",
                           AIMEE_ERR_REQUEST_PIPELINE);
      goto cleanup;
   }
   /* Re-read `model`: the model-pin stage may have replaced req's "model" node, so
    * the pointer cached above (line ~397) could now dangle. */
   model = jo_cstr(req, "model");

   /* Only AGGRESSIVE. Native Anthropic requests are mutated too now: the gateway
    * holds a per-session freeze, so a folded prefix stays byte-identical after the
    * turn it first engages, which is what the client's cache prefix needs. */
   if (gw_mutate_upstream_ok(parity))
   {
      char *mut_sys = anthropic_system_to_text(req);
      gw_buffered_mutate(req, "messages", model, mut_sys, server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(), &gwmc);
      free(mut_sys);
   }

   translate_request(req, driver, ag, &messages, &tools, &system_text);
   if (messages_add_host_guidance(req, messages, tools) != 0)
   {
      status = write_error(resp, cap, 500, "api_error", "cannot assemble host guidance", 0);
      goto cleanup;
   }
   free(system_text);
   system_text = anthropic_system_to_text(req);
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

   prov_body = anthropic_build_prov_body(req, driver, ag, parity, responses_wire, body);
   if (!prov_body)
   {
      status = write_error(resp, cap, 500, "api_error",
                           "could not assemble canonical provider request", 0);
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
      status = write_error(resp, cap, wire_fence_error_http_status(error),
                           wire_fence_error_type(error), error, AIMEE_ERR_REQUEST_PIPELINE);
      goto cleanup;
   }
   http_status =
       wire_fence_post(url, auth, wire_body.data, wire_body.len, &response, ag->timeout_ms,
                       extra[0] ? extra : NULL, 1, 0, 0, ag->provider, ag->model, NULL, wire_route);
   if (http_status == HTTP_RETRY_ADMISSION_REFUSED)
   {
      const char *error = wire_fence_last_error();
      status = write_error(resp, cap, wire_fence_error_http_status(error),
                           wire_fence_error_type(error), error, AIMEE_ERR_REQUEST_PIPELINE);
      goto cleanup;
   }

   /* THE GATEWAY SAFETY NET. Until now nothing called this, so a reduced payload
    * the provider rejected tripped no breaker and repeated on every later turn.
    * The module decides and disables; the restore happens here because the body
    * is ours.
    *
    * A 4xx also asks for ONE resend of this turn with the restored body, which
    * this path cannot honour yet: the wire body is built once above and rebuilding
    * it needs that block made re-runnable. The breaker still trips, so the failure
    * stops repeating from the next turn -- the recovery of THIS turn is what is
    * still missing. */
   if (gw_buffered_after_status(req, "messages", http_status, &gwmc) == GW_POST_RESEND)
      aimee_log(LOG_INFO, "economizer.gateway",
                "seam=gateway upstream=%d restored to pristine and breaker tripped; the "
                "single resend of this turn is not wired yet",
                http_status);

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

   memset(&parsed, 0, sizeof(parsed));
   /* Codex/Responses replies with raw SSE (response.output_* / response.completed),
    * not a JSON body — parse it directly via the driver, the same way the streaming
    * buffered-replay path does. cJSON_Parse would fail and yield an empty reply. */
   if (responses_wire)
   {
      if (driver && driver->parse_response)
         driver->parse_response(NULL, response, &parsed);
   }
   else
      provider_resp = cJSON_Parse(response);
   if (responses_wire || provider_resp)
   {
      /* Slice 3: OPENAI-WIRE response parse via the IR (default-off flag). The
       * responses-wire reply was already parsed above from raw SSE. */
      if (!responses_wire)
         ir_or_legacy_parse_response(provider_resp, response, driver, &parsed);

      /* Slice 2 (canonical-IR): shadow-compare the legacy parse against the IR
       * response parse -> RESP_MATCH / RESP_MISMATCH on GET /v1/dashboard/metrics.
       * No-op unless AIMEE_IR_SHADOW; never affects the served reply. Provider wire
       * is anthropic when the primary is anthropic, else OpenAI chat (codex is SSE,
       * so provider_resp is NULL here and the compare self-skips). */
      aimee_ir_shadow_compare_response(&parsed, provider_resp, shadow_provider_wire(driver));

      /* P2c (response-side tool policing, buffered). Drops any `tool_use` block
       * the model emitted despite the request-side strip, before the audit row
       * reads parsed.stop_reason (so the audit log matches the wire) and before
       * anthropic_response_from_parsed renders the reply. The already-resolved
       * gateway policy gate is sent to the event-bus governance module; no
       * in-process decision fallback remains. Drop count is plumbed through to the
       * same pipeline-runner total the request-side strip already emits; a
       * future P2b audit pass surfaces both at once. */
      gw_response_run_governance(&parsed, anthropic_governance_enabled(),
                                 gateway_prevent_subagents_enabled());

      /* Cost accounting: the Anthropic /v1/messages ingress is a raw provider
       * proxy (no agent_log_call), so record this turn's spend, billed against
       * the served model and tagged as ingress. */
      if (agent_ingress_accounting_enabled())
         agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                   parsed.prompt_tokens, parsed.completion_tokens,
                                   parsed.cache_write_tokens, parsed.cache_read_tokens,
                                   "anthropic-ingress", NULL);

      if (gw_response_run_completion(&parsed, messages, tools, "tool_use"))
         LOG_INFO("completion.gate", "continued incomplete buffered Anthropic repair");
   }

   mint_msg_id(msg_id, sizeof(msg_id));
   out = anthropic_response_from_parsed(msg_id, model, &parsed);
   rc = write_json(out, resp, cap);
   status = rc;

   agent_free_parsed_response(&parsed);

cleanup:
   wire_fence_destroy(wire_snapshot);
   gw_mutate_ctx_free(&gwmc);
   cJSON_Delete(out);
   cJSON_Delete(provider_resp);
   free(response);
   free(prov_body);
   free(system_text);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
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
   size_t data_len;
   size_t data_cap;
   int emitted;
   /* An upstream error FRAME saying our request was invalid. The HTTP status is
      200 on a stream that opens and then fails, so the frame is the only place
      this shows up. */
   int saw_invalid_request;
   /* Usage observed by tapping the relayed Anthropic SSE (message_start carries
    * input + cache tokens, message_delta the final output_tokens), for the
    * ingress cost row written after the stream. The relayed bytes are unchanged. */
   int input_tokens;
   int output_tokens;
   int cache_write_tokens;
   int cache_read_tokens;
   /* Observation-only reasoning tap (see relay_observe_reasoning). Parsing the
    * relayed SSE does not mutate it, so exact-parity passthrough is unaffected. */
   anthropic_backend_stream_state_t ir_bst;
   char *reasoning;         /* accumulated thinking text, NULL until any arrives */
   size_t reasoning_len;    /* bytes used (excluding the NUL) */
   int reasoning_truncated; /* hit RELAY_REASONING_MAX: present but incomplete */
   int reasoning_giveup;    /* parser rejected the stream: nothing trustworthy */
} anthropic_relay_ctx_t;

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

static int relay_append_data(anthropic_relay_ctx_t *c, const char *data)
{
   size_t add = data ? strlen(data) : 0;
   size_t sep = c->data_len ? 1 : 0;
   size_t need = c->data_len + sep + add + 1;
   char *tmp;

   if (need > c->data_cap)
   {
      size_t cap = c->data_cap ? c->data_cap : 4096;
      while (cap < need)
         cap *= 2;
      tmp = realloc(c->data, cap);
      if (!tmp)
         return -1;
      c->data = tmp;
      c->data_cap = cap;
   }
   if (sep)
      c->data[c->data_len++] = '\n';
   if (add)
   {
      memcpy(c->data + c->data_len, data, add);
      c->data_len += add;
   }
   c->data[c->data_len] = '\0';
   return 0;
}

/* The model's reasoning is accumulated only to be MATCHED against, never replayed,
 * so it is bounded: the cap costs recall of late thought, where an unbounded buffer
 * would let a long chain grow the proxy's per-stream memory without limit. A capped
 * buffer is FLAGGED, never silently short. */
#define RELAY_REASONING_MAX 16384

static void relay_append_reasoning(anthropic_relay_ctx_t *c, const char *s)
{
   size_t add = strlen(s);
   if (!add)
      return;
   if (c->reasoning_len >= RELAY_REASONING_MAX)
   {
      c->reasoning_truncated = 1;
      return;
   }
   if (add > RELAY_REASONING_MAX - c->reasoning_len)
   {
      add = RELAY_REASONING_MAX - c->reasoning_len;
      c->reasoning_truncated = 1;
   }
   if (!c->reasoning)
   {
      /* allocated lazily: a turn with no thinking pays nothing */
      c->reasoning = malloc(RELAY_REASONING_MAX + 1);
      if (!c->reasoning)
         return;
      c->reasoning[0] = '\0';
   }
   memcpy(c->reasoning + c->reasoning_len, s, add);
   c->reasoning_len += add;
   c->reasoning[c->reasoning_len] = '\0';
}

/* Observation-only tap: feed the reassembled Anthropic SSE through the IR backend
 * parser and accumulate the model's reasoning. PARSING IS NOT MUTATING -- the caller
 * still emits the provider's bytes verbatim, so the exact-parity passthrough this
 * relay exists to provide is untouched.
 *
 * On ANY parser rejection the accumulated text is discarded and the tap gives up for
 * the rest of the stream. Half-parsed reasoning is worse than none: a consumer that
 * acts on a fragment it cannot trust is exactly the failure this feature must avoid,
 * so the tap abstains rather than hand on something questionable. */
static void relay_observe_reasoning(anthropic_relay_ctx_t *c, const char *event, const char *data)
{
   if (c->reasoning_giveup || !data || !data[0])
      return;
   /* Hot path: a text content_block_delta arrives once per token chunk, and parsing
    * every one would put a JSON parse in front of every streamed token. A real
    * thinking event always contains the substring, so this pre-filter is
    * conservative -- it can cost a needless parse, never a missed delta. */
   if (strcmp(event, "content_block_delta") == 0 && !strstr(data, "thinking"))
      return;
   cJSON *payload = cJSON_Parse(data);
   if (!payload)
      return; /* unparseable: still relayed verbatim, just nothing to observe */
   aimee_delta_t d[2];
   int n = anthropic_stream_to_deltas(event, payload, &c->ir_bst, d, 2);
   if (n < 0)
   {
      c->reasoning_giveup = 1;
      free(c->reasoning);
      c->reasoning = NULL;
      c->reasoning_len = 0;
   }
   for (int i = 0; i < n; i++)
      if (d[i].type == AIMEE_DELTA_BLOCK_DELTA && d[i].kind == AIMEE_BLK_THINKING &&
          d[i].text_delta)
         relay_append_reasoning(c, d[i].text_delta);
   cJSON_Delete(payload);
}

/* Observe usage on the relayed Anthropic SSE without altering it. message_start
 * carries input + cache tokens under message.usage; message_delta carries the
 * final cumulative output_tokens under usage. */
static void relay_capture_usage(anthropic_relay_ctx_t *c, const char *event, const char *data)
{
   if (!data || !data[0])
      return;
   if (strcmp(event, "message_start") != 0 && strcmp(event, "message_delta") != 0)
      return;
   cJSON *root = cJSON_Parse(data);
   if (!root)
      return;
   cJSON *msg = cJSON_GetObjectItem(root, "message");
   cJSON *usage = cJSON_GetObjectItem(msg ? msg : root, "usage");
   if (cJSON_IsObject(usage))
   {
      cJSON *it;
      if ((it = cJSON_GetObjectItem(usage, "input_tokens")) && cJSON_IsNumber(it))
         c->input_tokens = it->valueint;
      if ((it = cJSON_GetObjectItem(usage, "output_tokens")) && cJSON_IsNumber(it))
         c->output_tokens = it->valueint;
      if ((it = cJSON_GetObjectItem(usage, "cache_creation_input_tokens")) && cJSON_IsNumber(it))
         c->cache_write_tokens = it->valueint;
      if ((it = cJSON_GetObjectItem(usage, "cache_read_input_tokens")) && cJSON_IsNumber(it))
         c->cache_read_tokens = it->valueint;
   }
   cJSON_Delete(root);
}

static void relay_flush(anthropic_relay_ctx_t *c)
{
   const char *event = c->event[0] ? c->event : "message";

   if (c->data_len > 0 && strcmp(c->data, "[DONE]") != 0)
   {
      /* Classify BEFORE emitting: the bytes go to the client either way, but a
         frame blaming the request is what tells the breaker this session's
         reduced payload is the suspect. */
      if (gw_stream_anthropic_error_is_invalid_request(c->data))
         c->saw_invalid_request = 1;
      relay_capture_usage(c, event, c->data);
      relay_observe_reasoning(c, event, c->data);
      c->emit(c->emit_ctx, event, c->data); /* provider bytes, unchanged */
      c->emitted++;
   }
   c->event[0] = '\0';
   c->data_len = 0;
   if (c->data)
      c->data[0] = '\0';
}

static void relay_emit_transport_error(anthropic_relay_ctx_t *c, int status)
{
   char data[192];

   snprintf(data, sizeof(data),
            "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
            "\"message\":\"primary provider stream failed with status %d\"}}",
            status);
   c->emit(c->emit_ctx, "error", data);
   c->emitted++;
}

static int anthropic_relay_line_cb(const char *line, size_t len, void *ud)
{
   anthropic_relay_ctx_t *c = (anthropic_relay_ctx_t *)ud;
   if (len == 0)
   {
      relay_flush(c);
      return 0;
   }
   if (strncmp(line, "event:", 6) == 0)
   {
      const char *p = line + 6;
      while (*p == ' ')
         p++;
      snprintf(c->event, sizeof(c->event), "%s", p);
      return 0;
   }
   if (strncmp(line, "data:", 5) == 0)
   {
      const char *p = line + 5;
      while (*p == ' ')
         p++;
      return relay_append_data(c, p);
   }
   return 0;
}

static int anthropic_relay_chunk_cb(const char *data, size_t len, void *ud)
{
   anthropic_relay_ctx_t *c = (anthropic_relay_ctx_t *)ud;
   return sse_parser_feed(&c->parser, data, len, anthropic_relay_line_cb, c);
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
                                            const cJSON *messages, const cJSON *tools,
                                            server_http_sse_event_emit emit, void *ctx)
{
   char *buf_resp = NULL;
   int buf_status;
   parsed_response_t parsed;
   int raw_responses = responses_wire;
   memset(&parsed, 0, sizeof(parsed));
   wire_fence_route_t wire_route = responses_wire ? WIRE_FENCE_OPENAI_RESPONSES
                                   : driver && driver->name && !strcmp(driver->name, "anthropic")
                                       ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                       : WIRE_FENCE_OPENAI_CHAT;
   buf_status =
       wire_fence_post(url, auth, prov_body, prov_body_len, &buf_resp, ag->timeout_ms,
                       extra[0] ? extra : NULL, 1, 0, 0, ag->provider, ag->model, NULL, wire_route);
   if (buf_status == HTTP_RETRY_ADMISSION_REFUSED)
   {
      messages_stream_pipeline_error(emit, ctx);
      free(buf_resp);
      return;
   }
   if (buf_status == 200 && buf_resp)
   {
      if (raw_responses)
      {
         if (driver && driver->parse_response)
            driver->parse_response(NULL, buf_resp, &parsed);
         else
         {
            char err[256];
            snprintf(err, sizeof(err),
                     "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                     "\"message\":\"primary provider parser unavailable\"}}");
            if (emit)
               emit(ctx, "error", err);
            free(buf_resp);
            return;
         }
         gw_response_run_governance(&parsed, anthropic_governance_enabled(),
                                    gateway_prevent_subagents_enabled());
         /* Cost accounting (mirror the buffered path). */
         if ((parsed.prompt_tokens > 0 || parsed.completion_tokens > 0) &&
             agent_ingress_accounting_enabled())
            agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                      parsed.prompt_tokens, parsed.completion_tokens,
                                      parsed.cache_write_tokens, parsed.cache_read_tokens,
                                      "anthropic-ingress", NULL);
         /* The continuation may rewrite `parsed`, so it has to run BEFORE the
          * reply is serialised to the client -- emitting first would ship the
          * incomplete answer and then repair a copy nobody sees. */
         if (gw_response_run_completion(&parsed, messages, tools, "tool_use"))
            LOG_INFO("completion.gate", "continued incomplete streamed Anthropic repair");
         emit_message_as_sse(&parsed, msg_id, model, emit, ctx);
         agent_free_parsed_response(&parsed);
      }
      else
      {
         cJSON *provider_resp = cJSON_Parse(buf_resp);
         if (provider_resp)
         {
            /* Slice 3: OPENAI-WIRE response parse via the IR (default-off flag). */
            ir_or_legacy_parse_response(provider_resp, buf_resp, driver, &parsed);
            /* Slice 2 (canonical-IR): shadow-compare legacy vs IR response parse
             * (RESP_MATCH / RESP_MISMATCH). No-op unless AIMEE_IR_SHADOW. */
            aimee_ir_shadow_compare_response(&parsed, provider_resp, shadow_provider_wire(driver));
            gw_response_run_governance(&parsed, anthropic_governance_enabled(),
                                       gateway_prevent_subagents_enabled());
            /* Cost accounting (mirror the buffered path). */
            if ((parsed.prompt_tokens > 0 || parsed.completion_tokens > 0) &&
                agent_ingress_accounting_enabled())
               agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                         parsed.prompt_tokens, parsed.completion_tokens,
                                         parsed.cache_write_tokens, parsed.cache_read_tokens,
                                         "anthropic-ingress", NULL);
            /* The continuation may rewrite `parsed`, so it has to run BEFORE the
             * reply is serialised to the client -- emitting first would ship the
             * incomplete answer and then repair a copy nobody sees. */
            if (gw_response_run_completion(&parsed, messages, tools, "tool_use"))
               LOG_INFO("completion.gate", "continued incomplete streamed Anthropic repair");
            emit_message_as_sse(&parsed, msg_id, model, emit, ctx);
            agent_free_parsed_response(&parsed);
         }
         else
         {
            char err[256];
            snprintf(err, sizeof(err),
                     "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                     "\"message\":\"primary provider returned an unparseable reply\"}}");
            if (emit)
               emit(ctx, "error", err);
         }
         cJSON_Delete(provider_resp);
      }
   }
   else
   {
      char err[256];
      snprintf(err, sizeof(err),
               "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
               "\"message\":\"primary provider call failed with status %d\"}}",
               buf_status);
      if (emit)
         emit(ctx, "error", err);
   }
   free(buf_resp);
   return;
}

/* Native Anthropic streaming relay: forward the upstream Anthropic SSE
 * verbatim through the mutation breaker, then record realized/partial cost.
 * Extracted from messages_stream(); goto cleanup became return. */
/* Returns the upstream HTTP status. `invalid_frame_out` reports an upstream
 * error frame that blamed the request, which a 200-then-fail stream cannot show
 * any other way. */
static int messages_stream_native_relay(const char *url, const char *auth, const void *prov_body,
                                        size_t prov_body_len, const char *extra, agent_t *ag,
                                        const char *model, server_http_sse_event_emit emit,
                                        void *ctx, int *invalid_frame_out)
{
   anthropic_relay_ctx_t relay;
   int stream_status;
   memset(&relay, 0, sizeof(relay));
   sse_parser_init(&relay.parser);
   anthropic_backend_stream_state_init(&relay.ir_bst);
   relay.emit = emit;
   relay.emit_ctx = ctx;
   stream_status = wire_fence_post_stream(
       url, auth, prov_body, prov_body_len, anthropic_relay_chunk_cb, &relay, ag->timeout_ms,
       extra[0] ? extra : NULL, ag->provider, ag->model, WIRE_FENCE_ANTHROPIC_MESSAGES);
   if (stream_status == HTTP_RETRY_ADMISSION_REFUSED)
   {
      messages_stream_pipeline_error(emit, ctx);
      sse_parser_free(&relay.parser);
      free(relay.data);
      free(relay.reasoning);
      if (invalid_frame_out)
         *invalid_frame_out = 0;
      return stream_status;
   }
   relay_flush(&relay);
   if (stream_status != 200)
   {
      relay_emit_transport_error(&relay, stream_status);
   }
   /* Cost accounting for the native Anthropic streaming ingress, from the
    * usage tapped off the relayed SSE. A clean 200 is realized spend; an
    * aborted stream that still observed usage is recorded as partial so the
    * observed tokens are not silently lost. */
   if ((relay.input_tokens > 0 || relay.output_tokens > 0) && agent_ingress_accounting_enabled())
      agent_ingress_record_cost(ag->name, ag->model, model, NULL, relay.input_tokens,
                                relay.output_tokens, relay.cache_write_tokens,
                                relay.cache_read_tokens, "anthropic-ingress",
                                stream_status == 200 ? "realized" : "partial");
   /* Record what the reasoning tap saw. Nothing consumes the text yet -- these two
    * counters ARE the deliverable of this step: they measure how often a relayed
    * turn carries readable reasoning at all, which is what decides whether
    * thought-triggered recall is worth building on this path. */
   if (relay.reasoning_len > 0)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_ANTHROPIC);
   if (relay.reasoning_truncated || relay.reasoning_giveup)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_INCOMPLETE, AIMEE_WIRE_ANTHROPIC);
   sse_parser_free(&relay.parser);
   free(relay.data);
   free(relay.reasoning);
   if (invalid_frame_out)
      *invalid_frame_out = relay.saw_invalid_request;
   return stream_status;
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
   cJSON *messages = NULL, *tools = NULL;
   char *system_text = NULL, *prov_body = NULL;
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
   /* Request stages (memory injection, tool policing) over the canonical IR, then
    * translate as the terminal render step. Same order/behavior as the prior
    * inline prelude. A <0 return is a stage that hard-failed: emit a clean empty
    * stream (matching the setup-failure path) so the client's SSE reader
    * terminates, then abort rather than forward a half-altered request. (No stage
    * returns <0 today; the count is plumbed for P2b's audit.) */
   if (messages_run_request_pipeline(req, driver, ag, parity, 1 /* stream */) < 0)
   {
      anthropic_stream_state_t state = {0};
      aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                             .error_message = "Cannot initialize provider request"};
      anthropic_delta_emit(&error, &state, msg_id, model, emit, ctx);
      goto cleanup;
   }
   /* Re-read `model`: the model-pin stage may have replaced req's "model" node, so
    * the pointer cached at the top of this function could now dangle. */
   model = jo_cstr(req, "model");

   if (gw_mutate_upstream_ok(parity))
   {
      char *mut_sys = anthropic_system_to_text(req);
      gw_buffered_mutate(req, "messages", model, mut_sys, server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(), &gwmc);
      free(mut_sys);
   }

   translate_request(req, driver, ag, &messages, &tools, &system_text);
   if (messages_add_host_guidance(req, messages, tools) != 0)
   {
      emit(ctx, "error",
           "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"cannot assemble "
           "host guidance\"}}");
      stream_status = 500;
      goto cleanup;
   }
   free(system_text);
   system_text = anthropic_system_to_text(req);
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
    * client/provider protocol pairing. Unarmed native Anthropic turns retain the
    * byte-for-byte streaming relay. */
   int completion_buffered = gw_response_completion_armed(messages, tools);
   int buffered_replay =
       gateway_prevent_subagents_enabled() || responses_wire || completion_buffered;
   int upstream_stream = responses_wire ? 1 : (buffered_replay ? 0 : 1);

   prov_body = anthropic_build_prov_body(req, driver, ag, parity, upstream_stream, body);

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
                                      driver, model, msg_id, responses_wire, messages, tools, emit,
                                      ctx);
      goto cleanup;
   }

   if (driver_is_anthropic(driver))
   {
      stream_status = messages_stream_native_relay(url, auth, wire_prov_body, wire_prov_body_len,
                                                   extra, ag, model, emit, ctx, &stream_bad_frame);
      goto cleanup;
   }

   input_est = messages ? session_compact_estimate_tokens(messages) : 0;

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
   free(system_text);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
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
            hs = agent_http_post(ct_url, auth, (body && body[0]) ? body : "{}", &response,
                                 ag->timeout_ms, extra[0] ? extra : NULL);
            if (response && (int)strlen(response) < cap)
            {
               /* Relay upstream status + body verbatim (200 with the real count,
                * or the real error). */
               memcpy(resp, response, strlen(response) + 1);
               free(response);
               cJSON_Delete(req);
               return hs;
            }
            free(response); /* unreachable/oversized: fall through to the estimate */
         }
      }
   }

   char *system_text = req ? anthropic_system_to_text(req) : NULL;
   cJSON *messages = anthropic_messages_to_openai(request_messages, system_text);
   int n = messages ? session_compact_estimate_tokens(messages) : 0;
   cJSON *out = cJSON_CreateObject();
   int status;

   cJSON_AddNumberToObject(out, "input_tokens", n);
   status = write_json(out, resp, cap);

   cJSON_Delete(out);
   free(system_text);
   cJSON_Delete(messages);
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
