/* test_anthropic_http.c: pure tests for file-local Anthropic HTTP ingress
 * helpers. This intentionally includes anthropic_http.c so production helpers
 * can remain private while buffer/parsing behavior stays unit-tested. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../headers/aimee.h"
#include "../headers/agent_config.h"
#include "../headers/agent_exec.h"
#include "../headers/agent_protocol.h"
#include "../headers/cli_session.h"
#include "../headers/delegate_driver.h"
#include "../headers/server_http.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static const delegate_driver_t *g_driver;
static char *g_last_body;
static char *g_last_extra; /* upstream extra-header block from the last post */
static int g_stream_status = 200;
static const char *g_stream_payload;

/* CLI-backed ingress (tmux claude-oauth) test controls: g_cli_mode makes
 * agent_load_config yield a tmux-cli primary; the rest drive/observe the
 * agent_execute_cli_session stub below. */
static int g_cli_mode = 0;
static const char *g_cli_agent_name = "primary";

static void reset_capture(void)
{
   free(g_last_body);
   g_last_body = NULL;
   free(g_last_extra);
   g_last_extra = NULL;
   g_stream_status = 200;
   g_stream_payload = NULL;
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
   if (g_cli_mode)
   {
      snprintf(cfg->agents[0].name, sizeof(cfg->agents[0].name), "%s", g_cli_agent_name);
      snprintf(cfg->agents[0].backend, sizeof(cfg->agents[0].backend), "%s", AGENT_BACKEND_TMUX_CLI);
      cfg->agents[0].session_reuse = 0;
   }
   return 0;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   (void)name;
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
   *response_buf = strdup("{}");
   return 200;
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

void agent_parse_response_openai(cJSON *root, parsed_response_t *out)
{
   (void)root;
   memset(out, 0, sizeof(*out));
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
/* P2c streaming branch: off by default in these whitebox shape tests
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

/* --- CLI-backed ingress stubs (tmux claude-oauth path) ------------------- */
static int g_cli_rc = 0; /* agent_execute_cli_session return code */
static const char *g_cli_reply = "hello from CLI";
static char g_cli_seen_system[4096];
static char g_cli_seen_user[8192];
static char g_cli_seen_sid[128];
static char g_cli_seen_cmd[256];
static int g_override_set;
static int g_override_clear;
/* Live-streaming: when nparts>0 the executor stub fires the registered stream cb
 * with each part (exercising the real-time delta bridge) and returns their concat. */
static const char *g_cli_stream_parts[4];
static int g_cli_stream_nparts;
static cli_session_stream_cb_t g_test_stream_cb;
static void *g_test_stream_ud;

void cli_session_set_stream_cb(cli_session_stream_cb_t cb, void *ud)
{
   g_test_stream_cb = cb;
   g_test_stream_ud = ud;
}
cli_session_stream_cb_t cli_session_get_stream_cb(void **ud_out)
{
   if (ud_out)
      *ud_out = g_test_stream_ud;
   return g_test_stream_cb;
}

int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out)
{
   const char *catp;
   (void)network;
   (void)max_tokens;
   (void)temperature;
   snprintf(g_cli_seen_system, sizeof(g_cli_seen_system), "%s", system_prompt ? system_prompt : "");
   snprintf(g_cli_seen_user, sizeof(g_cli_seen_user), "%s", user_prompt ? user_prompt : "");
   snprintf(g_cli_seen_cmd, sizeof(g_cli_seen_cmd), "%s", agent->cli_cmd);
   /* When the system prompt was delivered via --append-system-prompt "$(cat P)",
    * read P back so the test can assert the content actually reached the CLI. */
   catp = strstr(agent->cli_cmd, "$(cat ");
   if (catp)
   {
      const char *end = strchr(catp + 6, ')');
      if (end && (size_t)(end - (catp + 6)) < 256)
      {
         char path[256];
         FILE *pf;
         snprintf(path, sizeof(path), "%.*s", (int)(end - (catp + 6)), catp + 6);
         pf = fopen(path, "r");
         if (pf)
         {
            size_t got = fread(g_cli_seen_system, 1, sizeof(g_cli_seen_system) - 1, pf);
            g_cli_seen_system[got] = '\0';
            fclose(pf);
         }
      }
   }
   memset(out, 0, sizeof(*out));
   if (g_cli_rc == 0)
   {
      if (g_cli_stream_nparts > 0)
      {
         char buf[1024] = "";
         for (int i = 0; i < g_cli_stream_nparts; i++)
         {
            if (g_test_stream_cb)
               g_test_stream_cb(g_cli_stream_parts[i], g_test_stream_ud);
            strncat(buf, g_cli_stream_parts[i], sizeof(buf) - strlen(buf) - 1);
         }
         out->response = strdup(buf);
      }
      else
      {
         out->response = strdup(g_cli_reply ? g_cli_reply : "");
      }
      out->success = 1;
      out->turns = 1;
   }
   else
   {
      snprintf(out->error, sizeof(out->error), "stub cli error");
   }
   return g_cli_rc;
}

void session_id_set_override(const char *sid)
{
   g_override_set++;
   snprintf(g_cli_seen_sid, sizeof(g_cli_seen_sid), "%s", sid ? sid : "");
}
void session_id_clear_override(void)
{
   g_override_clear++;
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

/* --- CLI-backed ingress (tmux claude-oauth) tests ------------------------ */

static void test_cli_transcript_render(void)
{
   cJSON *msgs = parse("[{\"role\":\"user\",\"content\":\"first\"},"
                       "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"reply\"}]},"
                       "{\"role\":\"user\",\"content\":["
                       "{\"type\":\"tool_result\",\"tool_use_id\":\"t\",\"content\":\"TOOLONLY\"},"
                       "{\"type\":\"text\",\"text\":\"second\"}]}]");
   char *t = anthropic_messages_to_transcript(msgs);
   cJSON *empty = parse("[]");

   assert(t != NULL);
   assert(strstr(t, "User: first") != NULL);
   assert(strstr(t, "Assistant: reply") != NULL);
   assert(strstr(t, "User: second") != NULL);
   assert(strstr(t, "TOOLONLY") == NULL); /* tool_result text skipped */
   free(t);

   assert(anthropic_messages_to_transcript(empty) == NULL); /* nothing renderable */
   cJSON_Delete(empty);
   cJSON_Delete(msgs);
   PASS("cli_transcript_render");
}

static void test_messages_buffered_cli_success(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *out;
   const cJSON *content, *blk;

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary"; /* non-claude name -> login gate bypassed */
   g_cli_rc = 0;
   g_cli_reply = "hello from CLI";
   g_override_set = g_override_clear = 0;
   g_cli_seen_system[0] = g_cli_seen_user[0] = g_cli_seen_sid[0] = '\0';

   assert(messages_buffered("{\"model\":\"claude-x\",\"max_tokens\":64,\"system\":\"SYSTEXT\","
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi there\"}]}",
                            resp, sizeof(resp)) == 200);
   out = parse(resp);
   assert(strcmp(obj(out, "type")->valuestring, "message") == 0);
   assert(strcmp(obj(out, "role")->valuestring, "assistant") == 0);
   assert(strcmp(obj(out, "model")->valuestring, "claude-x") == 0); /* requested model echoed */
   assert(strcmp(obj(out, "stop_reason")->valuestring, "end_turn") == 0);
   content = obj(out, "content");
   assert(cJSON_IsArray(content) && cJSON_GetArraySize((cJSON *)content) == 1);
   blk = cJSON_GetArrayItem((cJSON *)content, 0);
   assert(strcmp(obj(blk, "type")->valuestring, "text") == 0);
   assert(strcmp(obj(blk, "text")->valuestring, "hello from CLI") == 0);
   /* prompt rendering: system passed verbatim, transcript carries labeled user text */
   assert(strcmp(g_cli_seen_system, "SYSTEXT") == 0);
   assert(strstr(g_cli_seen_user, "User: hi there") != NULL);
   /* per-request session override set + cleared exactly once, unique id shape */
   assert(g_override_set == 1 && g_override_clear == 1);
   assert(strncmp(g_cli_seen_sid, "aimee-ingress-msg_", 18) == 0);

   cJSON_Delete(out);
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_buffered_cli_success");
}

static void test_messages_buffered_cli_error(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *out;

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = -2; /* executor timeout */
   g_override_set = g_override_clear = 0;

   assert(messages_buffered(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", resp,
              sizeof(resp)) == 502);
   out = parse(resp);
   assert(strcmp(obj(out, "type")->valuestring, "error") == 0);
   assert(g_override_set == 1 && g_override_clear == 1); /* override cleared even on failure */

   cJSON_Delete(out);
   g_cli_rc = 0;
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_buffered_cli_error");
}

static void test_messages_buffered_cli_empty_reply(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = 0;
   g_cli_reply = ""; /* success rc but empty text -> treated as failure */

   assert(messages_buffered(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", resp,
              sizeof(resp)) == 502);

   g_cli_reply = "hello from CLI";
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_buffered_cli_empty_reply");
}

static void test_messages_buffered_cli_not_logged_in(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *out;
   const char *home = getenv("HOME");
   char saved[512];

   saved[0] = '\0';
   if (home)
      snprintf(saved, sizeof(saved), "%s", home);

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "claude"; /* claude-family -> login pre-flight applies */
   setenv("HOME", "/nonexistent-aimee-cli-home", 1);

   assert(messages_buffered(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", resp,
              sizeof(resp)) == 401);
   out = parse(resp);
   assert(strcmp(obj(out, "type")->valuestring, "error") == 0);
   assert(strcmp(obj(obj(out, "error"), "type")->valuestring, "authentication_error") == 0);

   cJSON_Delete(out);
   if (saved[0])
      setenv("HOME", saved, 1);
   else
      unsetenv("HOME");
   g_cli_mode = 0;
   g_cli_agent_name = "primary";
   reset_capture();
   PASS("messages_buffered_cli_not_logged_in");
}

static void test_messages_stream_cli_success(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   emit_capture_t cap;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = 0;
   g_cli_reply = "streamed reply";

   assert(messages_stream(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
              cap_emit, &cap) == 0);
   /* full Anthropic SSE sequence for a single text block */
   assert(cap.count == 6);
   assert(strcmp(cap.events[0], "message_start") == 0);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strcmp(cap.events[2], "content_block_delta") == 0);
   assert(strstr(cap.data[2], "streamed reply") != NULL);
   assert(strcmp(cap.events[3], "content_block_stop") == 0);
   assert(strcmp(cap.events[4], "message_delta") == 0);
   assert(strstr(cap.data[4], "end_turn") != NULL);
   assert(strcmp(cap.events[5], "message_stop") == 0);

   g_cli_mode = 0;
   reset_capture();
   PASS("messages_stream_cli_success");
}

static void test_messages_stream_cli_error(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   emit_capture_t cap;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = -1; /* executor failure -> single typed error event, stream terminates */

   assert(messages_stream(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
              cap_emit, &cap) == 0);
   assert(cap.count == 1);
   assert(strcmp(cap.events[0], "error") == 0);

   g_cli_rc = 0;
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_stream_cli_error");
}

static void test_messages_buffered_cli_appends_system_prompt(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   const char *home = getenv("HOME");
   char saved[512];
   char tmphome[] = "/tmp/aimee-cli-home-XXXXXX";
   char dir[600];
   char creds[700];
   FILE *cf;

   saved[0] = '\0';
   if (home)
      snprintf(saved, sizeof(saved), "%s", home);
   assert(mkdtemp(tmphome) != NULL);
   snprintf(dir, sizeof(dir), "%s/.claude", tmphome);
   assert(mkdir(dir, 0700) == 0);
   snprintf(creds, sizeof(creds), "%s/.claude/.credentials.json", tmphome);
   cf = fopen(creds, "w");
   assert(cf != NULL);
   fputs("{}", cf);
   fclose(cf);
   setenv("HOME", tmphome, 1); /* claude-login pre-flight now passes */

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "claude"; /* claude-family -> --append-system-prompt path */
   g_cli_rc = 0;
   g_cli_reply = "ok";
   g_cli_seen_system[0] = g_cli_seen_cmd[0] = '\0';

   assert(messages_buffered("{\"model\":\"claude-x\",\"system\":\"MYSYS-12345\","
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   /* system delivered as a flag (not pasted), and its content reached the CLI */
   assert(strstr(g_cli_seen_cmd, "--append-system-prompt") != NULL);
   assert(strstr(g_cli_seen_cmd, "$(cat ") != NULL);
   assert(strcmp(g_cli_seen_system, "MYSYS-12345") == 0);

   if (saved[0])
      setenv("HOME", saved, 1);
   else
      unsetenv("HOME");
   unlink(creds);
   rmdir(dir);
   rmdir(tmphome);
   g_cli_mode = 0;
   g_cli_agent_name = "primary";
   reset_capture();
   PASS("messages_buffered_cli_appends_system_prompt");
}

static void test_messages_stream_cli_live_deltas(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   emit_capture_t cap;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = 0;
   g_cli_stream_parts[0] = "Hello ";
   g_cli_stream_parts[1] = "world";
   g_cli_stream_nparts = 2;

   assert(messages_stream(
              "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
              cap_emit, &cap) == 0);
   /* Each CLI delta is forwarded live as its own text_delta (not one block at the
    * end): start, block_start, delta("Hello "), delta("world"), block_stop, msg_delta, stop. */
   assert(cap.count == 7);
   assert(strcmp(cap.events[0], "message_start") == 0);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strcmp(cap.events[2], "content_block_delta") == 0);
   assert(strstr(cap.data[2], "Hello ") != NULL);
   assert(strcmp(cap.events[3], "content_block_delta") == 0);
   assert(strstr(cap.data[3], "world") != NULL);
   assert(strcmp(cap.events[4], "content_block_stop") == 0);
   assert(strcmp(cap.events[5], "message_delta") == 0);
   assert(strcmp(cap.events[6], "message_stop") == 0);

   g_cli_stream_nparts = 0;
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_stream_cli_live_deltas");
}

static void test_messages_buffered_cli_too_large(void)
{
   const delegate_driver_t anthropic = {.name = "anthropic", .parse_response = parsed_text};
   char resp[4096];
   cJSON *out;
   /* Build a > INGRESS_MAX_PROMPT_BYTES user message. */
   size_t big = (size_t)INGRESS_MAX_PROMPT_BYTES + 16;
   char *huge = malloc(big + 1);
   char *body;

   assert(huge != NULL);
   memset(huge, 'x', big);
   huge[big] = '\0';
   body = malloc(big + 128);
   assert(body != NULL);
   snprintf(body, big + 128,
            "{\"model\":\"claude-x\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", huge);

   reset_capture();
   g_driver = &anthropic;
   g_cli_mode = 1;
   g_cli_agent_name = "primary";
   g_cli_rc = 0;

   assert(messages_buffered(body, resp, sizeof(resp)) == 413);
   out = parse(resp);
   assert(strcmp(obj(obj(out, "error"), "type")->valuestring, "request_too_large") == 0);
   cJSON_Delete(out);

   free(huge);
   free(body);
   g_cli_mode = 0;
   reset_capture();
   PASS("messages_buffered_cli_too_large");
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
   test_cli_transcript_render();
   test_messages_buffered_cli_success();
   test_messages_buffered_cli_appends_system_prompt();
   test_messages_buffered_cli_error();
   test_messages_buffered_cli_empty_reply();
   test_messages_buffered_cli_not_logged_in();
   test_messages_stream_cli_success();
   test_messages_stream_cli_error();
   test_messages_stream_cli_live_deltas();
   test_messages_buffered_cli_too_large();
   printf("anthropic_http: OK\n");
   return 0;
}
