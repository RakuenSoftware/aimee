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

/* Translate an Anthropic request into the provider's OpenAI-shaped messages +
 * tools. *out_system is a malloc'd flattened system prompt (caller frees). */
static void translate_request(const cJSON *req, cJSON **out_messages, cJSON **out_tools,
                              char **out_system)
{
   *out_system = anthropic_system_to_text(req);
   *out_messages =
       anthropic_messages_to_openai(cJSON_GetObjectItemCaseSensitive(req, "messages"), *out_system);
   *out_tools = anthropic_tools_to_openai(cJSON_GetObjectItemCaseSensitive(req, "tools"));
}

/* Build the provider request body (OpenAI chat shape). `stream` toggles the
 * streaming flag. Returns a malloc'd JSON string (caller frees), or NULL. */
static char *build_provider_body(const agent_t *ag, cJSON *messages, cJSON *tools, int max_tokens,
                                 double temperature, int stream)
{
   cJSON *req = agent_build_request_openai((agent_t *)ag, messages, tools, max_tokens, temperature);
   char *body;
   if (!req)
      return NULL;
   if (stream)
      cJSON_AddBoolToObject(req, "stream", 1);
   body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   return body;
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

   translate_request(req, &messages, &tools, &system_text);

   delegate_drivers_init();
   driver = delegate_driver_get(ag->provider);
   if (delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
   {
      status = write_error(resp, cap, 502, "api_error", "failed to reach the primary provider");
      goto cleanup;
   }
   agent_build_extra_headers(ag, extra, sizeof(extra));

   prov_body = build_provider_body(ag, messages, tools, jo_int(req, "max_tokens", 4096),
                                   jo_num(req, "temperature", 1.0), 0);
   http_status = agent_http_post(url, auth, prov_body ? prov_body : "{}", &response, ag->timeout_ms,
                                 extra[0] ? extra : NULL);
   if (http_status != 200 || !response)
   {
      status = write_error(resp, cap, 502, "api_error",
                           response ? response : "primary provider call failed");
      goto cleanup;
   }

   provider_resp = cJSON_Parse(response);
   memset(&parsed, 0, sizeof(parsed));
   if (provider_resp)
      agent_parse_response_openai(provider_resp, &parsed);

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

   translate_request(req, &messages, &tools, &system_text);
   input_est = messages ? session_compact_estimate_tokens(messages) : 0;
   xl = anthropic_stream_begin(msg_id, model, input_est, emit, ctx);

   delegate_drivers_init();
   driver = delegate_driver_get(ag->provider);
   if (!xl || delegate_build_url(driver, ag, url, sizeof(url)) != 0 ||
       agent_resolve_auth(ag, auth, sizeof(auth)) != 0)
   {
      if (xl)
      {
         anthropic_stream_finish(xl);
         anthropic_stream_free(xl);
      }
      goto cleanup;
   }
   agent_build_extra_headers(ag, extra, sizeof(extra));

   prov_body = build_provider_body(ag, messages, tools, jo_int(req, "max_tokens", 4096),
                                   jo_num(req, "temperature", 1.0), 1);

   sse_parser_init(&pc.parser);
   pc.xl = xl;
   agent_http_post_stream(url, auth, prov_body ? prov_body : "{}", prov_chunk_cb, &pc,
                          ag->timeout_ms, extra[0] ? extra : NULL);
   sse_parser_free(&pc.parser);

   anthropic_stream_finish(xl);
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

void anthropic_http_register(void)
{
   server_http_set_messages_handler(messages_buffered);
   server_http_set_messages_stream_handler(messages_stream);
   server_http_set_count_tokens_handler(count_tokens);
}
