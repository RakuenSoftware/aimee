/* test_anthropic_http_streaming_p2c.c: P2c streaming-side response-policing
 * integration tests for the Anthropic /v1/messages path. Like the buffered
 * P2c test (test_anthropic_http_p2c.c), this links the real gateway_policy.o
 * so the production police function runs end-to-end. The two streaming
 * paths (Anthropic-native raw-byte relay, OpenAI-via-translator per-block)
 * both bypass their incremental paths when the policy is ON, fall through
 * to a buffered upstream fetch + police + canonical response SSE renderer replay.
 *
 * Scope: covers the policy-ON path (drops + replays) AND the policy-OFF
 * regression canary (native incremental output and Chat canonical replay). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/aimee.h"
#include "../headers/agent_config.h"
#include "../headers/agent_exec.h"
#include "../headers/agent_protocol.h"
#include "../headers/anthropic_ingress.h"
#include <aimee/delegates/delegate_driver.h>
#include "gw_stage_governance.h"
#include "../headers/log.h"
#include "../headers/server_http.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

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

static const delegate_driver_t *g_driver;
static char *g_last_body;
static char *g_last_extra;
static int g_stream_status;
static const char *g_stream_payload;
static size_t g_stream_payload_length;
/* Fake upstream response body for the BUFFERED fetch on the policy-ON path.
 * The streaming path uses agent_http_post (not the streaming variant) when
 * the policy is on, so we hook the same `g_response_body` machinery as the
 * buffered P2c test. NULL = empty "{}" (regression-canary default). */
static const char *g_response_body;
static int g_response_status;
/* P2c policy gate. The real legacy_config_read is stubbed to honor this global;
 * flip g_prevent to 1 to enable the streaming-side tool policing. */
static int g_prevent;
/* Tool-use fixtures for parsed_with_tool_uses (the driver parse_response
 * stub reads `g_tool_uses_json` and synthesizes a populated parsed struct). */
static const char *g_tool_uses_json;
static const char *g_upstream_stop_reason;
static const char *g_response_content;

static int governance_event_bus_provider(int policy_active, const aimee_tool_identity_view_t *tools,
                                         uint32_t tool_count, const char *stop_reason,
                                         aimee_governance_decision_t *decision)
{
   memset(decision, 0, sizeof(*decision));
   decision->keep_mask = aimee_governance_mask_for_count(tool_count);
   snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
            stop_reason ? stop_reason : "");
   if (!policy_active)
      return 0;
   decision->keep_mask = 0;
   for (uint32_t i = 0; i < tool_count; ++i)
   {
      int denied =
          strcmp(tools[i].name, "Agent") == 0 || strcmp(tools[i].name, "spawn_agent") == 0 ||
          strcmp(tools[i].name, "RemoteTrigger") == 0 || strcmp(tools[i].name, "Task") == 0;
      if (denied)
         decision->drop_count++;
      else
         decision->keep_mask |= 1u << i;
   }
   uint32_t kept = tool_count - decision->drop_count;
   if (!decision->stop_reason[0] || kept == 0)
      snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
               kept > 0 ? "tool_use" : "end_turn");
   return 0;
}

static void reset_capture(void)
{
   gw_response_governance_register_provider(governance_event_bus_provider);
   free(g_last_body);
   g_last_body = NULL;
   free(g_last_extra);
   g_last_extra = NULL;
   g_stream_status = 200;
   g_stream_payload = NULL;
   g_stream_payload_length = 0;
   g_response_body = NULL;
   g_response_status = 200;
   g_prevent = 0;
   g_tool_uses_json = NULL;
   g_upstream_stop_reason = NULL;
   g_response_content = NULL;
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
   snprintf(url, url_len, "https://example.invalid/v1/messages");
   return 0;
}

int agent_resolve_auth_headers(const agent_t *agent, char *buf, size_t buf_len, char *headers,
                               size_t headers_capacity)
{
   (void)agent;
   if (headers_capacity)
      headers[0] = 0;
   snprintf(buf, buf_len, "Bearer test");
   return 0;
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
   *response_buf = g_response_body ? strdup(g_response_body)
                                   : native_response_fixture(g_driver ? g_driver->name : NULL,
                                                             g_response_content, g_tool_uses_json,
                                                             g_upstream_stop_reason);
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
       callback(g_stream_payload,
                g_stream_payload_length ? g_stream_payload_length : strlen(g_stream_payload),
                userdata))
      return -1;
   return g_stream_status;
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
/* The context_reduce / result_free / reduce-ledger stubs that stood here are gone:
 * the C reducer moved to the Go economizer module, so messages_run_request_pipeline
 * references none of those symbols any more and the minimal P2c link stays minimal
 * without them. */
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

/* Accessor stubs mirror this fixture's policy — prevent_subagents
 * tracks g_prevent, pin_model was left zeroed — so assertions are unchanged. */
int config_gateway_prevent_subagents(void)
{
   return g_prevent;
}

int config_gateway_pin_model(void)
{
   return 0;
}

/* The economizer fixture leaves the
 * economizer zeroed, so the live-config form must report OFF. */
int econ_mode_current(void)
{
   return ECON_MODE_OFF;
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
 * canonical response SSE renderer replays the surviving sequence. No tool_use block
 * on the wire; stop_reason "end_turn" (all-dropped rewrite). */
static void test_streaming_anthropic_police_drops_subagent_tool_use(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
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

   /* No content survived; emit a complete empty turn without inventing a
    * model-authored text block. */
   assert(cap.count == 3);
   assert(!strcmp(cap.events[0], "message_start"));
   assert(!strcmp(cap.events[2], "message_stop"));
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
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[]";
   g_response_content = "plain native answer";
   g_upstream_stop_reason = "end_turn";
   g_prevent = 1;

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* message_start, [text_start, text_stop], message_delta, message_stop. */
   assert(cap.count == 6);
   assert(strstr(cap.data[2], "plain native answer"));
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
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
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

/* Policy OFF still admits native frames through canonical IR. */
static void test_streaming_anthropic_policy_off_uses_ir(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_prevent = 0;
   g_stream_payload =
       "event: message_start\ndata: "
       "{\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"role\":\"assistant\",\"usage\":{"
       "\"input_tokens\":0,\"output_tokens\":0}}}\n\n"
       "event: message_delta\ndata: "
       "{\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":0}}\n\n"
       "event: message_stop\ndata: {}\n\n";

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Native identity and the admitted completion survive client rendering. */
   int saw_start = 0, saw_stop = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_start") == 0 && strstr(cap.data[i], "\"id\":\"m1\""))
         saw_start = 1;
      if (strcmp(cap.events[i], "message_stop") == 0)
         saw_stop = 1;
   }
   assert(saw_start);
   assert(!strcmp(cap.events[cap.count - 1], "message_stop"));
   assert(saw_stop);

   reset_capture();
   PASS("streaming_anthropic_policy_off_uses_ir");
}

/* --- OpenAI-via-translator streaming tests ----------------------------- */

/* Partial-drop + max_tokens: upstream emits two tool_use blocks
 * (subagent + web_search) with `max_tokens` stop_reason; the policy
 * drops the subagent. The replay must carry `stop_reason:"max_tokens"`
 * verbatim (NOT `"tool_use"`, NOT `"end_turn"`) and exactly one
 * `tool_use` block named `web_search`. Pins the B2 fix end-to-end. */
static void test_streaming_anthropic_partial_drop_preserves_max_tokens(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
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
   const delegate_driver_t driver = {
       .name = "openai",
   };
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
   const delegate_driver_t driver = {
       .name = "openai",
   };
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

/* Policy OFF still runs canonical assembly and replay. A native completion
 * event must precede [DONE], so EOF cannot invent a successful turn. */
static void test_streaming_openai_policy_off_replays_observed_usage(void)
{
   const delegate_driver_t driver = {
       .name = "openai",
   };
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_prevent = 0;
   g_stream_payload =
       "data: "
       "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
       "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
       "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}\n\n"
       "data: [DONE]\n\n";

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,"
                   "\"stream\":true,"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                   replay_capture, &cap);

   /* Canonical replay emits message_start with actual observed input usage. */
   int saw_start = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_start") == 0)
         saw_start = 1;
   }
   assert(saw_start);

   reset_capture();
   PASS("streaming_openai_policy_off_replays_observed_usage");
}

/* An Anthropic client can stream against an OpenAI-compatible delegate. Once
 * that ordinary session has edited code, completion inspection needs the whole
 * response before any bytes reach the client. This pins the branch condition:
 * an armed turn is buffered even when subagent policing is off. */
static void assert_streaming_armed_completion_is_buffered(const char *driver_name)
{
   const delegate_driver_t driver = {
       .name = driver_name,
   };
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[]";
   g_upstream_stop_reason = "end_turn";
   g_response_content =
       "The same vulnerability exists in another handler, but I left it unchanged. Want me to "
       "fix it?";

   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,\"stream\":true,"
                   "\"tools\":[{\"name\":\"Bash\",\"input_schema\":{\"type\":\"object\","
                   "\"properties\":{\"command\":{\"type\":\"string\"}}}}],"
                   "\"messages\":[{\"role\":\"user\",\"content\":\"repair it\"},"
                   "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"e1\","
                   "\"name\":\"Bash\",\"input\":{\"command\":\"cat > app/file.py\"}}]},"
                   "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
                   "\"tool_use_id\":\"e1\",\"content\":\"ok\"}]}]}",
                   replay_capture, &cap);

   int saw_gate_tool = 0;
   int saw_gate_marker = 0;
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "content_block_start") == 0 &&
          strstr(cap.data[i], "\"type\":\"tool_use\"") && strstr(cap.data[i], "\"name\":\"Bash\""))
         saw_gate_tool = 1;
      if (strstr(cap.data[i], AIMEE_COMPLETION_CALL_PREFIX))
         saw_gate_marker = 1;
   }
   assert(saw_gate_tool);
   assert(saw_gate_marker);

   reset_capture();
}

static void test_streaming_armed_completion_is_provider_neutral(void)
{
   assert_streaming_armed_completion_is_buffered("openai");
   assert_streaming_armed_completion_is_buffered("anthropic");
   PASS("streaming_armed_completion_is_provider_neutral");
}

static void test_native_completion_followup_reaches_provider_system(void)
{
   const delegate_driver_t driver = {
       .name = "anthropic",
   };
   replay_cap_t cap;

   reset_capture();
   g_driver = &driver;
   g_stream_payload =
       "event: message_start\ndata: "
       "{\"message\":{\"id\":\"m1\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":0,\"output_"
       "tokens\":0}}}\n\n"
       "event: message_delta\ndata: "
       "{\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":0}}\n\n"
       "event: message_stop\ndata: {}\n\n";
   memset(&cap, 0, sizeof(cap));
   messages_stream("{\"model\":\"ignored\",\"max_tokens\":16,\"stream\":true,"
                   "\"system\":\"existing system\","
                   "\"messages\":[{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\","
                   "\"id\":\"call_aimee_completion_42\",\"name\":\"Bash\","
                   "\"input\":{\"command\":\"true\"}}]},"
                   "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
                   "\"tool_use_id\":\"call_aimee_completion_42\",\"content\":\"\"}]}]}",
                   replay_capture, &cap);

   assert(g_last_body != NULL);
   assert(strstr(g_last_body, "existing system") != NULL);
   assert(strstr(g_last_body, "aimee-completion-policy") != NULL);
   reset_capture();
   PASS("native_completion_followup_reaches_provider_system");
}

static cJSON *signed_snapshot;
static int reject_signed_snapshot;
static int signed_capture(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (event->kind != AIMEE_SESSION_MESSAGE)
      return 0;
   cJSON_Delete(signed_snapshot);
   signed_snapshot = aimee_ir_message_json(event->message);
   assert(signed_snapshot);
   assert(event->message->n_blocks == 1);
   assert(event->message->blocks[0].thinking_binding);
   return reject_signed_snapshot ? -1 : 0;
}
static void test_signed_stream_snapshot_before_completion(void)
{
   const delegate_driver_t driver = {.name = "anthropic"};
   for (int reject = 0; reject < 2; reject++)
   {
      reset_capture();
      aimee_ir_session_set_sink(NULL, NULL);
      reject_signed_snapshot = reject;
      g_driver = &driver;
      g_stream_payload =
          "event: message_start\ndata: "
          "{\"message\":{\"id\":\"signed\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":0,"
          "\"output_tokens\":0}}}"
          "\n\n"
          "event: content_block_start\ndata: "
          "{\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
          "event: content_block_delta\ndata: "
          "{\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"reason\"}}\n\n"
          "event: content_block_delta\ndata: "
          "{\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"opaque-signature\"}"
          "}\n\n"
          "event: content_block_stop\ndata: {\"index\":0}\n\n"
          "event: message_delta\ndata: "
          "{\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":0}}\n\n"
          "event: message_stop\ndata: {}\n\n";
      aimee_ir_session_set_sink(signed_capture, NULL);
      replay_cap_t cap = {0};
      messages_stream("{\"model\":\"ignored\",\"max_tokens\":64,\"stream\":true,"
                      "\"messages\":[{\"role\":\"user\",\"content\":\"original\"}]}",
                      replay_capture, &cap);
      assert(signed_snapshot && g_last_body);
      int stopped = 0;
      for (int i = 0; i < cap.count; i++)
         stopped += !strcmp(cap.events[i], "message_stop");
      assert(stopped == !reject);
      if (!reject)
      {
         aimee_request_t request = {0};
         char error[128];
         assert(!aimee_frontend_parse_wire(g_last_body, strlen(g_last_body), AIMEE_WIRE_ANTHROPIC,
                                           &request, error, sizeof error));
         request.messages = realloc(request.messages, 2 * sizeof *request.messages);
         assert(request.messages);
         memset(&request.messages[1], 0, sizeof request.messages[1]);
         assert(!aimee_ir_message_parse(signed_snapshot, &request.messages[1]));
         request.n_messages = 2;
         cJSON *native = anthropic_backend_build(&request);
         char *json = cJSON_PrintUnformatted(native);
         assert(json && strstr(json, "opaque-signature"));
         free(json);
         cJSON_Delete(native);
         aimee_request_free(&request);
      }
      aimee_ir_session_set_sink(NULL, NULL);
      cJSON_Delete(signed_snapshot);
      signed_snapshot = NULL;
   }
   reset_capture();
   PASS("signed_native_stream_persists_prefix_bound_snapshot_before_client_completion");
}

static void test_strict_provider_stream_frames(void)
{
   static const char native_nul[] =
       "event: message_start\ndata: "
       "{\"message\":{\"id\":\"before\\u0000after\",\"role\":"
       "\"assistant\",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}\n\n";
   static const char native_raw_nul[] =
       "event: message_start\ndata: "
       "{\"message\":{\"id\":\"prefix\",\"role\":\"assistant\"}}\0{}\n\n";
   static const char native_trailing[] =
       "event: message_start\ndata: {\"message\":{\"id\":\"prefix\",\"role\":\"assistant\"}} "
       "{}\n\n";
   static const char chat_nul[] =
       "data: {\"choices\":[{\"delta\":{\"content\":\"before\\u0000after\"}}]}\n\n";
   static const char chat_raw_nul[] =
       "data: {\"choices\":[{\"delta\":{\"content\":\"prefix\"}}]}\0{}\n\n";
   static const char chat_duplicate[] =
       "data: {\"choices\":[],\"choices\":[{\"delta\":{\"content\":\"prefix\"}}]}\n\n";
   struct
   {
      const char *driver, *bytes;
      size_t length;
   } cases[] = {{"anthropic", native_nul, sizeof native_nul - 1},
                {"anthropic", native_raw_nul, sizeof native_raw_nul - 1},
                {"anthropic", native_trailing, sizeof native_trailing - 1},
                {"openai", chat_nul, sizeof chat_nul - 1},
                {"openai", chat_raw_nul, sizeof chat_raw_nul - 1},
                {"openai", chat_duplicate, sizeof chat_duplicate - 1}};
   for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
   {
      reset_capture();
      aimee_ir_session_set_sink(NULL, NULL);
      delegate_driver_t driver = {.name = cases[i].driver};
      g_driver = &driver;
      g_stream_payload = cases[i].bytes;
      g_stream_payload_length = cases[i].length;
      assert(memchr(cases[i].bytes, '\n', cases[i].length));
      if (i == 1 || i == 4)
         assert(strlen(cases[i].bytes) < cases[i].length); /* Exercise the full byte frame. */
      else
         assert(strlen(cases[i].bytes) == cases[i].length);
      replay_cap_t cap = {0};
      messages_stream("{\"model\":\"ignored\",\"max_tokens\":64,\"stream\":true,\"messages\":[{"
                      "\"role\":\"user\",\"content\":\"original\"}]}",
                      replay_capture, &cap);
      int errors = 0;
      for (int j = 0; j < cap.count; j++)
      {
         errors += !strcmp(cap.events[j], "error");
         assert(strcmp(cap.events[j], "message_stop"));
         assert(strcmp(cap.events[j], "content_block_start"));
      }
      assert(errors == 1);
   }
   reset_capture();
   aimee_ir_session_set_sink(NULL, NULL);
   PASS("strict_provider_stream_frames");
}

int main(void)
{
   printf("test_anthropic_http_streaming_p2c:\n");
   test_streaming_anthropic_police_drops_subagent_tool_use();
   test_streaming_anthropic_police_on_no_tool_use_passthrough();
   test_streaming_anthropic_policy_on_upstream_error_returns_error();
   test_streaming_anthropic_policy_off_uses_ir();
   test_signed_stream_snapshot_before_completion();
   test_strict_provider_stream_frames();
   test_streaming_anthropic_partial_drop_preserves_max_tokens();
   test_streaming_openai_police_drops_subagent_tool_use();
   test_streaming_openai_police_on_no_tool_use_passthrough();
   test_streaming_openai_policy_off_replays_observed_usage();
   test_streaming_armed_completion_is_provider_neutral();
   test_native_completion_followup_reaches_provider_system();
   printf("anthropic_http_streaming_p2c: OK\n");
   return 0;
}

/* This policing integration fixture explicitly enables
 * the governance module; the module's unspecified production default is
 * covered by test_response_governance_stage.c. */
int config_present(void)
{
   return 1;
}

int config_module_governance(void)
{
   return 1;
}

int config_ingress_preinject_anthropic_enabled(void)
{
   return 0;
}
