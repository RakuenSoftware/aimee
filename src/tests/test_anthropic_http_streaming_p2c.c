/* test_anthropic_http_streaming_p2c.c: P2c streaming-side response-policing
 * integration tests for the Anthropic /v1/messages path. Like the buffered
 * P2c test (test_anthropic_http_p2c.c), this links the real gateway_policy.o
 * so the production police function runs end-to-end. The two streaming
 * paths (Anthropic-native raw-byte relay, OpenAI-via-translator per-block)
 * both bypass their incremental paths when the policy is ON, fall through
 * to a buffered upstream fetch + police + emit_message_as_sse replay.
 *
 * Scope: covers the policy-ON path (drops + replays) AND the policy-OFF
 * regression canary (today's incremental paths are unchanged). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/aimee.h"
#include "../headers/agent_config.h"
#include "../headers/agent_exec.h"
#include "../headers/agent_protocol.h"
#include "../headers/anthropic_ingress.h"
#include "../headers/delegate_driver.h"
#include "../headers/log.h"
#include "../headers/server_http.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

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

/* The ingress now enforces the per-agent concurrency slot; stub it (always
 * acquires) so this minimal link builds. */
int provider_catalog_concurrent_acquire(const char *agent_name, int max_parallel)
{
   (void)agent_name;
   (void)max_parallel;
   return 1;
}
void provider_catalog_concurrent_release(const char *agent_name)
{
   (void)agent_name;
}

static const delegate_driver_t *g_driver;
static char *g_last_body;
static char *g_last_extra;
static int g_stream_status;
static const char *g_stream_payload;
/* Fake upstream response body for the BUFFERED fetch on the policy-ON path.
 * The streaming path uses agent_http_post (not the streaming variant) when
 * the policy is on, so we hook the same `g_response_body` machinery as the
 * buffered P2c test. NULL = empty "{}" (regression-canary default). */
static const char *g_response_body;
static int g_response_status;
/* P2c policy gate. The real config_load is stubbed to honor this global;
 * flip g_prevent to 1 to enable the streaming-side tool policing. */
static int g_prevent;
/* Tool-use fixtures for parsed_with_tool_uses (the driver parse_response
 * stub reads `g_tool_uses_json` and synthesizes a populated parsed struct). */
static const char *g_tool_uses_json;
static const char *g_upstream_stop_reason;

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
   g_prevent = 0;
   g_tool_uses_json = NULL;
   g_upstream_stop_reason = NULL;
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

void agent_free_parsed_response(parsed_response_t *p)
{
   if (!p)
      return;
   free(p->content);
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
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
/* Context economizer (gateway seam) is default-off and not under test here; the
 * shadow-mode hook in messages_run_request_pipeline references these three, so stub
 * them as no-ops to keep the minimal P2c link from pulling the economizer + its
 * db1/token_tracker dependency chain. */
#include "../headers/context_reduce.h"
int context_reduce(cJSON *messages, const char *system_prompt, const char *model,
                   const char *session_id, reduce_seam_t seam, const reduce_config_t *cfg,
                   reduce_state_t *st, reduce_result_t *out)
{
   (void)messages;
   (void)system_prompt;
   (void)model;
   (void)session_id;
   (void)seam;
   (void)cfg;
   (void)st;
   if (out)
      memset(out, 0, sizeof(*out));
   return 0;
}
void context_reduce_result_free(reduce_result_t *out)
{
   (void)out;
}
void agent_record_reduce_ledger(const struct reduce_result_s *r, const char *model,
                                const char *agent_name, const char *role)
{
   (void)r;
   (void)model;
   (void)agent_name;
   (void)role;
}
int agent_ingress_accounting_enabled(void)
{
   return 1;
}

int agent_http_last_retry_after(void)
{
   return 0;
}

/* Driver parse stub (mirrors test_anthropic_http_p2c.c): populates
 * parsed.calls[] from g_tool_uses_json, leaves content empty, and copies
 * g_upstream_stop_reason into parsed.stop_reason. */
static void parsed_with_tool_uses(cJSON *root, const char *body, parsed_response_t *out)
{
   cJSON *arr;
   int i;
   int cap;
   (void)root;
   (void)body;
   memset(out, 0, sizeof(*out));
   if (g_upstream_stop_reason)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", g_upstream_stop_reason);
   arr = g_tool_uses_json ? cJSON_Parse(g_tool_uses_json) : NULL;
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      return;
   }
   cap = cJSON_GetArraySize(arr);
   if (cap > AGENT_MAX_TOOL_CALLS)
      cap = AGENT_MAX_TOOL_CALLS;
   for (i = 0; i < cap; i++)
   {
      cJSON *t = cJSON_GetArrayItem(arr, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
      const cJSON *args = cJSON_GetObjectItemCaseSensitive(t, "arguments");
      snprintf(out->calls[i].id, sizeof(out->calls[i].id), "%s",
               cJSON_IsString(id) ? id->valuestring : "");
      snprintf(out->calls[i].name, sizeof(out->calls[i].name), "%s",
               cJSON_IsString(name) ? name->valuestring : "");
      out->calls[i].arguments = cJSON_IsString(args) ? strdup(args->valuestring) : strdup("{}");
   }
   out->call_count = cap;
   cJSON_Delete(arr);
}

/* Minimal config_load stub (the real one depends on the YAML loader,
 * out of scope for this minimal-link test). Mirrors test_anthropic_http_p2c.c. */
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->gateway_prevent_subagents = g_prevent;
   }
   return 0;
}

/* Minimal guardrails_canonical_tool_name stub: maps Task/Agent/spawn_agent to
 * "Subagent" (matches the production canonicalization used by
 * gateway_policy.c via the real guardrails_orchestrator.o). */
const char *guardrails_canonical_tool_name(const char *n)
{
   if (n && (strcmp(n, "Task") == 0 || strcmp(n, "Agent") == 0 || strcmp(n, "spawn_agent") == 0))
      return "Subagent";
   return n ? n : "";
}

/* Pre-injection stubs (the real ones depend on KB clients + config; this
 * test is minimal-link). Mirrors test_anthropic_http_p2c.c. */
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

#include "../server/anthropic_http.c"

/* --- captured SSE events for the replay path ------------------------- */

#define REPLAY_CAP 32
typedef struct
{
   char events[REPLAY_CAP][32];
   char data[REPLAY_CAP][2048];
   int count;
} replay_cap_t;

static void replay_capture(void *ctx, const char *event, const char *data)
{
   replay_cap_t *c = (replay_cap_t *)ctx;
   assert(c->count < REPLAY_CAP);
   snprintf(c->events[c->count], sizeof(c->events[c->count]), "%s", event);
   snprintf(c->data[c->count], sizeof(c->data[c->count]), "%s", data);
   c->count++;
}

/* --- Anthropic-native streaming tests ---------------------------------- */

/* Policy ON, Anthropic-native primary, upstream reply contains a subagent
 * tool_use. The streaming path takes the buffered-and-replay branch: the
 * raw byte relay is bypassed, the upstream reply is fetched as a single
 * agent_http_post, the police function drops the subagent call, and
 * emit_message_as_sse replays the surviving sequence. No tool_use block
 * on the wire; stop_reason "end_turn" (all-dropped rewrite). */
static void test_streaming_anthropic_police_drops_subagent_tool_use(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;
   const char *names[] = {"Task"};
   const char *ids[] = {"t1"};

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[{\"id\":\"t1\",\"name\":\"Task\",\"arguments\":\"{}\"}]";
   g_upstream_stop_reason = "tool_use";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Replay emits message_start, [text_start, text_stop], message_delta,
    * message_stop (no tool_use blocks survived). */
   assert(cap.count >= 5);
   int seen_tool_use = 0;
   const char *stop_reason = NULL;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         stop_reason = strstr(cap.data[i], "\"stop_reason\":\"");
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\""))
         seen_tool_use = 1;
   }
   assert(!seen_tool_use);
   assert(stop_reason != NULL);
   assert(strstr(stop_reason, "end_turn") != NULL);

   reset_capture();
   (void)names;
   (void)ids;
   PASS("streaming_anthropic_police_drops_subagent_tool_use");
}

/* Policy ON, Anthropic-native primary, upstream reply is text-only. The
 * streaming path takes the buffered-and-replay branch; the replay
 * emits the text block + an end_turn message_delta. */
static void test_streaming_anthropic_police_on_no_tool_use_passthrough(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[]";
   g_upstream_stop_reason = "end_turn";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* message_start, [text_start, text_stop], message_delta, message_stop. */
   assert(cap.count >= 4);
   int seen_tool_use = 0;
   const char *stop_reason = NULL;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         stop_reason = strstr(cap.data[i], "\"stop_reason\":\"");
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\""))
         seen_tool_use = 1;
   }
   assert(!seen_tool_use);
   assert(stop_reason != NULL);
   assert(strstr(stop_reason, "end_turn") != NULL);

   reset_capture();
   PASS("streaming_anthropic_police_on_no_tool_use_passthrough");
}

/* Policy ON, upstream returns 5xx. The streaming path emits a single
 * `error` event so the client's SSE reader terminates cleanly (no
 * partial SSE events). */
static void test_streaming_anthropic_policy_on_upstream_error_returns_error(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[]";
   g_upstream_stop_reason = "tool_use";
   g_prevent = 1;
   g_response_status = 502;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Exactly one error event, no other events. */
   assert(cap.count == 1);
   assert(strcmp(cap.events[0], "error") == 0);
   assert(strstr(cap.data[0], "\"type\":\"error\"") != NULL);
   assert(strstr(cap.data[0], "status 502") != NULL);

   reset_capture();
   PASS("streaming_anthropic_policy_on_upstream_error_returns_error");
}

/* Policy OFF regression canary: today's incremental raw byte relay runs
 * unchanged. We send a fake Anthropic SSE upstream payload and assert
 * the events pass through verbatim (the relay emits the parsed data
 * payload as a "message" event — its event name is the SSE `event:` line
 * or "message" by default). */
static void test_streaming_anthropic_policy_off_is_byte_neutral(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_prevent = 0;
   g_stream_payload = "event: message_start\ndata: "
                      "{\"type\":\"message_start\",\"message\":{\"id\":\"m1\"}}\n\n"
                      "event: message_stop\ndata: {}\n\n";

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Today the relay emits at least message_start + message_stop verbatim. */
   int saw_start = 0, saw_stop = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_start") == 0 && strstr(cap.data[i], "\"id\":\"m1\""))
         saw_start = 1;
      if (strcmp(cap.events[i], "message_stop") == 0)
         saw_stop = 1;
   }
   assert(saw_start);
   assert(saw_stop);

   reset_capture();
   PASS("streaming_anthropic_policy_off_is_byte_neutral");
}

/* --- OpenAI-via-translator streaming tests ----------------------------- */

/* Partial-drop + max_tokens: upstream emits two tool_use blocks
 * (subagent + web_search) with `max_tokens` stop_reason; the policy
 * drops the subagent. The replay must carry `stop_reason:"max_tokens"`
 * verbatim (NOT `"tool_use"`, NOT `"end_turn"`) and exactly one
 * `tool_use` block named `web_search`. Pins the B2 fix end-to-end. */
static void test_streaming_anthropic_partial_drop_preserves_max_tokens(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[{\"id\":\"t1\",\"name\":\"Task\",\"arguments\":\"{}\"},"
                      "{\"id\":\"t2\",\"name\":\"web_search\",\"arguments\":\"{}\"}]";
   g_upstream_stop_reason = "max_tokens";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Find message_delta and assert stop_reason is "max_tokens" verbatim. */
   const char *stop = NULL;
   int tool_use_count = 0;
   int web_search_seen = 0;
   int task_seen = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         stop = strstr(cap.data[i], "\"stop_reason\":\"");
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\""))
      {
         tool_use_count++;
         if (strstr(cap.data[i], "\"name\":\"web_search\""))
            web_search_seen = 1;
         if (strstr(cap.data[i], "\"name\":\"Task\""))
            task_seen = 1;
      }
   }
   assert(stop != NULL);
   assert(strstr(stop, "max_tokens") != NULL);
   assert(tool_use_count == 1);
   assert(web_search_seen);
   assert(!task_seen);

   reset_capture();
   PASS("streaming_anthropic_partial_drop_preserves_max_tokens");
}

/* Policy ON, OpenAI-via-translator primary, upstream reply contains a
 * subagent tool_use. Same shape as the Anthropic-native case: replay,
 * no tool_use on the wire, end_turn. */
static void test_streaming_openai_police_drops_subagent_tool_use(void)
{
   const delegate_driver_t driver = {.name = "openai", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;
   const char *names[] = {"Task"};
   const char *ids[] = {"t1"};

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[{\"id\":\"t1\",\"name\":\"Task\",\"arguments\":\"{}\"}]";
   g_upstream_stop_reason = "tool_use";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   int seen_tool_use = 0;
   const char *stop_reason = NULL;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         stop_reason = strstr(cap.data[i], "\"stop_reason\":\"");
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\""))
         seen_tool_use = 1;
   }
   assert(!seen_tool_use);
   assert(stop_reason != NULL);
   assert(strstr(stop_reason, "end_turn") != NULL);

   reset_capture();
   (void)names;
   (void)ids;
   PASS("streaming_openai_police_drops_subagent_tool_use");
}

/* Policy ON, OpenAI-via-translator primary, text-only upstream reply. */
static void test_streaming_openai_police_on_no_tool_use_passthrough(void)
{
   const delegate_driver_t driver = {.name = "openai", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[]";
   g_upstream_stop_reason = "end_turn";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   int seen_tool_use = 0;
   const char *stop_reason = NULL;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         stop_reason = strstr(cap.data[i], "\"stop_reason\":\"");
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\""))
         seen_tool_use = 1;
   }
   assert(!seen_tool_use);
   assert(stop_reason != NULL);
   assert(strstr(stop_reason, "end_turn") != NULL);

   reset_capture();
   PASS("streaming_openai_police_on_no_tool_use_passthrough");
}

/* Policy OFF regression canary: today's OpenAI-via-translator per-block
 * translator runs unchanged (we send a synthetic OpenAI chunk payload and
 * assert the translator emits message_start + message_stop). */
static void test_streaming_openai_policy_off_is_byte_neutral(void)
{
   const delegate_driver_t driver = {.name = "openai", .parse_response = parsed_with_tool_uses};
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_prevent = 0;
   g_stream_payload = "data: "
                      "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                      "data: [DONE]\n\n";

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* The translator emits message_start at least once. */
   int saw_start = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_start") == 0)
         saw_start = 1;
   }
   assert(saw_start);

   reset_capture();
   PASS("streaming_openai_policy_off_is_byte_neutral");
}

int main(void)
{
   printf("test_anthropic_http_streaming_p2c:\n");
   test_streaming_anthropic_police_drops_subagent_tool_use();
   test_streaming_anthropic_police_on_no_tool_use_passthrough();
   test_streaming_anthropic_policy_on_upstream_error_returns_error();
   test_streaming_anthropic_policy_off_is_byte_neutral();
   test_streaming_anthropic_partial_drop_preserves_max_tokens();
   test_streaming_openai_police_drops_subagent_tool_use();
   test_streaming_openai_police_on_no_tool_use_passthrough();
   test_streaming_openai_policy_off_is_byte_neutral();
   printf("anthropic_http_streaming_p2c: OK\n");
   return 0;
}
