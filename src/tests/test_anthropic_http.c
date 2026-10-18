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
#include <aimee/delegates/delegate_driver.h>
#include "../headers/log.h"
#include "../headers/server_http.h"
#include "../headers/request_context.h"
#include "gateway_mutate_wire.h"
#include "../vendor/headers/cJSON.h"

#include "support/native_response_fixture.h"

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
static int g_proof_gated = 0;
static int g_mutation_mode, g_post_count, g_reject_first, g_policy_calls, g_router_calls;
static char *g_first_body;
static int g_receipt_refuse, g_receipt_calls;
int ingress_preinject_revalidate_sources(void)
{
   return 0;
}
int ingress_preinject_prepare_attempt(const void *body, size_t length, const char *route,
                                      const char *provider, const char *model, char attempt[33])
{
   if (!g_receipt_refuse)
   {
      attempt[0] = '\0';
      return 0;
   }
   g_receipt_calls++;
   attempt[0] = '\0';
   assert(request_context_refuse_assembly("unavailable") == 0);
   return -1;
}

static char g_requested_agent[128];

static void reset_capture(void)
{
   free(g_last_body);
   g_last_body = NULL;
   free(g_first_body);
   g_first_body = NULL;
   g_mutation_mode = g_post_count = g_reject_first = g_policy_calls = g_router_calls = 0;
   free(g_last_extra);
   g_last_extra = NULL;
   g_stream_status = 200;
   g_stream_payload = NULL;
   g_response_body = NULL;
   g_response_status = 200;
   g_proof_gated = 0;
   g_requested_agent[0] = '\0';
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

/* Registry accessors (see agent_config.h): the production ones read a cached
 * registry in place; here they answer from this file's stubbed loader so the
 * test's fixture still decides the outcome. */
int agent_registry_find(const char *name, agent_t *out)
{
   agent_config_t cfg;
   if (!name || !out || agent_load_config(&cfg) != 0)
      return -1;
   agent_t *found = agent_find(&cfg, name);
   if (!found)
      return -1;
   *out = *found;
   return 0;
}

int agent_registry_default_primary(agent_t *out)
{
   agent_config_t cfg;
   if (!out || agent_load_config(&cfg) != 0)
      return -1;
   agent_t *found = agent_default_primary(&cfg);
   if (!found)
      return -1;
   *out = *found;
   return 0;
}

int agent_registry_resolve_ingress_model(const char *model, agent_t *out)
{
   snprintf(g_requested_agent, sizeof(g_requested_agent), "%s", model ? model : "");
   if (model && strcmp(model, "missing") == 0)
      return -1;
   if (model && model[0] && strcmp(model, "aimee") != 0)
      return agent_registry_find(model, out);
   return agent_registry_default_primary(out);
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
   *response_buf = g_response_body ? strdup(g_response_body)
                                   : native_response_fixture(g_driver ? g_driver->name : NULL, "ok",
                                                             NULL, NULL);
   g_post_count++;
   if (g_reject_first && g_post_count == 1)
   {
      g_first_body = strdup(body);
      assert(g_first_body);
      return 400;
   }
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
   if (g_stream_payload && callback &&
       callback(g_stream_payload, strlen(g_stream_payload), userdata) != 0)
      return -1;
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
void agent_ingress_record_cost(const char *agent_name, const char *agent_model,
                               const char *requested_model, const char *stop_reason,
                               int prompt_tokens, int completion_tokens, int cache_write_tokens,
                               int cache_read_tokens, const char *source, const char *kind)
{
   (void)agent_name;
   (void)agent_model;
   (void)requested_model;
   (void)stop_reason;
   (void)prompt_tokens;
   (void)completion_tokens;
   (void)cache_write_tokens;
   (void)cache_read_tokens;
   (void)source;
   (void)kind;
}
/* Enable accounting in this unit so the tap/record path is exercised. */
int agent_ingress_accounting_enabled(void)
{
   return 1;
}

/* Pre-injection stubs. query_from_messages returns a non-NULL query when a turn
 * is present so pre-injection proceeds; build returns the per-test
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
/* Mirror the configured economizer mode for these assertions. */
int econ_mode_current(void)
{
   return g_proof_gated ? ECON_MODE_SAFE : ECON_MODE_OFF;
}

/* HTTP-layer stub: agent_http_last_retry_after has no upstream socket here, so 0
 * (no Retry-After) suffices. */
int agent_http_last_retry_after(void)
{
   return 0;
}
/* Gateway policy is no-op in these whitebox shape tests (its own behavior is
 * covered by test_gateway_policy); keeps the request shape unaltered. */
int gateway_policy_apply_ir(aimee_request_t *request)
{
   assert(!aimee_ir_request_admit(request));
   g_policy_calls++;
   return 0;
}
int config_gateway_pin_model(void)
{
   return 0;
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

void router_advise_ir(const aimee_request_t *request)
{
   assert(!aimee_ir_request_admit(request));
   g_router_calls++;
}
int gw_mutate_upstream_ok(int native)
{
   (void)native;
   return g_mutation_mode != 0;
}
void gw_mutate_ctx_free(gw_mutate_ctx_t *context)
{
   cJSON_Delete(context->original);
   context->original = NULL;
}
void gw_buffered_mutate(cJSON *container, const char *key, const char *model, const char *system,
                        const char *session, const char *bearer, const char *principal,
                        gw_mutate_ctx_t *context)
{
   (void)model;
   (void)system;
   (void)session;
   (void)bearer;
   (void)principal;
   cJSON *history = cJSON_GetObjectItemCaseSensitive(container, key);
   assert(cJSON_GetArraySize(history) == 3);
   for (const cJSON *message = history->child; message; message = message->next)
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(message, "blocks")) &&
             !cJSON_GetObjectItemCaseSensitive(message, "content"));
   context->original = cJSON_DetachItemFromObjectCaseSensitive(container, key);
   cJSON *reduced = cJSON_Parse(
       g_mutation_mode == 2
           ? "[{\"role\":\"user\",\"content\":\"invalid native bypass\"}]"
           : "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"reduced\"}]}]");
   assert(reduced && cJSON_AddItemToObject(container, key, reduced));
   context->mutated = 1;
}
gw_post_action_t gw_buffered_after_status(cJSON *container, const char *key, int status,
                                          gw_mutate_ctx_t *context)
{
   if (!context->mutated || status != 400)
      return GW_POST_NONE;
   cJSON_DeleteItemFromObjectCaseSensitive(container, key);
   assert(cJSON_AddItemToObject(container, key, context->original));
   context->original = NULL;
   context->mutated = 0;
   return GW_POST_RESEND;
}

#include "../server/anthropic_http.c"

/* Sized for the longest relayed sequence a test drives (a full thinking turn is
 * message_start + two content blocks + message_delta/stop = 10 events). */
#define EMIT_CAP_MAX 96

typedef struct
{
   char events[EMIT_CAP_MAX][64];
   char data[EMIT_CAP_MAX][8192];
   int count;
} emit_capture_t;

static cJSON *parse(const char *json)
{
   cJSON *j = cJSON_Parse(json);
   assert(j != NULL);
   return j;
}

static const cJSON *obj(const cJSON *parent, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)parent, key);
   assert(v != NULL);
   return v;
}

static void system_prompt_driver_caps(const agent_t *agent, driver_caps_t *caps)
{
   (void)agent;
   caps->capability_flags = DRIVER_CAP_STREAMING | DRIVER_CAP_SYSTEM_MSG;
   caps->context_limit = DRIVER_CTX_HUGE;
}

static void cap_emit(void *ctx, const char *event, const char *data_json)
{
   emit_capture_t *cap = (emit_capture_t *)ctx;
   assert(cap->count < EMIT_CAP_MAX);
   snprintf(cap->events[cap->count], sizeof(cap->events[cap->count]), "%s", event);
   snprintf(cap->data[cap->count], sizeof(cap->data[cap->count]), "%s", data_json);
   cap->count++;
}

static void test_explicit_unknown_model_is_rejected(void)
{
   const char *request =
       "{\"model\":\"missing\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\","
       "\"content\":\"hello\"}]}";
   char resp[1024];
   emit_capture_t cap;

   reset_capture();
   assert(messages_buffered(request, resp, sizeof(resp)) == 404);
   assert(strstr(resp, "not_found_error") != NULL);
   assert(strcmp(g_requested_agent, "missing") == 0);

   memset(&cap, 0, sizeof(cap));
   assert(messages_stream(request, cap_emit, &cap) == 0);
   assert(cap.count == 1);
   assert(strcmp(cap.events[0], "error") == 0);
   assert(strstr(cap.data[0], "not_found_error") != NULL);
   assert(strcmp(g_requested_agent, "missing") == 0);
   reset_capture();
   PASS("explicit_unknown_model_is_rejected");
}

static const char *native_empty_stream =
    "event: message_start\ndata: "
    "{\"type\":\"message_start\",\"message\":{\"id\":\"native-id\",\"role\":\"assistant\"}}\n\n"
    "event: message_delta\ndata: "
    "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

static int stream_sink_calls, stream_sink_fail_at;
static int stream_sink(const aimee_session_event_t *event, void *context)
{
   (void)context;
   assert(event->kind == AIMEE_SESSION_DELTA && event->delta);
   assert(event->message_id && event->message_id[0]);
   cJSON *wire = aimee_ir_session_event_json(event);
   assert(wire);
   cJSON_Delete(wire);
   stream_sink_calls++;
   return stream_sink_fail_at && stream_sink_calls >= stream_sink_fail_at ? -1 : 0;
}

static void test_native_ir_fragmentation_and_persistence(void)
{
   const char *frames =
       "event: message_start\r\ndata: "
       "{\"message\":{\"id\":\"opaque-id\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":120,"
       "\"cache_creation_input_tokens\":30,\"cache_read_input_tokens\":10}}}\r\n\r\n"
       "event: content_block_start\ndata: "
       "{\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"initial\"}}\n\n"
       "event: content_block_delta\ndata: "
       "{\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"opaque-signature\"}}"
       "\n\n"
       "event: content_block_stop\ndata: {\"index\":0}\n\n"
       "event: content_block_start\ndata: "
       "{\"index\":1,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"opaque-"
       "redaction\"}}\n\n"
       "event: content_block_stop\ndata: {\"index\":1}\n\n"
       "event: content_block_start\ndata: "
       "{\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"full-tool-id\",\"name\":"
       "\"Read\",\"input\":{}}}\n\n"
       "event: content_block_delta\ndata: "
       "{\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":"
       "\\\"a\\\"}\"}}\n\n"
       "event: content_block_stop\ndata: {\"index\":2}\n\n"
       "event: message_delta\ndata: "
       "{\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":55}}\n\n"
       "event: message_stop\ndata: {}\n\n";
   for (size_t step = 1; step <= 61; step += 15)
   {
      emit_capture_t cap = {0};
      native_ir_stream_ctx_t stream = {.emit = cap_emit, .emit_ctx = &cap, .model = "model"};
      stream_sink_calls = stream_sink_fail_at = 0;
      aimee_ir_session_set_sink(stream_sink, NULL);
      size_t length = strlen(frames);
      for (size_t at = 0; at < length; at += step)
         assert(native_ir_chunk_cb(frames + at, length - at < step ? length - at : step, &stream) ==
                0);
      assert(stream.backend.terminal_emitted && !stream.failed);
      assert(stream.backend.pending_usage_in == 120 && stream.backend.pending_usage_out == 55);
      assert(stream.backend.pending_cache_write == 30 && stream.backend.pending_cache_read == 10);
      assert(stream_sink_calls == 10 && cap.count == 11);
      assert(strstr(cap.data[0], "opaque-id"));
      assert(strstr(cap.data[1], "initial"));
      assert(strstr(cap.data[2], "opaque-signature"));
      assert(strstr(cap.data[4], "opaque-redaction"));
      assert(!strcmp(cap.events[cap.count - 1], "message_stop"));
      aimee_ir_session_set_sink(NULL, NULL);
      free(stream.data);
      free(stream.message_id);
      sse_parser_free(&stream.parser);
   }
   /* A rejected durable append prevents that delta and all later client output. */
   emit_capture_t cap = {0};
   native_ir_stream_ctx_t stream = {.emit = cap_emit, .emit_ctx = &cap};
   stream_sink_calls = 0;
   stream_sink_fail_at = 2;
   aimee_ir_session_set_sink(stream_sink, NULL);
   assert(native_ir_chunk_cb(frames, strlen(frames), &stream) < 0);
   assert(cap.count == 1 && stream.failed && aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   stream_sink_fail_at = 0;
   free(stream.data);
   free(stream.message_id);
   sse_parser_free(&stream.parser);
   PASS("native IR fragmentation preserves signed state and persists before rendering");
}

static void test_native_ir_refuses_incomplete_and_invalid_streams(void)
{
   const char *bad[] = {
       "event: message_stop\ndata: {}\n\n",
       "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n",
       "event: message_start\ndata: {\"message\":{\"role\":\"assistant\"}}\n\n",
       "event: message_start\ndata: "
       "{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":1e300}}}\n\n",
       "event: message_start\ndata: {\"message\":{\"role\":\"assistant\"}}\n\nevent: "
       "content_block_start\ndata: "
       "{\"index\":0,\"content_block\":{\"type\":\"future-content\"}}\n\n",
       "event: message_start\ndata: {\"message\":{\"role\":\"assistant\"}}\n\nevent: "
       "content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\nevent: "
       "message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
       "event: message_start\ndata: {\"message\":{\"role\":\"assistant\"}} trailing\n\n",
   };
   agent_t agent = {0};
   for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
   {
      emit_capture_t cap = {0};
      reset_capture();
      g_stream_payload = bad[i];
      assert(messages_stream_native_ir("fixture", "", "{}", 2, "", &agent, "model", cap_emit, &cap,
                                       NULL) < 0);
      assert(!strcmp(cap.events[cap.count - 1], "error"));
      for (int j = 0; j < cap.count; j++)
         assert(strcmp(cap.events[j], "message_stop"));
   }
   reset_capture();
   PASS("native IR refuses malformed, unsupported and unfinished streams");
}

static void test_native_response_retains_all_typed_blocks_and_long_tools(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
   reset_capture();
   g_driver = &driver;
   cJSON *native = cJSON_Parse("{\"role\":\"assistant\",\"stop_reason\":\"tool_use\","
                               "\"content\":[{\"type\":\"thinking\",\"thinking\":\"private "
                               "reasoning\",\"signature\":\"signed-state\"},"
                               "{\"type\":\"redacted_thinking\",\"data\":\"opaque-state\"},"
                               "{\"type\":\"text\",\"text\":\"visible answer\"},"
                               "{\"type\":\"image\",\"source\":{\"type\":\"url\",\"url\":\"https://"
                               "example.invalid/image.png\"}}],"
                               "\"usage\":{\"input_tokens\":120,\"output_tokens\":7,\"cache_read_"
                               "input_tokens\":20,\"cache_creation_input_tokens\":10}}");
   assert(native);
   cJSON *blocks = cJSON_GetObjectItemCaseSensitive(native, "content");
   char ids[17][512], argument[1024];
   memset(argument, 'x', sizeof argument - 1);
   argument[sizeof argument - 1] = 0;
   for (int i = 0; i < 17; i++)
   {
      memset(ids[i], 'a' + i, sizeof ids[i] - 1);
      ids[i][sizeof ids[i] - 1] = 0;
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "tool_use");
      cJSON_AddStringToObject(tool, "id", ids[i]);
      cJSON_AddStringToObject(tool, "name", "Read");
      cJSON *input = cJSON_AddObjectToObject(tool, "input");
      cJSON_AddStringToObject(input, "path", argument);
      cJSON_AddItemToArray(blocks, tool);
   }
   char *frame = cJSON_PrintUnformatted(native);
   assert(frame);
   g_response_body = frame;
   char reply[65536];
   assert(messages_buffered("{\"model\":\"ignored\",\"max_tokens\":16,\"messages\":["
                            "{\"role\":\"user\",\"content\":\"hi\"}]}",
                            reply, sizeof reply) == 200);
   cJSON *rendered = parse(reply);
   const cJSON *result = obj(rendered, "content");
   assert(cJSON_Compare(blocks, result, 1));
   assert(cJSON_Compare(obj(native, "usage"), obj(rendered, "usage"), 1));
   cJSON_Delete(rendered);
   emit_capture_t cap = {0};
   agent_t agent;
   assert(!agent_registry_default_primary(&agent));
   messages_stream_buffered_replay("https://example.invalid/v1/messages", "", "{}", 2, "", &agent,
                                   &driver, "client-model", "client-id", 0, NULL, cap_emit, &cap);
   int tools = 0, signatures = 0, redacted = 0, images = 0;
   for (int i = 0; i < cap.count; i++)
   {
      cJSON *event = parse(cap.data[i]);
      if (!strcmp(cap.events[i], "content_block_start"))
      {
         const cJSON *block = obj(event, "content_block");
         const char *kind = cJSON_GetStringValue(obj(block, "type"));
         if (!strcmp(kind, "tool_use"))
         {
            assert(tools < 17 && !strcmp(cJSON_GetStringValue(obj(block, "id")), ids[tools]));
            tools++;
         }
         if (!strcmp(kind, "redacted_thinking"))
            redacted += !strcmp(cJSON_GetStringValue(obj(block, "data")), "opaque-state");
         if (!strcmp(kind, "image"))
            images++;
      }
      if (strstr(cap.data[i], "signed-state"))
         signatures++;
      cJSON_Delete(event);
   }
   assert(tools == 17 && signatures == 1 && redacted == 1 && images == 1);
   assert(!strcmp(cap.events[cap.count - 1], "message_stop"));
   free(frame);
   cJSON_Delete(native);
   reset_capture();
   PASS("typed native response retains 17 long tools, thinking, redaction, media and cache usage");
}

static void test_invalid_native_response_never_uses_legacy_parser(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
   const char *frames[] = {
       "{}",
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"forged\"}]}",
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"forged\","
       "\"content\":\"forged\"}]}",
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"same\",\"name\":"
       "\"Read\",\"input\":{}},"
       "{\"type\":\"tool_use\",\"id\":\"same\",\"name\":\"Read\",\"input\":{}}]}",
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"unknown_future_type\"}]}",
       "{\"role\":\"assistant\",\"content\":[]} trailing-junk"};
   for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++)
   {
      reset_capture();
      g_driver = &driver;
      g_response_body = frames[i];
      char reply[1024];
      assert(messages_buffered("{\"model\":\"ignored\",\"max_tokens\":16,\"messages\":["
                               "{\"role\":\"user\",\"content\":\"hi\"}]}",
                               reply, sizeof reply) == 502);
      assert(strstr(reply, "invalid canonical provider response"));
   }
   reset_capture();
   PASS("invalid response framing and forged model blocks fail without a legacy parser");
}

static void test_messages_buffered_anthropic_preserves_request_shape(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
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
   assert(strcmp(g_requested_agent, "ignored") == 0);
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
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
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

static void test_native_admission_failure_sends_no_provider_request(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   const char *request =
       "{\"model\":\"ignored\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":"
       "\"hi\"}],\"tool_choice\":{\"type\":\"unsupported\"}}";
   char response[4096];
   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered(request, response, sizeof response) == 400 && !g_last_body);
   assert(!g_policy_calls && !g_router_calls);
   emit_capture_t cap = {0};
   assert(messages_stream(request, cap_emit, &cap) == 0 && !g_last_body);
   assert(cap.count == 1 && !strcmp(cap.events[0], "error"));
   assert(!g_policy_calls && !g_router_calls);
   reset_capture();
   PASS("native_admission_failure_sends_no_provider_request");
}

static void test_native_host_guidance_retains_client_cache(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   const char *request =
       "{\"model\":\"ignored\",\"max_tokens\":16,\"system\":[{\"type\":\"text\",\"text\":"
       "\"stable\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}],\"messages\":[{"
       "\"role\":\"user\",\"content\":\"hi\"}],\"tools\":[{\"name\":\"Bash\",\"input_schema\":{"
       "\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}}]}";
   char response[4096];
   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered(request, response, sizeof response) == 200);
   cJSON *sent = parse(g_last_body);
   cJSON *system = cJSON_GetObjectItemCaseSensitive(sent, "system");
   assert(cJSON_IsArray(system) && cJSON_GetArraySize(system) == 2);
   cJSON *stable = cJSON_GetArrayItem(system, 0);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(stable, "text")), "stable"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(stable, "cache_control"), "ttl")),
                  "1h"));
   assert(strstr(cJSON_GetStringValue(
                     cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(system, 1), "text")),
                 "aimee-tool-policy"));
   cJSON_Delete(sent);
   reset_capture();
   PASS("native_host_guidance_retains_client_cache");
}

static void test_messages_native_frame_retains_exact_bytes(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   const char *frame =
       " { \"model\": \"ignored\", \"max_tokens\": 16, \"stream\": false, \"future\": "
       "9007199254740993, \"system\": "
       "[{\"type\":\"text\",\"text\":\"stable\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":"
       "\"1h\"}}], \"messages\": [{\"role\":\"user\",\"content\":\"hi\"}] } \n";
   char response[4096];
   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered(frame, response, sizeof response) == 200);
   assert(g_last_body && !strcmp(g_last_body, frame));
   reset_capture();
   PASS("messages_native_frame_retains_exact_bytes");
}

static void test_messages_buffered_anthropic_strips_stream_false_path(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   char resp[4096];
   cJSON *sent;

   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered("{\"max_tokens\":16,\"model\":\"ignored\",\"stream\":true,"
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
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   emit_capture_t cap;
   cJSON *sent;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_stream_payload = native_empty_stream;
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
   assert(cap.count == 3);
   assert(strcmp(cap.events[2], "message_stop") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_stream_anthropic_preserves_request_shape");
}

/* End-to-end through the REAL native-relay entry point: the counters are the whole
 * deliverable of the reasoning tap, so "the tap fills a buffer" is not enough -- the
 * metric must actually move on a live stream, and stay still when there is nothing
 * to observe. (This target links the real aimee_ir_metrics.o; the weak stub in
 * ir_ingress_stubs.c would otherwise make every counter inert.) */
static void test_messages_stream_anthropic_reasoning_metrics(void)
{
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   emit_capture_t cap;
   const char *REQ = "{\"model\":\"m\",\"max_tokens\":64,"
                     "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

   /* (1) a turn with no reasoning must leave BOTH counters alone -- a tap that
    * fires on ordinary turns would make the base rate meaningless. */
   aimee_ir_metrics_reset();
   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_stream_payload = "event: message_start\n"
                      "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\"}}\n\n"
                      "event: content_block_start\n"
                      "data: {\"type\":\"content_block_start\",\"index\":0,"
                      "\"content_block\":{\"type\":\"text\"}}\n\n"
                      "event: content_block_delta\n"
                      "data: {\"type\":\"content_block_delta\",\"index\":0,"
                      "\"delta\":{\"type\":\"text_delta\",\"text\":\"plain answer\"}}\n\n"
                      "event: content_block_stop\n"
                      "data: {\"index\":0}\n\n"
                      "event: message_delta\n"
                      "data: {\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
                      "event: message_stop\n"
                      "data: {\"type\":\"message_stop\"}\n\n";
   assert(messages_stream(REQ, cap_emit, &cap) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 0);

   /* (2) a turn that carries reasoning counts as observed, and NOT as incomplete */
   aimee_ir_metrics_reset();
   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_stream_payload = "event: message_start\n"
                      "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\"}}\n\n"
                      "event: content_block_start\n"
                      "data: {\"type\":\"content_block_start\",\"index\":0,"
                      "\"content_block\":{\"type\":\"thinking\"}}\n\n"
                      "event: content_block_delta\n"
                      "data: {\"type\":\"content_block_delta\",\"index\":0,"
                      "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"weighing it up\"}}\n\n"
                      "event: content_block_stop\n"
                      "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                      "event: message_delta\n"
                      "data: {\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
                      "event: message_stop\n"
                      "data: {\"type\":\"message_stop\"}\n\n";
   assert(messages_stream(REQ, cap_emit, &cap) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 1);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 0);

   /* (3) reasoning the parser had to abandon counts ONLY as incomplete: it was
    * discarded, so counting it as observed would inflate the base rate with
    * thoughts nothing can actually act on. */
   aimee_ir_metrics_reset();
   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_stream_payload = "event: message_start\n"
                      "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\"}}\n\n"
                      "event: content_block_start\n"
                      "data: {\"type\":\"content_block_start\",\"index\":0,"
                      "\"content_block\":{\"type\":\"thinking\"}}\n\n"
                      "event: content_block_delta\n"
                      "data: {\"type\":\"content_block_delta\",\"index\":0,"
                      "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"half a thought\"}}\n\n"
                      "event: content_block_start\n"
                      "data: {\"type\":\"content_block_start\",\"index\":424242,"
                      "\"content_block\":{\"type\":\"thinking\"}}\n\n"
                      "event: message_stop\n"
                      "data: {\"type\":\"message_stop\"}\n\n";
   assert(messages_stream(REQ, cap_emit, &cap) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 1);

   aimee_ir_metrics_reset();
   reset_capture();
   PASS("messages_stream_anthropic_reasoning_metrics");
}

static void test_foreign_ingress_cannot_fall_back_from_ir(void)
{
   const delegate_driver_t drivers[] = {{
                                            .name = "openai",
                                        },
                                        {
                                            .name = "chatgpt",
                                        }};
   const char *old = getenv("AIMEE_IR_PATH");
   char *previous = old ? strdup(old) : NULL;
   setenv("AIMEE_IR_PATH", "0", 1);
   for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++)
   {
      reset_capture();
      g_driver = &drivers[i];
      const char *invalid = "{\"model\":\"ignored\",\"max_tokens\":16,\"messages\":["
                            "{\"role\":\"user\",\"content\":\"hi\"}],"
                            "\"tool_choice\":{\"type\":\"unsupported\"}}";
      char reply[1024];
      assert(messages_buffered(invalid, reply, sizeof reply) == 400);
      assert(!g_last_body && strstr(reply, "canonical"));
      emit_capture_t cap = {0};
      assert(!messages_stream(invalid, cap_emit, &cap));
      assert(!g_last_body && cap.count == 1 && !strcmp(cap.events[0], "error"));
   }
   if (previous)
      setenv("AIMEE_IR_PATH", previous, 1);
   else
      unsetenv("AIMEE_IR_PATH");
   free(previous);
   reset_capture();
   PASS("foreign ingress refuses invalid IR even with the retired flag disabled");
}

static void test_messages_buffered_openai_family_translates(void)
{
   const delegate_driver_t openai = {
       .name = "openai",
   };
   cJSON *sent;
   const cJSON *messages;
   const cJSON *tools;
   char resp[4096];

   reset_capture();
   g_driver = &openai;
   assert(
       messages_buffered("{\"max_tokens\":16,\"model\":\"ignored\",\"system\":\"SYS\","
                         "\"messages\":[{\"role\":\"assistant\",\"content\":["
                         "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\","
                         "\"input\":{\"path\":\"a.c\"}}]}],"
                         "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]}",
                         resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   assert(cJSON_IsString(obj(sent, "model")));
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

                                      .get_caps = system_prompt_driver_caps};
   cJSON *sent;
   const cJSON *input;
   char resp[4096];

   reset_capture();
   g_driver = &chatgpt;
   assert(messages_buffered("{\"max_tokens\":16,\"model\":\"ignored\",\"system\":\"SYS\","
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

static void test_messages_buffered_capability_does_not_override_wire(void)
{
   const delegate_driver_t capable = {.name = "future-responses",

                                      .get_caps = system_prompt_driver_caps};
   cJSON *sent;
   const cJSON *input;
   char resp[4096];

   reset_capture();
   g_driver = &capable;
   assert(messages_buffered("{\"max_tokens\":16,\"model\":\"ignored\",\"system\":\"SYS\","
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);
   sent = parse(g_last_body);
   input = obj(sent, "messages");
   assert(cJSON_IsArray(input));
   assert(cJSON_GetArraySize((cJSON *)input) == 2);
   assert(strcmp(obj(cJSON_GetArrayItem((cJSON *)input, 0), "role")->valuestring, "system") == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_capability_does_not_override_wire");
}

static void test_messages_stream_openai_family_translates(void)
{
   const delegate_driver_t openai = {
       .name = "openai",
   };
   emit_capture_t cap;
   cJSON *sent;
   const cJSON *messages;

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &openai;
   g_stream_payload = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                      "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                      "data: "
                      "{\"choices\":[],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":2,"
                      "\"prompt_tokens_details\":{\"cached_tokens\":12}}}\n\n"
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
   assert(!strcmp(cap.events[cap.count - 1], "message_stop"));
   assert(strstr(cap.data[cap.count - 2], "\"input_tokens\":20") &&
          strstr(cap.data[cap.count - 2], "\"cache_read_input_tokens\":12"));
   cJSON_Delete(sent);
   memset(&cap, 0, sizeof cap);
   g_stream_payload = "data: {\"choices\":[{\"delta\":{\"content\":\"incomplete\"}}]}\n\n";
   assert(messages_stream("{\"model\":\"ignored\",\"messages\":[{\"role\":\"user\",\"content\":"
                          "\"hi\"}],\"stream\":true}",
                          cap_emit, &cap) == 0);
   assert(!strcmp(cap.events[cap.count - 1], "error"));
   for (int i = 0; i < cap.count; i++)
      assert(strcmp(cap.events[i], "message_stop"));
   reset_capture();
   PASS("messages_stream_openai_family_translates");
}

static void test_messages_stream_chatgpt_buffered_replays_responses(void)
{
   const delegate_driver_t chatgpt = {.name = "chatgpt",

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
   assert(cJSON_IsArray(obj(sent, "input")));
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

static void test_proof_gated_ingress_wire_parity(void)
{
   const char *request = "{\"model\":\"ignored\",\"max_tokens\":64,"
                         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   const delegate_driver_t anthropic = {
       .name = "anthropic",
   };
   const delegate_driver_t openai = {
       .name = "openai",
   };
   char resp[4096];
   char *off_body;
   emit_capture_t cap;

   reset_capture();
   g_driver = &anthropic;
   assert(messages_buffered(request, resp, sizeof(resp)) == 200);
   off_body = strdup(g_last_body);
   assert(off_body != NULL);
   g_proof_gated = 1;
   assert(messages_buffered(request, resp, sizeof(resp)) == 200);
   assert(strcmp(g_last_body, off_body) == 0);
   free(off_body);

   reset_capture();
   memset(&cap, 0, sizeof(cap));
   g_driver = &anthropic;
   g_stream_payload = native_empty_stream;
   assert(messages_stream(request, cap_emit, &cap) == 0);
   off_body = strdup(g_last_body);
   assert(off_body != NULL);
   g_proof_gated = 1;
   memset(&cap, 0, sizeof(cap));
   assert(messages_stream(request, cap_emit, &cap) == 0);
   assert(strcmp(g_last_body, off_body) == 0);
   free(off_body);

   reset_capture();
   g_driver = &openai;
   assert(messages_buffered(request, resp, sizeof(resp)) == 200);
   off_body = strdup(g_last_body);
   assert(off_body != NULL);
   g_proof_gated = 1;
   assert(messages_buffered(request, resp, sizeof(resp)) == 200);
   assert(strcmp(g_last_body, off_body) == 0);
   free(off_body);
   reset_capture();
   PASS("proof_gated_ingress_wire_parity");
}

/* Exercise the actual buffered/streaming handlers and their final fence.
 * A missing status provider in this minimal link retains HTTP 502; the owner
 * kind must still be present and neither transport may be called. */
static void test_memory_refusal_blocks_provider_dispatch(void)
{
   const char *request = "{\"model\":\"ignored\",\"max_tokens\":64,"
                         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   const delegate_driver_t drivers[] = {
       {
           .name = "anthropic",
       },
       {
           .name = "openai",
       },
       {
           .name = "chatgpt",
       },
   };
   for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
      for (int gated = 0; gated <= 1; gated++)
      {
         reset_capture();
         g_driver = &drivers[i];
         g_proof_gated = gated;
         request_context_t context = {0};
         request_context_set(&context);
         assert(request_context_refuse_assembly("protected_context_overflow") == 0);
         char response[4096];
         assert(messages_buffered(request, response, sizeof(response)) == 502);
         assert(strstr(response, "protected_context_overflow") && !g_last_body);
         emit_capture_t cap = {0};
         (void)messages_stream(request, cap_emit, &cap);
         assert(cap.count == 1 && !strcmp(cap.events[0], "error"));
         assert(strstr(cap.data[0], "protected_context_overflow") && !g_last_body);
         request_context_clear();
      }
   reset_capture();
   PASS("memory_refusal_blocks_provider_dispatch");
}

static void assert_terminal_receipt_error(const emit_capture_t *cap)
{
   int errors = 0;
   for (int i = 0; i < cap->count; i++)
   {
      assert(strcmp(cap->events[i], "message_stop") != 0);
      if (!strcmp(cap->events[i], "error"))
      {
         errors++;
         assert(strstr(cap->data[i], "unavailable"));
      }
   }
   assert(errors == 1 && !g_last_body && g_receipt_calls == 1);
}
static void test_durable_receipt_refusal_blocks_buffered_and_streamed(void)
{
   const char *request = "{\"model\":\"ignored\",\"max_tokens\":64,"
                         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   const delegate_driver_t drivers[] = {
       {
           .name = "anthropic",
       },
       {
           .name = "openai",
       },
       {
           .name = "chatgpt",
       },
   };
   g_receipt_refuse = 1;
   for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
      for (int gated = 0; gated <= 1; gated++)
         for (int versioned = 0; versioned <= 1; versioned++)
         {
            reset_capture();
            g_driver = &drivers[i];
            g_proof_gated = gated;
            request_context_t context = {0};
            context.memory_receipt_required = 1;
            if (versioned)
               strcpy(context.memory_source_release, "0123456789abcdef0123456789abcdef");
            request_context_set(&context);
            g_receipt_calls = 0;
            char response[4096];
            assert(messages_buffered(request, response, sizeof(response)) == 502);
            assert(strstr(response, "unavailable") && !g_last_body && g_receipt_calls == 1);
            request_context_set(&context);
            g_receipt_calls = 0;
            emit_capture_t cap = {0};
            assert(messages_stream(request, cap_emit, &cap) == 0);
            assert_terminal_receipt_error(&cap);
            if (i == 1)
            {
               request_context_set(&context);
               g_receipt_calls = 0;
               memset(&cap, 0, sizeof(cap));
               agent_t agent = {0};
               assert(messages_stream_ir_relay("fixture", "", "{}", 2, "", &agent, "model",
                                               "message", 0, cap_emit,
                                               &cap) == HTTP_RETRY_ADMISSION_REFUSED);
               assert_terminal_receipt_error(&cap);
            }
            request_context_clear();
         }
   g_receipt_refuse = 0;
   reset_capture();
   PASS("durable_receipt_refusal_blocks_buffered_and_streamed");
}

static void test_count_tokens_validates_request_shape(void)
{
   const delegate_driver_t openai = {
       .name = "openai",
   };
   static const char *const invalid[] = {
       "{", "[]", "null", "{}", "{\"messages\":\"not-an-array\"}", NULL,
   };
   char resp[4096];

   reset_capture();
   g_driver = &openai;
   for (int i = 0; invalid[i]; i++)
   {
      assert(count_tokens(invalid[i], resp, sizeof(resp)) == 400);
      assert(strstr(resp, "invalid_request_error") != NULL);
   }
   assert(count_tokens("{\"messages\":[]}", resp, sizeof(resp)) == 200);
   assert(strcmp(resp, "{\"input_tokens\":0}") == 0);
   assert(count_tokens("{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}", resp,
                       sizeof(resp)) == 200);
   assert(strcmp(resp, "{\"input_tokens\":1}") == 0);
   reset_capture();
   PASS("count_tokens_validates_request_shape");
}

static void test_messages_buffered_rejects_missing_messages(void)
{
   const delegate_driver_t openai = {
       .name = "openai",
   };
   char resp[4096];

   reset_capture();
   g_driver = &openai;
   assert(messages_buffered("{\"max_tokens\":16}", resp, sizeof(resp)) == 400);
   assert(strstr(resp, "messages must be a non-empty array") != NULL);
   assert(g_last_body == NULL);
   reset_capture();
   PASS("messages_buffered_rejects_missing_messages");
}

static void test_native_alias_uses_provider_model(void)
{
   const char *body =
       "{\"model\":\"registered-agent\",\"max_tokens\":16,\"system\":[{\"type\":\"text\",\"text\":"
       "\"prefix\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}],"
       "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   agent_t agent = {0};
   snprintf(agent.name, sizeof agent.name, "registered-agent");
   snprintf(agent.model, sizeof agent.model, "actual-provider-model");
   cJSON *request = parse(body);
   aimee_request_t canonical = {0};
   char error[128];
   assert(!messages_prepare_request(body, &agent, 1, &canonical, error, sizeof error));
   char *wire = anthropic_build_prov_body(&canonical, AIMEE_WIRE_ANTHROPIC, 0);
   cJSON *native = wire ? cJSON_Parse(wire) : NULL;
   assert(native && !strcmp(cJSON_GetStringValue(obj(native, "model")), "actual-provider-model"));
   assert(cJSON_Compare(obj(request, "system"), obj(native, "system"), 1));
   assert(cJSON_Compare(obj(request, "messages"), obj(native, "messages"), 1));
   free(wire);
   cJSON_Delete(native);
   cJSON_Delete(request);
   aimee_request_free(&canonical);
}

static void test_canonical_economizer_restore_resends_once(void)
{
   const delegate_driver_t driver = {.name = "anthropic"};
   const char *request = "{\"model\":\"ignored\",\"max_tokens\":32,\"messages\":["
                         "{\"role\":\"user\",\"content\":\"first\"},"
                         "{\"role\":\"assistant\",\"content\":\"answer\"},"
                         "{\"role\":\"user\",\"content\":\"last\"}]}";
   char response[4096];
   reset_capture();
   g_driver = &driver;
   g_mutation_mode = g_reject_first = 1;
   assert(messages_buffered(request, response, sizeof response) == 200);
   assert(g_post_count == 2 && g_policy_calls == 1 && g_router_calls == 1);
   cJSON *reduced = parse(g_first_body), *restored = parse(g_last_body), *original = parse(request);
   assert(cJSON_GetArraySize(obj(reduced, "messages")) == 1 && strstr(g_first_body, "reduced"));
   assert(cJSON_Compare(obj(restored, "messages"), obj(original, "messages"), 1));
   cJSON_Delete(reduced);
   cJSON_Delete(restored);
   cJSON_Delete(original);
   reset_capture();
   g_driver = &driver;
   g_mutation_mode = 2;
   assert(messages_buffered(request, response, sizeof response) == 500 && !g_post_count);
   reset_capture();
   PASS("canonical economizer restores original history once; native bypass refuses before "
        "dispatch");
}

static void test_native_count_admits_before_dispatch(void)
{
   const delegate_driver_t driver = {.name = "anthropic"};
   const char *invalid[] = {
       "{\"model\":\"aimee\",\"messages\":[{\"role\":\"forged\",\"content\":\"x\"}]}",
       "{\"model\":\"aimee\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]} trailing"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      reset_capture();
      g_driver = &driver;
      char response[1024];
      assert(count_tokens(invalid[i], response, sizeof response) == 400 && !g_post_count);
   }
   reset_capture();
   g_driver = &driver;
   char response[1024];
   assert(count_tokens("{\"model\":\"aimee\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}",
                       response, sizeof response) == 200 &&
          g_post_count == 1);
   cJSON *sent = parse(g_last_body);
   assert(!strcmp(cJSON_GetStringValue(obj(sent, "model")), "configured-claude"));
   cJSON_Delete(sent);
   reset_capture();
}

int main(void)
{
   test_canonical_economizer_restore_resends_once();
   test_native_count_admits_before_dispatch();
   test_native_alias_uses_provider_model();
   test_explicit_unknown_model_is_rejected();
   test_messages_stream_anthropic_reasoning_metrics();
   test_native_ir_fragmentation_and_persistence();
   test_native_ir_refuses_incomplete_and_invalid_streams();
   test_native_response_retains_all_typed_blocks_and_long_tools();
   test_invalid_native_response_never_uses_legacy_parser();
   test_messages_buffered_anthropic_preserves_request_shape();
   test_messages_buffered_anthropic_parity_passthrough();
   test_native_admission_failure_sends_no_provider_request();
   test_native_host_guidance_retains_client_cache();
   test_messages_native_frame_retains_exact_bytes();
   test_messages_buffered_anthropic_strips_stream_false_path();
   test_messages_stream_anthropic_preserves_request_shape();
   test_foreign_ingress_cannot_fall_back_from_ir();
   test_messages_buffered_openai_family_translates();
   test_messages_buffered_system_prompt_driver_no_duplicate_system();
   test_messages_buffered_capability_does_not_override_wire();
   test_messages_stream_openai_family_translates();
   test_messages_stream_chatgpt_buffered_replays_responses();
   test_proof_gated_ingress_wire_parity();
   test_memory_refusal_blocks_provider_dispatch();
   test_durable_receipt_refusal_blocks_buffered_and_streamed();
   test_count_tokens_validates_request_shape();
   test_messages_buffered_rejects_missing_messages();
   printf("anthropic_http: OK\n");
   return 0;
}

/* Configuration is readable, modules are unspecified (-1) so the environment
 * default decides, economizer is on, and the P5 injection opt-in is off. */
int config_present(void)
{
   return 1;
}

int config_module_governance(void)
{
   return -1;
}

int config_ingress_preinject_anthropic_enabled(void)
{
   return 0;
}
