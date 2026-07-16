/* test_anthropic_http_p2c.c: P2c (response-side tool policing) integration tests
 * for the buffered Anthropic /v1/messages path. Like test_anthropic_http.c, this
 * includes the production anthropic_http.c so we can drive the private
 * messages_buffered() end-to-end. Unlike that test, the real gateway_policy.o
 * is linked so the production police function runs against the driver's
 * parsed response. The two tests cover the all-dropped and partial-drop
 * manifestations of B2 (audit/wire stop_reason consistency). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/aimee.h"
#include "../headers/agent_config.h"
#include "../headers/agent_exec.h"
#include "../headers/agent_protocol.h"
#include "../headers/delegate_driver.h"
#include "../headers/server_http.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static const delegate_driver_t *g_driver;
static char *g_last_body;
static char *g_last_extra; /* upstream extra-header block from the last post */
static int g_stream_status = 200;
static const char *g_stream_payload;
/* Fake upstream response body. The P2c tests set this to a real Anthropic
 * response carrying `tool_use` blocks so the police function has something
 * to police. NULL = empty "{}" (preserves shape-test behavior). */
static const char *g_response_body;
static int g_response_status = 200;
/* P2c policy gate. The real config_load is stubbed here to honor this global;
 * flip g_prevent to 1 to enable the response-side tool policing. */
static int g_prevent = 0;
/* Tool-use fixtures for parsed_with_tool_uses. The JSON is a cJSON array of
 * {id, name, arguments} objects; the parser translates it into a populated
 * parsed_response_t.calls[] so messages_buffered's parse step has real
 * tool_use blocks to feed the police function. */
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
/* Real gateway_policy.o is linked into this test (via Rules.mk), so the
 * P2c police function runs as in production. We DO stub the request-side
 * helpers (they don't matter for response-side policing) to keep the test
 * focused on what B2 tests. */

/* Minimal config_load: only `gateway_prevent_subagents` is read by the
 * police function. Tests set g_prevent to flip the policy on/off. The real
 * config.c depends on the home dir + YAML loader, which are out of scope
 * for a minimal-link integration test. */
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->gateway_prevent_subagents = g_prevent;
   }
   return 0;
}

/* Minimal guardrails_canonical_tool_name: maps Task/Agent/spawn_agent to
 * "Subagent" (matches the production canonicalization used by
 * gateway_policy.c via the real guardrails_orchestrator.o). Tests don't
 * need the full guardrails chain. */
const char *guardrails_canonical_tool_name(const char *n)
{
   if (n && (strcmp(n, "Task") == 0 || strcmp(n, "Agent") == 0 || strcmp(n, "spawn_agent") == 0))
      return "Subagent";
   return n ? n : "";
}

#include "../server/anthropic_http.c"

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

/* P2c parser: populates parsed_response_t.calls[] from the JSON list in
 * g_tool_uses_json, leaves content empty, and copies g_upstream_stop_reason
 * into parsed.stop_reason. The real gateway_policy_police_parsed_response
 * then mutates this struct in place; anthropic_response_from_parsed reads
 * call_count to render the wire. */
static void parsed_with_tool_uses(cJSON *root, const char *body, parsed_response_t *out)
{
   cJSON *arr;
   int i;
   int cap;
   (void)root;
   (void)body;
   memset(out, 0, sizeof(*out));
   if (g_upstream_stop_reason)
   {
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", g_upstream_stop_reason);
   }
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

/* P2c (response-side tool policing, buffered) integration tests. The real
 * `gateway_policy.o` is linked into this test target so the production police
 * function runs against the driver's parsed response. The two tests cover the
 * all-dropped and partial-drop cases (the two integration-scope manifestations
 * of B2). */

/* All subagent: the police function drops every tool_use, the renderer
 * recomputes stop_reason to "end_turn", and the wire carries no tool_use
 * block. */
static void test_messages_buffered_anthropic_police_drops_subagent_tool_use(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   char resp[8192];
   cJSON *sent;
   cJSON *content;
   cJSON *blk;
   int tool_use_count;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[{\"id\":\"t1\",\"name\":\"Task\",\"arguments\":\"{}\"}]";
   g_upstream_stop_reason = "tool_use";
   g_prevent = 1; /* P2c policy on */

   assert(messages_buffered("{\"model\":\"ignored\",\"max_tokens\":16,"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);

   sent = parse(resp);
   assert(sent != NULL);
   assert(strcmp(obj(sent, "stop_reason")->valuestring, "end_turn") == 0);
   content = (cJSON *)obj(sent, "content");
   assert(cJSON_IsArray(content));
   tool_use_count = 0;
   cJSON_ArrayForEach(blk, content)
   {
      const cJSON *type = obj(blk, "type");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0)
         tool_use_count++;
   }
   assert(tool_use_count == 0);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_anthropic_police_drops_subagent_tool_use");
}

/* Mixed: one subagent + one non-subagent. The subagent is dropped, the
 * non-subagent survives, stop_reason stays "tool_use". The renderer reads
 * call_count to derive stop_reason (line 435 of anthropic_ingress.c) so
 * the wire stop_reason == "tool_use" matches the surviving call. */
static void test_messages_buffered_anthropic_police_keeps_non_subagent_tool_use(void)
{
   const delegate_driver_t driver = {.name = "anthropic", .parse_response = parsed_with_tool_uses};
   char resp[8192];
   cJSON *sent;
   cJSON *content;
   cJSON *blk;
   const cJSON *type;
   const cJSON *name;
   int tool_use_count = 0;

   reset_capture();
   g_driver = &driver;
   g_tool_uses_json = "[{\"id\":\"t1\",\"name\":\"Task\",\"arguments\":\"{}\"},"
                      "{\"id\":\"t2\",\"name\":\"web_search\",\"arguments\":\"{}\"}]";
   g_upstream_stop_reason = "tool_use";
   g_prevent = 1; /* P2c policy on */

   assert(messages_buffered("{\"model\":\"ignored\",\"max_tokens\":16,"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                            resp, sizeof(resp)) == 200);

   sent = parse(resp);
   assert(sent != NULL);
   assert(strcmp(obj(sent, "stop_reason")->valuestring, "tool_use") == 0);
   content = (cJSON *)obj(sent, "content");
   assert(cJSON_IsArray(content));
   cJSON_ArrayForEach(blk, content)
   {
      type = obj(blk, "type");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0)
      {
         tool_use_count++;
         name = obj(blk, "name");
         assert(cJSON_IsString(name));
         assert(strcmp(name->valuestring, "web_search") == 0);
      }
   }
   assert(tool_use_count == 1);
   cJSON_Delete(sent);
   reset_capture();
   PASS("messages_buffered_anthropic_police_keeps_non_subagent_tool_use");
}

int main(void)
{
   test_messages_buffered_anthropic_police_drops_subagent_tool_use();
   test_messages_buffered_anthropic_police_keeps_non_subagent_tool_use();
   printf("anthropic_http_p2c: OK\n");
   return 0;
}
