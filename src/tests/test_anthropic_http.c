/* test_anthropic_http.c: pure tests for file-local Anthropic HTTP ingress
 * helpers. This intentionally includes anthropic_http.c so production helpers
 * can remain private while buffer/parsing behavior stays unit-tested. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/aimee.h"
#include "../headers/agent_config.h"
#include "../headers/agent_exec.h"
#include "../headers/agent_protocol.h"
#include "../headers/delegate_driver.h"
#include "../headers/log.h"
#include "../headers/server_http.h"
#include "../vendor/headers/cJSON.h"

/* anthropic_http.c's write_error now logs aimee-internal error codes; this
 * minimal link stubs the logger like the other production deps below. */
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

/* write_error's route-unresolved branch reads the per-turn auth-error channel;
 * stub returns "no explicit reason" for this minimal link. */
const char *agent_request_auth_error(void)
{
   return NULL;
}

#define PASS(name) printf("  PASS: %s\n", (name))

static const delegate_driver_t *g_driver;
static char *g_last_body;
static char *g_last_extra; /* upstream extra-header block from the last post */
static int g_stream_status = 200;
static const char *g_stream_payload;
static const char *g_response_body = NULL;
static int g_response_status = 200;

static void reset_capture(void)
{
   free(g_last_body);
   g_last_body = NULL;
   free(g_last_extra);
   g_last_extra = NULL;
   g_stream_status = 200;
   g_stream_payload = NULL;
   g_response_body = NULL;
   g_response_status = 200;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   snprintf(cfg->agents[0].name, sizeof(cfg->agents[0].name), "primary");
   snprintf(cfg->agents[0].provider, sizeof(cfg->agents[0].provider), "%s",
            g_driver && g_driver->name ? g_driver->name : "anthropic");
   snprintf(cfg->agents[0].model, sizeof(cfg->agents[0].model), "configured-claude");
   cfg->agents[0].timeout_ms = 1;
   return 0;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   (void)name;
   return cfg && cfg->agent_count ? &cfg->agents[0] : NULL;
}

agent_t *agent_default_primary(agent_config_t *cfg)
{
   return cfg && cfg->agent_count ? &cfg->agents[0] : NULL;
}

void delegate_drivers_init(void)
{
}

const delegate_driver_t *delegate_driver_get(const char *provider)
{
   (void)provider;
   return g_driver;
}

void delegate_get_caps(const delegate_driver_t *driver, const agent_t *agent, driver_caps_t *caps)
{
   memset(caps, 0, sizeof(*caps));
   if (driver && driver->get_caps)
   {
      driver->get_caps(agent, caps);
      return;
   }
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING;
   caps->context_limit = DRIVER_CTX_LARGE;
}

int delegate_build_url(const delegate_driver_t *driver, const agent_t *agent, char *url,
                       size_t url_len)
{
   (void)driver;
   (void)agent;
   if (g_driver && g_driver->name && strcmp(g_driver->name, "chatgpt") == 0)
      snprintf(url, url_len, "https://example.invalid/v1/responses");
   else
      snprintf(url, url_len, "https://example.invalid/v1/messages");
   return 0;
}

int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len)
{
   (void)agent;
   snprintf(buf, buf_len, "Bearer test");
   return 0;
}

void agent_build_extra_headers(const agent_t *agent, char *buf, size_t buf_len)
{
   (void)agent;
   if (buf_len)
      buf[0] = '\0';
}

cJSON *agent_build_request_openai(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  int max_tokens, double temperature)
{
   cJSON *out = cJSON_CreateObject();
   (void)agent;
   (void)messages;
   (void)tools;
   (void)max_tokens;
   (void)temperature;
   cJSON_AddStringToObject(out, "model", "openai-test");
   return out;
}

/* Stub: the ingress passes the incoming request's max_tokens through this
 * resolver; an explicit value wins, otherwise a model-derived ceiling applies. */
int agent_request_max_tokens(const agent_t *agent, int requested)
{
   (void)agent;
   return requested > 0 ? requested : 8192;
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   free(g_last_body);
   g_last_body = strdup(body ? body : "");
   assert(g_last_body != NULL);
   free(g_last_extra);
   g_last_extra = strdup(extra_headers ? extra_headers : "");
   *response_buf = strdup(g_response_body ? g_response_body : "{}");
   return g_response_status;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   free(g_last_body);
   g_last_body = strdup(body ? body : "");
   assert(g_last_body != NULL);
   if (g_stream_payload && callback)
      assert(callback(g_stream_payload, strlen(g_stream_payload), userdata) == 0);
   return g_stream_status;
}

int agent_ir_parse_json_response(cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                 parsed_response_t *out)
{
   (void)root;
   (void)anthropic;
   (void)rescue_mode;
   if (n_rescued)
      *n_rescued = 0;
   memset(out, 0, sizeof(*out));
   return 0;
}

static void parsed_responses(cJSON *root, const char *body, parsed_response_t *out)
{
   const char *p;
   char text[512];

   (void)root;
   memset(out, 0, sizeof(*out));
   if (!body)
      return;

   p = strstr(body, "event: response.output_text.delta");
   if (p)
   {
      p = strstr(p, "\"delta\":\"");
      if (p)
      {
         const char *start = p + strlen("\"delta\":\"");
         const char *end = strchr(start, '"');
         size_t n = end && end > start ? (size_t)(end - start) : 0;
         if (n >= sizeof(text))
            n = sizeof(text) - 1;
         if (n > 0)
         {
            memcpy(text, start, n);
            text[n] = '\0';
            out->content = strdup(text);
         }
      }
   }

   p = strstr(body, "event: response.completed");
   if (p)
   {
      const char *usage = strstr(p, "\"usage\":{");
      if (usage)
      {
         const char *it = strstr(usage, "\"input_tokens\":");
         const char *ot = strstr(usage, "\"output_tokens\":");
         if (it)
            out->prompt_tokens = atoi(it + strlen("\"input_tokens\":"));
         if (ot)
            out->completion_tokens = atoi(ot + strlen("\"output_tokens\":"));
      }
   }
}

void agent_free_parsed_response(parsed_response_t *p)
{
   if (!p)
      return;
   free(p->content);
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
   cJSON_Delete(p->assistant_message);
}

int session_compact_estimate_tokens(cJSON *messages)
{
   return messages ? cJSON_GetArraySize(messages) : 0;
}

void server_http_set_messages_handler(server_http_completion_fn fn)
{
   (void)fn;
}
void server_http_set_messages_stream_handler(server_http_responses_stream_fn fn)
{
   (void)fn;
}
void server_http_set_count_tokens_handler(server_http_completion_fn fn)
{
   (void)fn;
}

/* The ingress cost write is exercised by test_token_audit via the shared helper;
 * here we only validate the SSE usage tap, so no-op stubs suffice. */
void agent_record_token_audit(const agent_result_t *result, const char *role, const char *source)
{
   (void)result;
   (void)role;
   (void)source;
}
void agent_record_token_audit_kind(const agent_result_t *result, const char *role,
                                   const char *source, const char *usage_kind)
{
   (void)result;
   (void)role;
   (void)source;
   (void)usage_kind;
}
/* Enable accounting in this unit so the tap/record path is exercised. */
int agent_ingress_accounting_enabled(void)
{
   return 1;
}

/* Pre-injection stubs. query_from_messages returns a non-NULL query when a turn
 * is present so messages_apply_preinject proceeds; build returns the per-test
 * envelope (default NULL = no-op, so the passthrough/shape tests are unaffected).
 * The injection-coverage test sets g_stub_preinject_env. */
static char *g_stub_preinject_env = NULL;
char *ingress_preinject_query_from_messages(const cJSON *messages)
{
   return messages ? strdup("q") : NULL;
}
char *ingress_preinject_build(const char *query, int request_disabled)
{
   (void)query;
   (void)request_disabled;
   return g_stub_preinject_env ? strdup(g_stub_preinject_env) : NULL;
}
/* Link-only: the OpenAI branch of the shared gw_stage_memory references this, but
 * these /v1/messages whitebox tests only exercise the Anthropic branch. */
char *ingress_preinject_apply(const char *instructions, const char *envelope)
{
   (void)instructions;
   return envelope ? strdup(envelope) : NULL;
}
/* messages_run_request_pipeline reads config for the P5 anthropic-inject opt-in;
 * these whitebox tests run with it off (zeroed). */
int config_load(config_t *cfg)
{
   if (cfg)
      memset(cfg, 0, sizeof(*cfg));
   return 0;
}

/* HTTP-layer stub: agent_http_last_retry_after has no upstream socket here, so 0
 * (no Retry-After) suffices. */
int agent_http_last_retry_after(void)
{
   return 0;
}
/* Gateway policy is no-op in these whitebox shape tests (its own behavior is
 * covered by test_gateway_policy); keeps the request shape unaltered. */
int gateway_policy_apply_request(cJSON *req, int tools_openai_shape)
{
   (void)req;
   (void)tools_openai_shape;
   return 0;
}
int gateway_policy_pin_model(cJSON *req, const char *agent_model)
{
   (void)req;
   (void)agent_model;
   return 0; /* pin is off in these shape tests; covered by test_gateway_policy */
}
/* P2c streaming branch gate: off by default in these whitebox shape tests
 * (the real predicate is exercised by test_anthropic_http-p2c). */
int gateway_prevent_subagents_enabled(void)
{
   return 0;
}
/* P2c (response-side tool policing) is exercised by its own dedicated
 * integration test (unit-test-anthropic-http-p2c) which links the real
 * gateway_policy.o. In these whitebox shape tests we stub it to a no-op
 * so the response shape is unaltered. */
int gateway_policy_police_parsed_response(parsed_response_t *p)
{
   (void)p;
   return 0;
}

#include "../server/anthropic_http.c"

typedef struct
{
   char events[8][64];
   char data[8][8192];
   int count;
} emit_capture_t;

static cJSON *parse(const char *json)
{
   cJSON *j = cJSON_Parse(json);
   assert(j != NULL);
   return j;
}

static char *json_of(const cJSON *j)
{
   char *s = cJSON_PrintUnformatted((cJSON *)j);
   assert(s != NULL);
   return s;
}

static const cJSON *obj(const cJSON *parent, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key);
   assert(v != NULL);
   return v;
}

static cJSON *openai_driver_build(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  const char *system_prompt, int max_tokens, double temperature)
{
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "driver", "openai");
   cJSON_AddStringToObject(out, "model", agent->model);
   cJSON_AddItemToObject(out, "messages", cJSON_Duplicate(messages, 1));
   if (tools)
      cJSON_AddItemToObject(out, "tools", cJSON_Duplicate(tools, 1));
   cJSON_AddNumberToObject(out, "max_tokens", max_tokens);
   cJSON_AddNumberToObject(out, "temperature", temperature);
   cJSON_AddStringToObject(out, "system_prompt", system_prompt ? system_prompt : "");
   return out;
}

static cJSON *system_prompt_driver_build(const agent_t *agent, cJSON *messages, cJSON *tools,
                                         const char *system_prompt, int max_tokens,
                                         double temperature)
{
   cJSON *out = cJSON_CreateObject();
   (void)tools;
   (void)max_tokens;
   (void)temperature;
   cJSON_AddStringToObject(out, "driver", "chatgpt");
   cJSON_AddStringToObject(out, "model", agent->model);
   cJSON_AddStringToObject(out, "instructions", system_prompt ? system_prompt : "");
   cJSON_AddItemToObject(out, "input", cJSON_Duplicate(messages, 1));
   return out;
}

static void system_prompt_driver_caps(const agent_t *agent, driver_caps_t *caps)
{
   (void)agent;
   caps->capability_flags = DRIVER_CAP_STREAMING | DRIVER_CAP_SYSTEM_MSG;
   caps->context_limit = DRIVER_CTX_HUGE;
}

static void parsed_text(cJSON *root, const char *body, parsed_response_t *out)
{
   (void)root;
   (void)body;
   memset(out, 0, sizeof(*out));
   out->content = strdup("ok");
   assert(out->content != NULL);
}

static void cap_emit(void *ctx, const char *event, const char *data_json)
{
   emit_capture_t *cap = (emit_capture_t *)ctx;
   assert(cap->count < 8);
   snprintf(cap->events[cap->count], sizeof(cap->events[cap->count]), "%s", event);
   snprintf(cap->data[cap->count], sizeof(cap->data[cap->count]), "%s", data_json);
   cap->count++;
}

static void test_translate_request_anthropic_passthrough(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic"};
   cJSON *req = parse("{\"system\":\"SYS\",\"messages\":[{\"role\":\"user\",\"content\":["
                      "{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
                      "\"content\":\"exact\"}]}],\"tools\":[{\"name\":\"Read\","
                      "\"input_schema\":{\"type\":\"object\"}}]}");
   cJSON *orig_messages = cJSON_GetObjectItemCaseSensitive(req, "messages");
   cJSON *orig_tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   cJSON *messages = NULL;
   cJSON *tools = NULL;
   char *system_text = NULL;
   char *orig_messages_s;
   char *out_messages_s;
   char *orig_tools_s;
   char *out_tools_s;

   translate_request(req, &anthropic, NULL, &messages, &tools, &system_text);

   assert(system_text && strcmp(system_text, "SYS") == 0);
   assert(messages && messages != orig_messages);
   assert(tools && tools != orig_tools);
   orig_messages_s = json_of(orig_messages);
   out_messages_s = json_of(messages);
   orig_tools_s = json_of(orig_tools);
   out_tools_s = json_of(tools);
   assert(strcmp(orig_messages_s, out_messages_s) == 0);
   assert(strcmp(orig_tools_s, out_tools_s) == 0);

   free(orig_messages_s);
   free(out_messages_s);
   free(orig_tools_s);
   free(out_tools_s);
   free(system_text);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   cJSON_Delete(req);
   PASS("translate_request_anthropic_passthrough");
}

static void test_anthropic_relay_round_trip(void)
{
   anthropic_relay_ctx_t relay;
   emit_capture_t cap;
   const char *chunk1 = "event: content_block_delta\n"
                        "data: {\"type\":\"content_block_delta\"";
   const char *chunk2 = "}\n"
                        "data: {\"delta\":{\"text\":\"x\"}}\n\n"
                        "data: {\"type\":\"message_delta\"}\n\n"
                        "event: ping\n"
                        "data: [DONE]\n\n";

   memset(&relay, 0, sizeof(relay));
   memset(&cap, 0, sizeof(cap));
   sse_parser_init(&relay.parser);
   relay.emit = cap_emit;
   relay.emit_ctx = &cap;

   assert(anthropic_relay_chunk_cb(chunk1, strlen(chunk1), &relay) == 0);
   assert(anthropic_relay_chunk_cb(chunk2, strlen(chunk2), &relay) == 0);
   relay_flush(&relay);

   assert(cap.count == 2);
   assert(strcmp(cap.events[0], "content_block_delta") == 0);
   assert(strcmp(cap.data[0], "{\"type\":\"content_block_delta\"}\n{\"delta\":{\"text\":\"x\"}}") ==
          0);
   assert(strcmp(cap.events[1], "message") == 0);
   assert(strcmp(cap.data[1], "{\"type\":\"message_delta\"}") == 0);
   assert(relay.emitted == 2);

   sse_parser_free(&relay.parser);
   free(relay.data);
   PASS("anthropic_relay_round_trip");
}

static void test_anthropic_relay_usage_capture(void)
{
   anthropic_relay_ctx_t relay;
   emit_capture_t cap;
   const char *chunk =
       "event: message_start\n"
       "data: {\"type\":\"message_start\",\"message\":{\"model\":\"claude-3-5-sonnet\","
       "\"usage\":{\"input_tokens\":120,\"cache_creation_input_tokens\":30,"
       "\"cache_read_input_tokens\":10,\"output_tokens\":1}}}\n\n"
       "event: message_delta\n"
       "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":55}}\n\n";

   memset(&relay, 0, sizeof(relay));
   memset(&cap, 0, sizeof(cap));
   sse_parser_init(&relay.parser);
   relay.emit = cap_emit;
   relay.emit_ctx = &cap;

   assert(anthropic_relay_chunk_cb(chunk, strlen(chunk), &relay) == 0);
   relay_flush(&relay);

   /* Usage tapped off the relayed SSE (the relayed bytes are unchanged): input +
    * cache from message_start, final output from message_delta. */
   assert(relay.input_tokens == 120);
   assert(relay.output_tokens == 55);
   assert(relay.cache_write_tokens == 30);
   assert(relay.cache_read_tokens == 10);

   sse_parser_free(&relay.parser);
   free(relay.data);
   PASS("anthropic_relay_usage_capture");
}

static void test_relay_append_data_growth(void)
{
   anthropic_relay_ctx_t relay;
   char big[7000];

   memset(&relay, 0, sizeof(relay));
   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';

   assert(relay_append_data(&relay, big) == 0);
   assert(relay.data_cap >= sizeof(big));
   assert(relay.data_len == strlen(big));
   assert(strcmp(relay.data, big) == 0);

   free(relay.data);
   PASS("relay_append_data_growth");
}

static void test_relay_transport_error(void)
{
   anthropic_relay_ctx_t relay;
   emit_capture_t cap;

   memset(&relay, 0, sizeof(relay));
   memset(&cap, 0, sizeof(cap));
   relay.emit = cap_emit;
   relay.emit_ctx = &cap;

   relay_emit_transport_error(&relay, 599);
   assert(cap.count == 1);
   assert(strcmp(cap.events[0], "error") == 0);
   assert(strstr(cap.data[0], "\"type\":\"error\"") != NULL);
   assert(strstr(cap.data[0], "status 599") != NULL);
   assert(relay.emitted == 1);
   PASS("relay_transport_error");
}

static void test_messages_buffered_anthropic_preserves_request_shape(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *sent;
   const cJSON *system;
   const cJSON *cc;

   reset_capture();
   g_driver = &anthropic;
   assert(
       messages_buffered("{\"model\":\"ignored\",\"max_tokens\":64,"
                         "\"system\":[{\"type\":\"text\",\"text\":\"SYS\","
                         "\"cache_control\":{\"type\":\"ephemeral\"}}],"
                         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                         "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}],"
                         "\"tool_choice\":{\"type\":\"tool\",\"name\":\"Read\"},"
                         "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024},"
                         "\"stop_sequences\":[\"STOP\"],\"top_k\":7}",
                         resp, sizeof(resp)) == 200);
   assert(g_last_body != NULL);
   sent = parse(g_last_body);
   /* Anthropic primary speaks the Anthropic API -> inbound model honored verbatim. */
   assert(strcmp(obj(sent, "model")->valuestring, "ignored") == 0);
   system = obj(sent, "system");
   assert(cJSON_IsArray(system));
   cc = obj(cJSON_GetArrayItem((cJSON *)system, 0), "cache_control");
   assert(strcmp(obj(cc, "type")->valuestring, "ephemeral") == 0);
   assert(cJSON_IsObject(obj(sent, "tool_choice")));
   assert(cJSON_IsObject(obj(sent, "thinking")));
   assert(cJSON_IsArray(obj(sent, "stop_sequences")));
   assert(obj(sent, "top_k")->valueint == 7);
   assert(cJSON_GetObjectItemCaseSensitive(sent, "stream") == NULL);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_anthropic_preserves_request_shape");
}

/* Anthropic primary -> passthrough: inbound model honored, pre-injection skipped,
 * and the client's anthropic-beta forwarded upstream. */
static void test_messages_buffered_anthropic_parity_passthrough(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *sent;

   reset_capture();
   g_driver = &anthropic;
   g_stub_preinject_env = "<aimee-context>INJECTED</aimee-context>";
   anthropic_ingress_set_request_headers("2023-06-01", "test-beta-flag,extended-cache-ttl");

   assert(messages_buffered("{\"model\":\"claude-opus-4-8\",\"max_tokens\":64,"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   /* inbound model honored (an OpenAI primary would translate + swap the model) */
   assert(strcmp(obj(sent, "model")->valuestring, "claude-opus-4-8") == 0);
   /* pre-injection skipped on the passthrough path */
   assert(strstr(g_last_body, "INJECTED") == NULL);
   /* client beta forwarded upstream */
   assert(g_last_extra != NULL && strstr(g_last_extra, "anthropic-beta: test-beta-flag") != NULL);
   assert(strstr(g_last_extra, "anthropic-version: 2023-06-01") != NULL);
   cJSON_Delete(sent);

   anthropic_ingress_set_request_headers("", "");
   g_stub_preinject_env = NULL;
   reset_capture();
   PASS("messages_buffered_anthropic_parity_passthrough");
}

static void test_messages_buffered_anthropic_strips_stream_false_path(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *sent;

   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered("{\"model\":\"ignored\",\"stream\":true,"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   assert(cJSON_GetObjectItemCaseSensitive(sent, "stream") == NULL);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_anthropic_strips_stream_false_path");
}

static void test_messages_stream_anthropic_preserves_request_shape(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   emit_capture_t cap;
   cJSON *sent;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_stream_payload = "event: message_stop\n"
                      "data: {\"type\":\"message_stop\"}\n\n";
   assert(messages_stream("{\"model\":\"ignored\",\"max_tokens\":64,"
                          "\"system\":[{\"type\":\"text\",\"text\":\"SYS\","
                          "\"cache_control\":{\"type\":\"ephemeral\"}}],"
                          "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                          "\"tool_choice\":{\"type\":\"auto\"},"
                          "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024}}",
                          cap_emit, &cap) == 0);
   assert(g_last_body != NULL);
   sent = parse(g_last_body);
   /* Anthropic primary speaks the Anthropic API -> inbound model honored verbatim. */
   assert(strcmp(obj(sent, "model")->valuestring, "ignored") == 0);
   assert(cJSON_IsArray(obj(sent, "system")));
   assert(cJSON_IsObject(obj(sent, "tool_choice")));
   assert(cJSON_IsObject(obj(sent, "thinking")));
   assert(cJSON_IsTrue(obj(sent, "stream")));
   assert(cap.count == 1);
   assert(strcmp(cap.events[0], "message_stop") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_stream_anthropic_preserves_request_shape");
}

static void test_messages_buffered_openai_family_translates(void)
{
   const delegate_driver_t openai = {.name = "openai", .build_request = openai_driver_build};
   cJSON *sent;
   const cJSON *messages;
   const cJSON *tools;
   char resp[4096];

   reset_capture();
   g_driver = &openai;
   assert(
       messages_buffered("{\"model\":\"ignored\",\"system\":\"SYS\","
                         "\"messages\":[{\"role\":\"assistant\",\"content\":["
                         "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\","
                         "\"input\":{\"path\":\"a.c\"}}]}],"
                         "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]}",
                         resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   assert(strcmp(obj(sent, "driver")->valuestring, "openai") == 0);
   messages = obj(sent, "messages");
   assert(cJSON_IsArray(messages));
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)messages, 0), "role")->valuestring, "system") ==
          0);
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)messages, 1), "role")->valuestring, "assistant") ==
          0);
   tools = obj(sent, "tools");
   assert(cJSON_IsArray(tools));
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)tools, 0), "type")->valuestring, "function") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_openai_family_translates");
}

static void test_messages_buffered_system_prompt_driver_no_duplicate_system(void)
{
   const delegate_driver_t chatgpt = {.name = "chatgpt",
                                      .build_request = system_prompt_driver_build,
                                      .parse_response = parsed_text,
                                      .get_caps = system_prompt_driver_caps};
   cJSON *sent;
   const cJSON *input;
   char resp[4096];

   reset_capture();
   g_driver = &chatgpt;
   assert(messages_buffered("{\"model\":\"ignored\",\"system\":\"SYS\","
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   assert(strcmp(obj(sent, "instructions")->valuestring, "SYS") == 0);
   input = obj(sent, "input");
   assert(cJSON_IsArray(input));
   assert(cJSON_GetArraySize((cJSON *)input) == 1);
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)input, 0), "role")->valuestring, "user") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_system_prompt_driver_no_duplicate_system");
}

static void test_messages_buffered_system_prompt_capability_no_duplicate_system(void)
{
   const delegate_driver_t capable = {.name = "future-responses",
                                      .build_request = system_prompt_driver_build,
                                      .parse_response = parsed_text,
                                      .get_caps = system_prompt_driver_caps};
   cJSON *sent;
   const cJSON *input;
   char resp[4096];

   reset_capture();
   g_driver = &capable;
   assert(messages_buffered("{\"model\":\"ignored\",\"system\":\"SYS\","
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   assert(strcmp(obj(sent, "instructions")->valuestring, "SYS") == 0);
   input = obj(sent, "input");
   assert(cJSON_IsArray(input));
   assert(cJSON_GetArraySize((cJSON *)input) == 1);
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)input, 0), "role")->valuestring, "user") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_system_prompt_capability_no_duplicate_system");
}

static void test_messages_stream_openai_family_translates(void)
{
   const delegate_driver_t openai = {.name = "openai", .build_request = openai_driver_build};
   emit_capture_t cap;
   cJSON *sent;
   const cJSON *messages;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &openai;
   g_stream_payload = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                      "data: [DONE]\n\n";
   assert(messages_stream("{\"model\":\"ignored\",\"system\":\"SYS\","
                          "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                          "\"stream\":true}",
                          cap_emit, &cap) == 0);
   sent = parse(g_last_body);
   messages = obj(sent, "messages");
   assert(cJSON_IsArray(messages));
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)messages, 0), "role")->valuestring, "system") ==
          0);
   assert(cJSON_IsTrue(obj(sent, "stream")));
   assert(cap.count >= 2);
   assert(strcmp(cap.events[0], "message_start") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_stream_openai_family_translates");
}

static void test_messages_stream_chatgpt_buffered_replays_responses(void)
{
   const delegate_driver_t chatgpt = {.name = "chatgpt",
                                      .build_request = system_prompt_driver_build,
                                      .parse_response = parsed_responses,
                                      .get_caps = system_prompt_driver_caps};
   emit_capture_t cap;
   cJSON *sent;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &chatgpt;
   g_response_body =
       "event: response.output_text.delta\n"
       "data: {\"delta\":\"hello from codex\"}\n\n"
       "event: response.completed\n"
       "data: {\"response\":{\"usage\":{\"input_tokens\":12,\"output_tokens\":34}}}\n\n";
   g_response_status = 200;

   assert(messages_stream("{\"model\":\"ignored\",\"system\":\"SYS\","
                          "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                          "\"stream\":true}",
                          cap_emit, &cap) == 0);
   sent = parse(g_last_body);
   assert(strcmp(obj(sent, "driver")->valuestring, "chatgpt") == 0);
   assert(strcmp(obj(sent, "instructions")->valuestring, "SYS") == 0);
   assert(cJSON_IsTrue(obj(sent, "stream")));
   assert(cap.count >= 4);
   assert(strcmp(cap.events[0], "message_start") == 0);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strcmp(cap.events[2], "content_block_delta") == 0);
   assert(strcmp(cap.events[3], "content_block_stop") == 0);
   assert(strstr(cap.data[2], "hello from codex") != NULL);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_stream_chatgpt_buffered_replays_responses");
}

/* The pre-injection envelope is folded into the request `system` as a trailing
 * text block (array form), so a cached system prefix stays stable and both the
 * passthrough and translated paths inherit it. */
static void test_messages_preinject_appends_system_block(void)
{
   g_stub_preinject_env = "<aimee-context>ENV</aimee-context>";
   cJSON *req = parse("{\"system\":[{\"type\":\"text\",\"text\":\"SYS\"}],"
                      "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
   messages_apply_preinject(req);
   char *flat = anthropic_system_to_text(req);
   assert(flat);
   const char *sys = strstr(flat, "SYS");
   const char *env = strstr(flat, "<aimee-context>ENV</aimee-context>");
   assert(sys && env);
   assert(sys < env); /* envelope appended AFTER the (cacheable) prefix */
   free(flat);
   cJSON_Delete(req);
   g_stub_preinject_env = NULL;
   PASS("messages_preinject_appends_system_block");
}

int main(void)
{
   test_translate_request_anthropic_passthrough();
   test_messages_preinject_appends_system_block();
   test_anthropic_relay_round_trip();
   test_anthropic_relay_usage_capture();
   test_relay_append_data_growth();
   test_relay_transport_error();
   test_messages_buffered_anthropic_preserves_request_shape();
   test_messages_buffered_anthropic_parity_passthrough();
   test_messages_buffered_anthropic_strips_stream_false_path();
   test_messages_stream_anthropic_preserves_request_shape();
   test_messages_buffered_openai_family_translates();
   test_messages_buffered_system_prompt_driver_no_duplicate_system();
   test_messages_buffered_system_prompt_capability_no_duplicate_system();
   test_messages_stream_openai_family_translates();
   test_messages_stream_chatgpt_buffered_replays_responses();
   printf("anthropic_http: OK\n");
   return 0;
}
