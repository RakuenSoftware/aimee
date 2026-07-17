/* anthropic_http.c: Anthropic Messages API ingress (POST /v1/messages).
 *
 * Registered into the server_http dispatch via server_http_set_messages_*
 * handlers at startup (anthropic_http_register). Lets Claude Code — which speaks
 * only the Anthropic Messages API and picks its endpoint from ANTHROPIC_BASE_URL
 * — drive aimee's configured primary model.
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
#include "context_reduce.h"
#include "gateway_mutate_wire.h"
#include "server_http_identity.h"
#include "agent_protocol.h"
#include "agent_types.h"
#include "anthropic_ingress.h"
#include "cJSON.h"
#include "delegate_driver.h"
#include "gateway_policy.h"
#include "gateway_pipeline.h"
#include "gw_stage_memory.h"
#include "router_advise.h"   /* gw_stage_router — the request->workflow seam */
#include "aimee_ir_shadow.h" /* Slice 3: IR shadow-mode observer */
#include "aimee_ir_metrics.h"
#include "aimee_ir_serve.h"  /* Slice 5: IR live request-build */
#include "aimee_ir_stream.h" /* Slice 5-wire: IR-delta incremental relay */
#include "ingress_preinject.h"
#include "json_fluent.h"
#include "log.h"
#include "server_http.h"
#include "session_compact.h"
#include "sse_parser.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Resolve aimee's primary agent (the configured default, else the first
 * agent). Claude Code's requested model is intentionally ignored — switching
 * models is `aimee primary`. acfg is caller-owned and must outlive the returned
 * pointer (it indexes into acfg). Returns NULL when none configured. */
static agent_t *resolve_primary(agent_config_t *acfg)
{
   agent_t *ag = NULL;
   if (agent_load_config(acfg) != 0)
      return NULL;
   if (acfg->default_agent[0])
      ag = agent_find(acfg, acfg->default_agent);
   if (!ag && acfg->agent_count > 0)
      ag = &acfg->agents[0];
   return ag;
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
static int messages_run_request_pipeline(cJSON *req, const delegate_driver_t *driver,
                                         const agent_t *ag, int parity, int stream)
{
   /* P5 (§2.3): opt-in to inject the envelope on the Anthropic-native passthrough.
    * Read config here so gw_stage_memory stays config-free. */
   config_t pcfg;
   int cfg_ok = (config_load(&pcfg) == 0);
   int allow_anthropic_inject = (cfg_ok && pcfg.ingress_preinject_anthropic_enabled) ? 1 : 0;

   /* Context economizer (gateway seam, SHADOW-MODE ONLY): when reduce.gateway_seam
    * is on, measure this inbound /v1/messages request's baseline + foldable
    * opportunity and record a forecast ledger row. measure_only is forced on here so
    * the request is NEVER mutated — the client transcript is read for the baseline
    * and the live `req` is forwarded byte-identical (we ignore res.messages, NULL in
    * measure-only). Done BEFORE the request pipeline so the baseline reflects the
    * pristine client request, not the memory-injected one. Fully guarded
    * (config-load, array, free) and context_reduce hard-bypasses on any internal
    * error, so this can never perturb the client response. Reuses the loaded pcfg;
    * cheap mtime-cached load keeps it dark by default. */
   if (cfg_ok && pcfg.reduce_gateway_seam)
   {
      cJSON *cmsgs = cJSON_GetObjectItemCaseSensitive(req, "messages");
      if (cJSON_IsArray(cmsgs))
      {
         char *sys_text = anthropic_system_to_text(req); /* flatten string|array system */
         const cJSON *cmodel = cJSON_GetObjectItemCaseSensitive(req, "model");
         const char *model = (cmodel && cJSON_IsString(cmodel)) ? cmodel->valuestring : NULL;
         reduce_config_t rcfg;
         memset(&rcfg, 0, sizeof(rcfg));
         rcfg.gateway_seam = 1;
         rcfg.measure_only = 1; /* shadow mode: this slice never mutates the request */
         rcfg.fold.retained_msgs = pcfg.fold_retained_msgs;
         reduce_result_t gw_res;
         memset(&gw_res, 0, sizeof(gw_res));
         if (context_reduce(cmsgs, sys_text, model, NULL, REDUCE_SEAM_GATEWAY, &rcfg, NULL,
                            &gw_res) == 0)
            agent_record_reduce_ledger(&gw_res, model, "gateway", NULL);
         context_reduce_result_free(&gw_res);
         free(sys_text);
      }
   }

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
   static const gw_stage_t stages[] = {
       {gw_stage_memory, NULL, "memory"},
       {gw_stage_tool_policing, NULL, "tool_policing"},
       {gw_stage_router, NULL, "router"}, /* S1/S2: the unified request->workflow seam */
       {gw_stage_model_pin, NULL, "model_pin"},
   };
   return gw_pipeline_run_request(&r, stages, sizeof(stages) / sizeof(stages[0]));
}

/* Build the provider request body via the selected delegate driver. `stream`
 * toggles the provider stream flag. Returns a malloc'd JSON string (caller
 * frees), or NULL. */
static char *build_provider_body(const delegate_driver_t *driver, const agent_t *ag,
                                 cJSON *messages, cJSON *tools, const char *system_text,
                                 int max_tokens, double temperature, int stream)
{
   cJSON *req = NULL;
   char *body;

   if (driver && driver->build_request)
      req = driver->build_request(ag, messages, tools, system_text, max_tokens, temperature);
   else
      req = agent_build_request_openai((agent_t *)ag, messages, tools, max_tokens, temperature);
   if (!req)
      return NULL;
   if (stream)
   {
      cJSON_AddBoolToObject(req, "stream", 1);
      /* Ask OpenAI-compatible providers to emit a final usage chunk so the
       * translator can record realized (not estimated) cost. Only OpenAI-family
       * providers honour stream_options.include_usage; the Anthropic provider has
       * its own usage in message_start/message_delta and takes a separate body
       * builder, so this is never sent to it. */
      if (!driver_is_anthropic(driver))
      {
         cJSON *so = cJSON_AddObjectToObject(req, "stream_options");
         if (so)
            cJSON_AddBoolToObject(so, "include_usage", 1);
      }
   }
   body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   return body;
}

/* Anthropic-native ingress is a near-passthrough: Claude Code already speaks
 * Anthropic Messages, so preserve request fields such as system cache_control,
 * tool_choice, thinking, stop_sequences, top_p, and top_k. By default the
 * configured primary model wins over the inbound model name; under parity the
 * inbound model is honored verbatim (falling back to the primary only when the
 * client omitted it) so Claude Code's per-task model selection survives. */
static char *build_anthropic_provider_body(const cJSON *in, const agent_t *ag, int stream,
                                           int parity)
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
   cJSON_DeleteItemFromObjectCaseSensitive(req, "stream");
   if (stream)
      cJSON_AddBoolToObject(req, "stream", 1);
   body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   return body;
}

/* --- Buffered: POST /v1/messages (stream:false) ------------------------- */

/* Build the upstream provider body from the current `req` (Anthropic parity path
 * duplicates req directly; otherwise via the IR path or the legacy translator over
 * the extracted messages/tools/system). Re-callable so the gateway-mutation 4xx
 * restore path can rebuild the body from the restored (pristine) req. Returns a
 * malloc'd JSON string (caller frees) or NULL. */
static char *anthropic_build_prov_body(cJSON *req, const delegate_driver_t *driver,
                                       const agent_t *ag, int parity, cJSON *messages, cJSON *tools,
                                       const char *system_text)
{
   char *prov_body = NULL;
   if (driver_is_anthropic(driver))
      prov_body = build_anthropic_provider_body(req, ag, 0, parity);
   else
   {
      if (aimee_ir_path_enabled())
         prov_body = aimee_ir_build_provider_body(
             req, driver->name, ag->model,
             agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
             0 /* buffered ingress: never ask the upstream to stream */);
      if (!prov_body)
      {
         aimee_ir_metric_inc(AIMEE_IR_M_LEGACY_FALLBACK, AIMEE_WIRE_ANTHROPIC);
         prov_body = build_provider_body(driver, ag, messages, tools, system_text,
                                         agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
                                         jo_num(req, "temperature", 1.0), 0);
      }
   }
   return prov_body;
}

static int messages_buffered(const char *body, char *resp, int cap)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   agent_config_t acfg;
   agent_t *ag;
   cJSON *messages = NULL, *tools = NULL, *provider_resp = NULL, *out = NULL;
   char *system_text = NULL, *prov_body = NULL, *response = NULL;
   char url[MAX_ENDPOINT_LEN + 64];
   char auth[MAX_API_KEY_LEN + 32];
   char extra[512];
   char msg_id[48];
   const delegate_driver_t *driver;
   parsed_response_t parsed = {0}; /* freed only on the success path, but init defensively */
   int status, http_status, rc;
   const char *model;
   gw_mutate_ctx_t gwmc;
   gw_mutate_ctx_init(&gwmc);

   if (!req)
      return write_error(resp, cap, 400, "invalid_request_error", "invalid JSON body", 0);
   model = jo_cstr(req, "model");

   /* SHADOW (Slice 3): observe the IR round-trip on this live request. No-op unless
    * AIMEE_IR_SHADOW is set; never affects the response. */
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC);

   ag = resolve_primary(&acfg);
   if (!ag)
   {
      cJSON_Delete(req);
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

   /* Economizer gateway MUTATION (primary-agent reduction). Default-OFF. Reduces
    * req["messages"] in place (compress-only) BEFORE translate/build so the provider
    * body is assembled from the reduced array; on a bad reduction the upstream 4xx is
    * caught below (restore-resend), the session is circuit-broken, and provenance is
    * cleared. A dark no-op when reduce_gateway_mutate is off. */
   if (gw_mutate_is_enabled())
   {
      char *mut_sys = anthropic_system_to_text(req);
      gw_buffered_mutate(req, "messages", model, mut_sys, server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(), &gwmc);
      free(mut_sys);
   }

   translate_request(req, driver, ag, &messages, &tools, &system_text);
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
   if (parity)
      build_anthropic_parity_headers(extra, sizeof(extra));
   else
      agent_build_extra_headers(ag, extra, sizeof(extra));

   prov_body = anthropic_build_prov_body(req, driver, ag, parity, messages, tools, system_text);
   http_status = agent_http_post(url, auth, prov_body ? prov_body : "{}", &response, ag->timeout_ms,
                                 extra[0] ? extra : NULL);

   /* Gateway mutation post-send: on ANY 4xx of a mutated request, restore the
    * pristine original, repair, disable the session, and resend ONCE from pristine;
    * on 5xx, disable the session without resending. No-op unless we mutated. */
   if (gw_buffered_after_status(req, "messages", http_status, &gwmc) == GW_POST_RESEND)
   {
      cJSON_Delete(messages);
      messages = NULL;
      cJSON_Delete(tools);
      tools = NULL;
      free(system_text);
      system_text = NULL;
      translate_request(req, driver, ag, &messages, &tools, &system_text);
      free(prov_body);
      prov_body = anthropic_build_prov_body(req, driver, ag, parity, messages, tools, system_text);
      free(response);
      response = NULL;
      http_status = agent_http_post(url, auth, prov_body ? prov_body : "{}", &response,
                                    ag->timeout_ms, extra[0] ? extra : NULL);
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

   provider_resp = cJSON_Parse(response);
   memset(&parsed, 0, sizeof(parsed));
   if (provider_resp)
   {
      if (driver && driver->parse_response)
         driver->parse_response(provider_resp, response, &parsed);
      else
         /* No driver: default to the openai wire, parsed through the IR (zeroed
          * *parsed on failure), matching the driver hooks. */
         agent_ir_parse_json_response(provider_resp, 0 /*openai*/, -1, NULL, &parsed);

      /* P2c (response-side tool policing, buffered). Drops any `tool_use` block
       * the model emitted despite the request-side strip, before the audit row
       * reads parsed.stop_reason (so the audit log matches the wire) and before
       * anthropic_response_from_parsed renders the reply. Gated internally on
       * gateway_prevent_subagents (the predicate + the police function are both
       * no-ops at the default config). Drop count is plumbed through to the
       * same pipeline-runner total the request-side strip already emits; a
       * future P2b audit pass surfaces both at once. */
      gateway_policy_police_parsed_response(&parsed);

      /* Cost accounting: the Anthropic /v1/messages ingress is a raw provider
       * proxy (no agent_log_call), so record this turn's spend, billed against
       * the served model and tagged as ingress. */
      agent_result_t ar;
      memset(&ar, 0, sizeof(ar));
      snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
      snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
      /* Claude Code's requested model is recorded separately; aimee serves its
       * configured primary, so requested and served typically differ. */
      snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model ? model : "");
      snprintf(ar.stop_reason, sizeof(ar.stop_reason), "%s", parsed.stop_reason);
      ar.prompt_tokens = parsed.prompt_tokens;
      ar.completion_tokens = parsed.completion_tokens;
      ar.cache_write_tokens = parsed.cache_write_tokens;
      ar.cache_read_tokens = parsed.cache_read_tokens;
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&ar, "", "anthropic-ingress");
   }

   mint_msg_id(msg_id, sizeof(msg_id));
   out = anthropic_response_from_parsed(msg_id, model, &parsed);
   rc = write_json(out, resp, cap);
   status = rc;

   agent_free_parsed_response(&parsed);

cleanup:
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
   anthropic_stream_xlate_t *xl; /* legacy incremental translator */
   /* Slice 5-wire: neutral IR-delta relay (used instead of `xl` when the
    * AIMEE_IR_STREAM_RELAY flag is on). Provider OpenAI-chat SSE chunks ->
    * openai_chunk_to_deltas -> anthropic_delta_emit -> `emit`. */
   int ir_relay;
   openai_stream_state_t ir_ost;
   anthropic_stream_state_t ir_ast;
   server_http_sse_event_emit emit;
   void *emit_ctx;
   const char *msg_id;
   const char *model;
   long ir_usage_out; /* tapped from the TURN_STOP delta for the cost row */
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
   /* Usage observed by tapping the relayed Anthropic SSE (message_start carries
    * input + cache tokens, message_delta the final output_tokens), for the
    * ingress cost row written after the stream. The relayed bytes are unchanged. */
   int input_tokens;
   int output_tokens;
   int cache_write_tokens;
   int cache_read_tokens;
   /* Gateway-mutation streaming breaker: when this stream carried a reduced payload
    * (gwmc->mutated), an invalid-request error frame disables the session for
    * subsequent turns (§2.5 streaming). NULL when the request was not mutated. */
   gw_mutate_ctx_t *gwmc;
} anthropic_relay_ctx_t;

static int prov_line_cb(const char *line, size_t len, void *ud)
{
   prov_stream_ctx_t *c = (prov_stream_ctx_t *)ud;
   if (len >= 5 && strncmp(line, "data:", 5) == 0)
   {
      const char *p = line + 5;
      while (*p == ' ')
         p++;
      if (c->ir_relay)
      {
         /* OpenAI-chat providers close the stream with a literal `data: [DONE]`
          * sentinel that is not JSON; the finish_reason chunk already produced the
          * IR TURN_STOP, so just skip it. */
         if (strcmp(p, "[DONE]") == 0)
            return 0;
         cJSON *chunk = cJSON_Parse(p);
         if (chunk)
         {
            /* A finish chunk closes EVERY open block (text + up to
             * AIMEE_STREAM_MAX_TOOLS tool blocks) then TURN_STOP in one call, and
             * a chunk carrying many tool_calls opens as many; size the buffer to
             * hold the worst case so the terminal TURN_STOP is never truncated
             * (which would hang the client's SSE reader). */
            aimee_delta_t deltas[2 * AIMEE_STREAM_MAX_TOOLS + 4];
            int n = openai_chunk_to_deltas(chunk, &c->ir_ost, deltas,
                                           (int)(sizeof deltas / sizeof deltas[0]));
            for (int i = 0; i < n; i++)
            {
               if (deltas[i].type == AIMEE_DELTA_TURN_STOP)
                  c->ir_usage_out = deltas[i].usage_out;
               anthropic_delta_emit(&deltas[i], &c->ir_ast, c->msg_id, c->model, c->emit,
                                    c->emit_ctx);
            }
            cJSON_Delete(chunk);
         }
      }
      else
         anthropic_stream_feed_openai(c->xl, p); /* line is NUL-terminated by the parser */
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
      relay_capture_usage(c, event, c->data);
      c->emit(c->emit_ctx, event, c->data);
      c->emitted++;
      /* Inspect-as-forward (§2.5 streaming): the frame is already forwarded above;
       * if this MUTATED stream carried an invalid-request-class error frame, the
       * reduced payload is the likely cause -> disable subsequent turns. Transient
       * frames (rate-limit/overloaded) are forwarded WITHOUT disabling. */
      if (c->gwmc && c->gwmc->mutated && strcmp(event, "error") == 0 &&
          gw_stream_anthropic_error_is_invalid_request(c->data))
         gw_stream_disable(c->gwmc, "stream_invalid_request");
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

static int messages_stream(const char *body, server_http_sse_event_emit emit, void *ctx)
{
   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC); /* shadow (Slice 3), gated no-op */
   agent_config_t acfg;
   agent_t *ag = req ? resolve_primary(&acfg) : NULL;
   cJSON *messages = NULL, *tools = NULL;
   char *system_text = NULL, *prov_body = NULL;
   char url[MAX_ENDPOINT_LEN + 64];
   char auth[MAX_API_KEY_LEN + 32];
   char extra[512];
   char msg_id[48];
   const char *model = req ? jo_cstr(req, "model") : "";
   const delegate_driver_t *driver;
   anthropic_stream_xlate_t *xl;
   prov_stream_ctx_t pc;
   int input_est;
   int responses_wire = 0;
   gw_mutate_ctx_t gwmc;
   gw_mutate_ctx_init(&gwmc);

   mint_msg_id(msg_id, sizeof(msg_id));

   /* On any setup failure still emit a well-formed (empty) Anthropic stream so
    * the client's SSE reader terminates cleanly rather than hanging. */
   if (!req || !ag)
   {
      xl = anthropic_stream_begin(msg_id, model, 0, emit, ctx);
      if (xl)
      {
         anthropic_stream_finish(xl);
         anthropic_stream_free(xl);
      }
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
      xl = anthropic_stream_begin(msg_id, model, 0, emit, ctx);
      if (xl)
      {
         anthropic_stream_finish(xl);
         anthropic_stream_free(xl);
      }
      goto cleanup;
   }
   /* Re-read `model`: the model-pin stage may have replaced req's "model" node, so
    * the pointer cached at the top of this function could now dangle. */
   model = jo_cstr(req, "model");

   /* Economizer gateway MUTATION (streaming). Reduces req["messages"] in place before
    * translate/build. Streaming CANNOT restore-resend (the 200 is committed on the
    * first byte), so on a bad reduction only SUBSEQUENT turns are circuit-broken —
    * via the SSE error-frame inspect-as-forward in relay_flush + the post-stream
    * fail-safe below. Default-OFF dark no-op. */
   if (gw_mutate_is_enabled())
   {
      char *mut_sys = anthropic_system_to_text(req);
      gw_buffered_mutate(req, "messages", model, mut_sys, server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(), &gwmc);
      free(mut_sys);
   }

   translate_request(req, driver, ag, &messages, &tools, &system_text);
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

   if (driver_is_anthropic(driver))
      prov_body = build_anthropic_provider_body(req, ag, 1, parity);
   else
   {
      /* Slice 5 (streaming): build the provider request VIA THE IR when the flag is
       * on (Claude Code streams, so this is the path that matters for the codex
       * case); legacy fallback on failure / flag off. For codex the reply is
       * fetched buffered + replayed as Anthropic SSE below, so no IR-delta stream
       * translation is needed here. */
      /* Whether we ask the UPSTREAM to stream is decided by how we will read its
       * reply, NOT by what the client asked for. When the buffered replay below
       * takes over (prevent-subagents, or a /responses wire), we fetch the reply to
       * completion; asking for SSE there and then cJSON_Parse'ing it is exactly the
       * "primary provider returned an unparseable reply" bug. /responses is the
       * exception: codex requires stream=true and its replay parses the raw SSE
       * text, so it keeps the flag. */
      int buffered_replay = gateway_prevent_subagents_enabled() || responses_wire;
      int upstream_stream = responses_wire ? 1 : (buffered_replay ? 0 : 1);
      if (aimee_ir_path_enabled())
         prov_body = aimee_ir_build_provider_body(
             req, driver->name, ag->model,
             agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)), upstream_stream);
      /* Shadow: build what LEGACY would have sent for this same request and compare
       * byte-for-byte. This is the evidence that retires the translators — proving
       * the IR sends the provider the same bytes, not merely that it "worked". Off
       * unless AIMEE_IR_SHADOW is set: the extra build is real work. */
      if (prov_body && aimee_ir_shadow_enabled())
      {
         char *legacy_body =
             build_provider_body(driver, ag, messages, tools, system_text,
                                 agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
                                 jo_num(req, "temperature", 1.0), upstream_stream);
         aimee_ir_shadow_compare_bodies(prov_body, legacy_body, AIMEE_WIRE_ANTHROPIC);
         free(legacy_body);
      }
      if (!prov_body)
      {
         /* Served by the LEGACY translator. Counted at the call site, not inside the
          * IR builder: the *_FAIL counters record that a stage failed, this records
          * that a real request was answered by legacy. Deleting the translators is
          * gated on this reading 0 over a live window. */
         aimee_ir_metric_inc(AIMEE_IR_M_LEGACY_FALLBACK, AIMEE_WIRE_ANTHROPIC);
         prov_body = build_provider_body(driver, ag, messages, tools, system_text,
                                         agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
                                         jo_num(req, "temperature", 1.0), upstream_stream);
      }
   }

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
   if (gateway_prevent_subagents_enabled() || responses_wire)
   {
      char *buf_resp = NULL;
      int buf_status;
      parsed_response_t parsed;
      int raw_responses = responses_wire;
      memset(&parsed, 0, sizeof(parsed));
      buf_status = agent_http_post(url, auth, prov_body ? prov_body : "{}", &buf_resp,
                                   ag->timeout_ms, extra[0] ? extra : NULL);
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
               goto cleanup;
            }
            gateway_policy_police_parsed_response(&parsed);
            emit_message_as_sse(&parsed, msg_id, model, emit, ctx);
            /* Cost accounting (mirror the buffered path). */
            if ((parsed.prompt_tokens > 0 || parsed.completion_tokens > 0) &&
                agent_ingress_accounting_enabled())
            {
               agent_result_t ar;
               memset(&ar, 0, sizeof(ar));
               snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
               snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
               snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model ? model : "");
               snprintf(ar.stop_reason, sizeof(ar.stop_reason), "%s", parsed.stop_reason);
               ar.prompt_tokens = parsed.prompt_tokens;
               ar.completion_tokens = parsed.completion_tokens;
               ar.cache_write_tokens = parsed.cache_write_tokens;
               ar.cache_read_tokens = parsed.cache_read_tokens;
               agent_record_token_audit(&ar, "", "anthropic-ingress");
            }
            agent_free_parsed_response(&parsed);
         }
         else
         {
            cJSON *provider_resp = cJSON_Parse(buf_resp);
            if (provider_resp)
            {
               if (driver && driver->parse_response)
                  driver->parse_response(provider_resp, buf_resp, &parsed);
               else
                  /* No driver: default to the openai wire via the IR. */
                  agent_ir_parse_json_response(provider_resp, 0 /*openai*/, -1, NULL, &parsed);
               gateway_policy_police_parsed_response(&parsed);
               emit_message_as_sse(&parsed, msg_id, model, emit, ctx);
               /* Cost accounting (mirror the buffered path). */
               if ((parsed.prompt_tokens > 0 || parsed.completion_tokens > 0) &&
                   agent_ingress_accounting_enabled())
               {
                  agent_result_t ar;
                  memset(&ar, 0, sizeof(ar));
                  snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
                  snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
                  snprintf(ar.requested_model, sizeof(ar.requested_model), "%s",
                           model ? model : "");
                  snprintf(ar.stop_reason, sizeof(ar.stop_reason), "%s", parsed.stop_reason);
                  ar.prompt_tokens = parsed.prompt_tokens;
                  ar.completion_tokens = parsed.completion_tokens;
                  ar.cache_write_tokens = parsed.cache_write_tokens;
                  ar.cache_read_tokens = parsed.cache_read_tokens;
                  agent_record_token_audit(&ar, "", "anthropic-ingress");
               }
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
         /* Buffered-replay streaming: circuit-break subsequent turns only when the
          * non-200 indicates a bad reduced payload — an invalid-request-class 4xx
          * (400/413/422) or, fail-safe, a 5xx. Auth / rate-limit / not-found
          * (401/403/404/429) are NOT reduction bugs and are forwarded WITHOUT
          * disabling. No restore/resend — the 200 is already committed. */
         if (gw_status_is_invalid_request(buf_status))
            gw_stream_disable(&gwmc, "stream_invalid_request");
         else if (buf_status / 100 == 5)
            gw_stream_disable(&gwmc, "stream_decoder_error");
      }
      free(buf_resp);
      goto cleanup;
   }

   if (driver_is_anthropic(driver))
   {
      anthropic_relay_ctx_t relay;
      int stream_status;
      memset(&relay, 0, sizeof(relay));
      sse_parser_init(&relay.parser);
      relay.emit = emit;
      relay.emit_ctx = ctx;
      relay.gwmc = &gwmc; /* enable the streaming breaker for a mutated stream */
      stream_status =
          agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", anthropic_relay_chunk_cb,
                                 &relay, ag->timeout_ms, extra[0] ? extra : NULL);
      relay_flush(&relay);
      if (stream_status != 200)
      {
         relay_emit_transport_error(&relay, stream_status);
         /* Fail-safe (§2.5): a non-SSE post-200 upstream failure on a mutated stream
          * disables subsequent turns (the current turn is already lost). */
         gw_stream_disable(&gwmc, "stream_decoder_error");
      }
      /* Cost accounting for the native Anthropic streaming ingress, from the
       * usage tapped off the relayed SSE. A clean 200 is realized spend; an
       * aborted stream that still observed usage is recorded as partial so the
       * observed tokens are not silently lost. */
      if ((relay.input_tokens > 0 || relay.output_tokens > 0) && agent_ingress_accounting_enabled())
      {
         agent_result_t ar;
         memset(&ar, 0, sizeof(ar));
         snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
         snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
         snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model ? model : "");
         ar.prompt_tokens = relay.input_tokens;
         ar.completion_tokens = relay.output_tokens;
         ar.cache_write_tokens = relay.cache_write_tokens;
         ar.cache_read_tokens = relay.cache_read_tokens;
         agent_record_token_audit_kind(&ar, "", "anthropic-ingress",
                                       stream_status == 200 ? "realized" : "partial");
      }
      sse_parser_free(&relay.parser);
      free(relay.data);
      goto cleanup;
   }

   input_est = messages ? session_compact_estimate_tokens(messages) : 0;

   if (aimee_ir_stream_relay_enabled())
   {
      /* Slice 5-wire (default-off): drive the incremental OpenAI-chat -> Anthropic
       * SSE relay through the neutral IR-delta model instead of the legacy
       * anthropic_stream_feed_openai translator -- the last live direct-translation
       * site. */
      memset(&pc, 0, sizeof pc);
      sse_parser_init(&pc.parser);
      pc.ir_relay = 1;
      openai_stream_state_init(&pc.ir_ost);
      pc.emit = emit;
      pc.emit_ctx = ctx;
      pc.msg_id = msg_id;
      pc.model = model;
      int ir_status = agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", prov_chunk_cb,
                                             &pc, ag->timeout_ms, extra[0] ? extra : NULL);
      sse_parser_free(&pc.parser);
      /* Finish-safety: if the upstream cut off before a finish_reason chunk (no IR
       * TURN_STOP was produced), synthesize the closing sequence so the client's
       * SSE reader terminates cleanly, mirroring anthropic_stream_finish. Close any
       * still-open content blocks, then emit TURN_STOP. */
      if (pc.ir_ast.started && !pc.ir_ost.stopped)
      {
         aimee_delta_t bs = {0};
         bs.type = AIMEE_DELTA_BLOCK_STOP;
         if (pc.ir_ost.text_block >= 0)
         {
            bs.block_id = pc.ir_ost.text_block;
            anthropic_delta_emit(&bs, &pc.ir_ast, msg_id, model, emit, ctx);
         }
         for (int i = 0; i < AIMEE_STREAM_MAX_TOOLS; i++)
            if (pc.ir_ost.tool_block[i] >= 0)
            {
               bs.block_id = pc.ir_ost.tool_block[i];
               anthropic_delta_emit(&bs, &pc.ir_ast, msg_id, model, emit, ctx);
            }
         aimee_delta_t ts = {0};
         ts.type = AIMEE_DELTA_TURN_STOP;
         ts.stop_reason = AIMEE_STOP_END_TURN;
         ts.usage_out = pc.ir_usage_out;
         anthropic_delta_emit(&ts, &pc.ir_ast, msg_id, model, emit, ctx);
      }
      /* Cost row: input from the local estimate (message_start reports 0, same as
       * the legacy translator's estimate basis), output tapped from the IR
       * TURN_STOP delta. */
      if ((input_est > 0 || pc.ir_usage_out > 0) && agent_ingress_accounting_enabled())
      {
         agent_result_t ar;
         memset(&ar, 0, sizeof(ar));
         snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
         snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
         snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model ? model : "");
         ar.prompt_tokens = input_est;
         ar.completion_tokens = (int)pc.ir_usage_out;
         agent_record_token_audit_kind(&ar, "", "anthropic-ingress",
                                       ir_status == 200 ? "realized" : "partial");
      }
      goto cleanup;
   }

   xl = anthropic_stream_begin(msg_id, model, input_est, emit, ctx);
   if (!xl)
      goto cleanup;

   sse_parser_init(&pc.parser);
   pc.xl = xl;
   int xlate_status = agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", prov_chunk_cb,
                                             &pc, ag->timeout_ms, extra[0] ? extra : NULL);
   sse_parser_free(&pc.parser);

   /* OpenAI-via-translator streaming: this path lacks per-frame Anthropic error
    * classification, so a non-200 upstream on a mutated request is a fail-safe
    * disable of subsequent turns. */
   if (xlate_status != 200)
      gw_stream_disable(&gwmc, "stream_decoder_error");

   anthropic_stream_finish(xl);

   /* Cost accounting for the OpenAI-via-translator streaming ingress: tap the
    * usage captured off the upstream OpenAI stream (prompt count preferring the
    * upstream-reported value over the estimate, plus completion and cached
    * tokens) and write one ingress cost row, mirroring the native relay path. A
    * clean 200 is realized; an aborted stream with observed usage is partial. */
   {
      int in_tok = 0, out_tok = 0, cr_tok = 0;
      anthropic_stream_get_usage(xl, &in_tok, &out_tok, &cr_tok);
      if ((in_tok > 0 || out_tok > 0) && agent_ingress_accounting_enabled())
      {
         agent_result_t ar;
         memset(&ar, 0, sizeof(ar));
         snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
         snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
         snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model ? model : "");
         ar.prompt_tokens = in_tok;
         ar.completion_tokens = out_tok;
         ar.cache_read_tokens = cr_tok;
         agent_record_token_audit_kind(&ar, "", "anthropic-ingress",
                                       xlate_status == 200 ? "realized" : "partial");
      }
   }

   anthropic_stream_free(xl);

cleanup:
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

   /* Proxy to Anthropic's real /v1/messages/count_tokens so Claude Code's
    * context-budget math matches api.anthropic.com, not a local estimate — only
    * when the primary speaks the Anthropic API; otherwise fall through to the
    * estimate. */
   {
      agent_config_t acfg;
      agent_t *ag = resolve_primary(&acfg);
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
   cJSON *messages = req ? anthropic_messages_to_openai(
                               cJSON_GetObjectItemCaseSensitive(req, "messages"), system_text)
                         : NULL;
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
