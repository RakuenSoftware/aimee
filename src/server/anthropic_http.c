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
#include "agent_exec.h"
#include "agent_protocol.h"
#include "agent_types.h"
#include "anthropic_ingress.h"
#include "cJSON.h"
#include "delegate_driver.h"
#include "ingress_preinject.h"
#include "json_fluent.h"
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

/* Write an Anthropic-shaped error object into resp and return `status`. */
static int write_error(char *resp, int cap, int status, const char *type, const char *message)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *e;
   cJSON_AddStringToObject(o, "type", "error");
   e = cJSON_AddObjectToObject(o, "error");
   cJSON_AddStringToObject(e, "type", type);
   cJSON_AddStringToObject(e, "message", message);
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
   *out_tools = anthropic_tools_to_openai(cJSON_GetObjectItemCaseSensitive(req, "tools"));
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

/* P1 context pre-injection for the Anthropic /v1/messages ingress. Builds the
 * <aimee-context> envelope from this turn's query and folds it into the request's
 * `system` so BOTH the Anthropic-native passthrough (which duplicates `req`) and
 * the translated-provider path (which flattens `req`'s system via
 * anthropic_system_to_text) carry it. Mutates `req` in place, so it must run
 * before translate_request / build_*_provider_body. Appended as a trailing system
 * text block (array form) so a cached system prefix — Claude Code sends
 * cache_control'd system blocks — stays stable and prompt caching still hits.
 * No-op when pre-injection is disabled (config or the x-aimee-preinject:0 header,
 * via the thread-local checked in ingress_preinject_build) or recall is empty. */
static void messages_apply_preinject(cJSON *req)
{
   char *query =
       ingress_preinject_query_from_messages(cJSON_GetObjectItemCaseSensitive(req, "messages"));
   if (!query)
      return;
   char *env = ingress_preinject_build(query, 0);
   free(query);
   if (!env)
      return;

   cJSON *sys = cJSON_GetObjectItemCaseSensitive(req, "system");
   if (cJSON_IsArray(sys))
   {
      cJSON *blk = cJSON_CreateObject();
      if (blk)
      {
         cJSON_AddStringToObject(blk, "type", "text");
         cJSON_AddStringToObject(blk, "text", env);
         cJSON_AddItemToArray(sys, blk);
      }
   }
   else if (cJSON_IsString(sys) && sys->valuestring && sys->valuestring[0])
   {
      size_t n = strlen(sys->valuestring) + 2 + strlen(env) + 1;
      char *joined = malloc(n);
      if (joined)
      {
         snprintf(joined, n, "%s\n\n%s", sys->valuestring, env);
         cJSON_ReplaceItemInObjectCaseSensitive(req, "system", cJSON_CreateString(joined));
         free(joined);
      }
   }
   else
   {
      /* system absent or empty: the envelope becomes the system prompt. */
      cJSON_DeleteItemFromObjectCaseSensitive(req, "system");
      cJSON_AddStringToObject(req, "system", env);
   }
   free(env);
}

/* --- Buffered: POST /v1/messages (stream:false) ------------------------- */

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
   parsed_response_t parsed;
   int status, http_status, rc;
   const char *model;

   if (!req)
      return write_error(resp, cap, 400, "invalid_request_error", "invalid JSON body");
   model = jo_cstr(req, "model");

   ag = resolve_primary(&acfg);
   if (!ag)
   {
      cJSON_Delete(req);
      return write_error(resp, cap, 503, "api_error", "no primary agent configured");
   }

   delegate_drivers_init();
   driver = delegate_driver_get(ag->provider);
   /* Exact-parity passthrough only applies to the Anthropic-native path. */
   int parity = driver_is_anthropic(driver);
   if (!parity)
      messages_apply_preinject(req);
   translate_request(req, driver, ag, &messages, &tools, &system_text);
   if (delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
   {
      status = write_error(resp, cap, 502, "api_error", "failed to reach the primary provider");
      goto cleanup;
   }
   if (parity)
      build_anthropic_parity_headers(extra, sizeof(extra));
   else
      agent_build_extra_headers(ag, extra, sizeof(extra));

   if (driver_is_anthropic(driver))
      prov_body = build_anthropic_provider_body(req, ag, 0, parity);
   else
      prov_body = build_provider_body(driver, ag, messages, tools, system_text,
                                      agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
                                      jo_num(req, "temperature", 1.0), 0);
   http_status = agent_http_post(url, auth, prov_body ? prov_body : "{}", &response, ag->timeout_ms,
                                 extra[0] ? extra : NULL);
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
         status = write_error(resp, cap, 502, "api_error",
                              response ? response : "primary provider call failed");
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
         agent_parse_response_openai(provider_resp, &parsed);

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
   anthropic_stream_xlate_t *xl;
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
} anthropic_relay_ctx_t;

static int prov_line_cb(const char *line, size_t len, void *ud)
{
   prov_stream_ctx_t *c = (prov_stream_ctx_t *)ud;
   if (len >= 5 && strncmp(line, "data:", 5) == 0)
   {
      const char *p = line + 5;
      while (*p == ' ')
         p++;
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
   if (!parity)
      messages_apply_preinject(req);
   translate_request(req, driver, ag, &messages, &tools, &system_text);
   if (delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
      goto cleanup;
   if (parity)
      build_anthropic_parity_headers(extra, sizeof(extra));
   else
      agent_build_extra_headers(ag, extra, sizeof(extra));

   if (driver_is_anthropic(driver))
      prov_body = build_anthropic_provider_body(req, ag, 1, parity);
   else
      prov_body = build_provider_body(driver, ag, messages, tools, system_text,
                                      agent_request_max_tokens(ag, jo_int(req, "max_tokens", 0)),
                                      jo_num(req, "temperature", 1.0), 1);

   if (driver_is_anthropic(driver))
   {
      anthropic_relay_ctx_t relay;
      int stream_status;
      memset(&relay, 0, sizeof(relay));
      sse_parser_init(&relay.parser);
      relay.emit = emit;
      relay.emit_ctx = ctx;
      stream_status =
          agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", anthropic_relay_chunk_cb,
                                 &relay, ag->timeout_ms, extra[0] ? extra : NULL);
      relay_flush(&relay);
      if (stream_status != 200)
         relay_emit_transport_error(&relay, stream_status);
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
   xl = anthropic_stream_begin(msg_id, model, input_est, emit, ctx);
   if (!xl)
      goto cleanup;

   sse_parser_init(&pc.parser);
   pc.xl = xl;
   int xlate_status = agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", prov_chunk_cb,
                                             &pc, ag->timeout_ms, extra[0] ? extra : NULL);
   sse_parser_free(&pc.parser);

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
