/* test_agent_list_handler.c: handle_agent_list must distinguish a config LOAD
 * FAILURE from a genuinely-empty roster.
 *
 * Regression for the appliance incident where a missing/stale agents.json made
 * every caller see "no agents configured": the handler memset the config to
 * zero on load failure and returned {"status":"ok","agents":[]}, which is
 * byte-identical to a config that really has zero agents. This test pins the
 * two apart. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "agent_exec.h"
#include "server.h"
#include "vault_capability.h"
#include "vault_service.h"

/* --- capture layer: stub the transport so we can read what the handler sent --- */

static cJSON *g_last_response = NULL;
static char g_last_error[256];

/* server_send_ok is a static inline in server.h that calls this and then frees
 * resp itself, so this stub must NOT delete resp — it only snapshots it. Same
 * contract the real transport and every other handler test rely on. */
int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   if (g_last_response)
      cJSON_Delete(g_last_response);
   g_last_response = cJSON_Duplicate(resp, 1);
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

/* The classified variant. handle_agent_probe's argument check calls this one, so
 * the stub layer has to cover it too or the target does not link. Captured into
 * the same buffer: these tests assert on the message, and the kind is asserted
 * where it decides an HTTP status (runtime-web's TestRPCErrorStatus...). */
char g_last_error_kind[64];
int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error_kind, sizeof(g_last_error_kind), "%s", kind ? kind : "");
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

/* Key writes are outside this handler test's scope, but referencing agent.add
 * and agent.set retains their key branch at link time. Keep those seams inert. */
int vault_agent_key_server_seal_allowed(attested_transport_t transport)
{
   (void)transport;
   return 1;
}

vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   (void)agent;
   (void)cred;
   (void)secret;
   return VAULT_OK;
}

void vault_audit_server_write(const server_conn_t *conn, const char *agent, const char *cred,
                              const char *secret)
{
   (void)conn;
   (void)agent;
   (void)cred;
   (void)secret;
}

/* Probe transport seams. This target intentionally links no production agent
 * transport: the handler's observable response and backend selection are under
 * test, while these stubs record the exact executor contract it chose. */
static int g_cli_calls, g_http_calls, g_execute_failure;
static agent_t g_executed_agent;

void agent_http_init(void)
{
}

int agent_execute(const agent_t *agent, const char *system_prompt, const char *user_prompt,
                  int max_tokens, double temperature, agent_result_t *out)
{
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   g_http_calls++;
   g_executed_agent = *agent;
   if (g_execute_failure)
   {
      snprintf(out->error, sizeof(out->error), "injected HTTP failure");
      return -1;
   }
   out->response = strdup("ok");
   out->latency_ms = 11;
   return 0;
}

int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature,
                                      agent_result_t *out)
{
   (void)network;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   assert(strcmp(role, "explain") == 0);
   g_cli_calls++;
   g_executed_agent = *agent;
   if (g_execute_failure)
   {
      snprintf(out->error, sizeof(out->error), "injected CLI failure");
      return -1;
   }
   out->response = strdup("ok");
   out->latency_ms = 12;
   return 0;
}

int agent_http_get(const char *url, const char *headers, char **response_buf, int timeout_ms)
{
   (void)headers;
   (void)timeout_ms;
   if (strstr(url, "/models"))
      *response_buf = strdup("{\"data\":[{\"id\":\"http-model\"}]}");
   else if (strstr(url, "/slots"))
      *response_buf = strdup("{\"slots\":3,\"context_window\":8192}");
   else
      return -1;
   return 200;
}

/* --- helpers --- */

static char g_home[256];

static void set_home_empty(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-agentlist-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(g_home) != NULL);
   assert(platform_setenv("AIMEE_HOME", g_home) == 0);
   unsetenv("AIMEE_NO_CACHE");
}

static void write_agents(const char *json)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/agents.json", g_home);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);
}

static void reset_capture(void)
{
   if (g_last_response)
      cJSON_Delete(g_last_response);
   g_last_response = NULL;
   g_last_error[0] = '\0';
   g_cli_calls = g_http_calls = g_execute_failure = 0;
   memset(&g_executed_agent, 0, sizeof(g_executed_agent));
}

static cJSON *probe_request(const char *name, int no_run)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   cJSON_AddItemToArray(args, cJSON_CreateString(name));
   if (no_run)
      cJSON_AddItemToArray(args, cJSON_CreateString("--no-run"));
   return req;
}

static int response_bool(const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(g_last_response, name);
   return cJSON_IsTrue(v);
}

/* --- tests --- */

static void test_load_failure_is_an_error_not_empty(void)
{
   set_home_empty(); /* no agents.json in this home -> agent_load_config fails */
   reset_capture();

   assert(handle_agent_list(NULL, NULL, NULL) == 0);

   /* Must NOT be a silent success with an empty roster. */
   assert(g_last_response == NULL);
   assert(g_last_error[0] != '\0');
   printf("  PASS: a config load failure returns an error, not an empty roster\n");
}

static void test_empty_config_is_ok_with_empty_array(void)
{
   set_home_empty();
   write_agents("{\"agents\":[]}\n"); /* loads fine, genuinely zero agents */
   reset_capture();

   assert(handle_agent_list(NULL, NULL, NULL) == 0);

   /* This case IS a legitimate empty roster: ok + empty array, no error. */
   assert(g_last_error[0] == '\0');
   assert(g_last_response != NULL);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(g_last_response, "status");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   assert(cJSON_IsArray(agents) && cJSON_GetArraySize(agents) == 0);
   printf("  PASS: a real config with zero agents is ok with an empty array\n");
}

static void test_populated_config_lists_agents(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"]}]}\n");
   reset_capture();

   assert(handle_agent_list(NULL, NULL, NULL) == 0);

   assert(g_last_error[0] == '\0');
   assert(g_last_response != NULL);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   assert(cJSON_IsArray(agents) && cJSON_GetArraySize(agents) == 1);
   printf("  PASS: a populated config lists its agents\n");
}

/* primary_only survives parse -> agent_load_config -> server_agent_to_json (the
 * /v1/model/list surface the Web GUI reads to render the checkbox). An agent that
 * omits the field defaults to false. */
static void test_primary_only_round_trips(void)
{
   set_home_empty();
   write_agents(
       "{\"agents\":["
       "{\"name\":\"claude\",\"provider\":\"claude\",\"backend\":\"tmux-cli\","
       "\"cli_kind\":\"claude\",\"primary_only\":true,\"roles\":[\"code\"]},"
       "{\"name\":\"minimax\",\"provider\":\"anthropic\",\"model\":\"m\",\"roles\":[\"all\"]}"
       "]}\n");
   reset_capture();

   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   assert(cJSON_IsArray(agents) && cJSON_GetArraySize(agents) == 2);

   cJSON *a0 = cJSON_GetArrayItem(agents, 0); /* claude */
   cJSON *po0 = cJSON_GetObjectItemCaseSensitive(a0, "primary_only");
   assert(cJSON_IsBool(po0) && cJSON_IsTrue(po0));

   cJSON *a1 = cJSON_GetArrayItem(agents, 1); /* minimax: field omitted -> false */
   cJSON *po1 = cJSON_GetObjectItemCaseSensitive(a1, "primary_only");
   assert(cJSON_IsBool(po1) && !cJSON_IsTrue(po1));
   printf("  PASS: primary_only round-trips through the list handler\n");
}

static void test_cli_probe_uses_backend_executor_and_config_discovery(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"claude\",\"provider\":\"claude\","
                "\"backend\":\"tmux-cli\",\"cli_kind\":\"claude\",\"model\":\"opus\","
                "\"timeout_ms\":600000,\"cli_idle_timeout_ms\":600000,\"session_reuse\":true,"
                "\"max_parallel\":4,\"context_window\":200000,\"roles\":[\"explain\"]}]}\n");
   reset_capture();
   cJSON *req = probe_request("claude", 0);
   assert(handle_agent_probe(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   assert(g_last_error[0] == '\0' && g_last_response);
   assert(g_cli_calls == 1 && g_http_calls == 0);
   assert(g_executed_agent.timeout_ms == 60000);
   assert(g_executed_agent.cli_idle_timeout_ms == 60000);
   assert(g_executed_agent.session_reuse == 0);
   assert(g_executed_agent.force_cli_isolation == 1);
   assert(response_bool("model_available") && response_bool("execution_ok"));
   assert(cJSON_GetObjectItem(g_last_response, "models_status")->valueint == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "slots_source")->valuestring, "config") == 0);
   assert(cJSON_GetObjectItem(g_last_response, "model_probe") == NULL);
   printf("  PASS: CLI probe uses backend executor with bounded isolated policy\n");
}

static void test_cli_probe_failure_and_no_run_are_observable(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"cli\",\"provider\":\"x\","
                "\"backend\":\"provider-cli\",\"model\":\"m\",\"roles\":[\"explain\"]}]}\n");
   reset_capture();
   g_execute_failure = 1;
   cJSON *req = probe_request("cli", 0);
   assert(handle_agent_probe(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_cli_calls == 1 && !response_bool("execution_ok"));
   assert(strstr(cJSON_GetObjectItem(g_last_response, "execution_message")->valuestring,
                 "injected CLI failure"));

   reset_capture();
   req = probe_request("cli", 1);
   assert(handle_agent_probe(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_cli_calls == 0 && g_http_calls == 0);
   assert(cJSON_GetObjectItem(g_last_response, "execution_ok") == NULL);
   printf("  PASS: CLI probe failure and --no-run response contracts are explicit\n");
}

static void test_unknown_backend_fails_closed(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"mystery\",\"provider\":\"x\","
                "\"backend\":\"telepathy\",\"model\":\"m\",\"roles\":[\"explain\"]}]}\n");
   reset_capture();
   cJSON *req = probe_request("mystery", 0);
   assert(handle_agent_probe(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_cli_calls == 0 && g_http_calls == 0);
   assert(!response_bool("model_available") && !response_bool("execution_ok"));
   assert(cJSON_GetObjectItem(g_last_response, "models_status")->valueint == -1);
   assert(strstr(cJSON_GetObjectItem(g_last_response, "model_probe")->valuestring,
                 "unsupported agent backend"));
   printf("  PASS: unknown probe backend fails closed without transport fallback\n");
}

static void test_http_probe_preserves_discovery_and_plain_execution(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"http\",\"provider\":\"openai\","
                "\"endpoint\":\"http://model.test/v1\",\"model\":\"http-model\","
                "\"auth_type\":\"none\",\"roles\":[\"explain\"]}]}\n");
   reset_capture();
   cJSON *req = probe_request("http", 0);
   assert(handle_agent_probe(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_http_calls == 1 && g_cli_calls == 0);
   assert(response_bool("model_available") && response_bool("execution_ok"));
   assert(cJSON_GetObjectItem(g_last_response, "models_status")->valueint == 200);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "slots_source")->valuestring, "probe") == 0);
   assert(cJSON_GetObjectItem(g_last_response, "detected_slots")->valueint == 3);
   printf("  PASS: HTTP probe preserves model/slot discovery and plain execution\n");
}

/* The SERVER projection is what the GUI reads. It must carry the same identity
 * and pricing the CLI shows, or the two disagree about what an agent is and what
 * it costs. `provider` alone is ambiguous for a third-party model served over
 * another vendor's API, so catalog_provider and a canonical provider:model ref
 * are both required. */
static void test_list_exposes_catalog_identity_and_pricing(void)
{
   set_home_empty();
   write_agents("{\"agents\":["
                /* Anthropic WIRE format, MiniMax VENDOR. */
                "{\"name\":\"MiniMax-M3\",\"provider\":\"anthropic\","
                "\"endpoint\":\"https://api.minimax.io/anthropic\","
                "\"model\":\"MiniMax-M3\",\"auth_type\":\"bearer\","
                "\"api_key\":\"k\",\"roles\":[\"all\"]},"
                /* Operator-priced agent: the override must win over the catalog. */
                "{\"name\":\"priced\",\"provider\":\"anthropic\","
                "\"endpoint\":\"https://api.anthropic.com\","
                "\"model\":\"claude-opus-4-8\",\"auth_type\":\"bearer\","
                "\"api_key\":\"k\",\"price_in_per_mtok\":1.5,"
                "\"price_out_per_mtok\":2.5,\"roles\":[\"all\"]}"
                "]}\n");
   reset_capture();

   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   assert(cJSON_IsArray(agents) && cJSON_GetArraySize(agents) == 2);

   cJSON *mm = cJSON_GetArrayItem(agents, 0);
   /* Wire provider is untouched; catalog identity is the vendor. */
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(mm, "provider")),
                 "anthropic") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(mm, "catalog_provider")),
                 "minimax") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(mm, "model_ref")),
                 "minimax:MiniMax-M3") == 0);

   cJSON *pr = cJSON_GetArrayItem(agents, 1);
   cJSON *pin = cJSON_GetObjectItemCaseSensitive(pr, "price_base_in_per_mtok");
   cJSON *pout = cJSON_GetObjectItemCaseSensitive(pr, "price_base_out_per_mtok");
   assert(cJSON_IsNumber(pin) && pin->valuedouble == 1.5);
   assert(cJSON_IsNumber(pout) && pout->valuedouble == 2.5);
   cJSON *ovr = cJSON_GetObjectItemCaseSensitive(pr, "price_overridden");
   assert(cJSON_IsBool(ovr) && cJSON_IsTrue(ovr));

   /* The unpriced-override agent reports the catalog rate as NOT overridden. */
   cJSON *ovr0 = cJSON_GetObjectItemCaseSensitive(mm, "price_overridden");
   assert(cJSON_IsBool(ovr0) && !cJSON_IsTrue(ovr0));

   printf("  PASS: list exposes catalog identity and pricing\n");
}

/* Read the roles array off the captured response. */
static int response_has_role(const char *role)
{
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(g_last_response, "roles");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, roles) if (cJSON_IsString(item) &&
                                       strcmp(item->valuestring, role) == 0) return 1;
   return 0;
}

static cJSON *args_request(const char *a, const char *b)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   cJSON_AddItemToArray(args, cJSON_CreateString(a));
   if (b)
      cJSON_AddItemToArray(args, cJSON_CreateString(b));
   return req;
}

static cJSON *args_request_array(const char *const *values, int count)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(args, cJSON_CreateString(values[i]));
   return req;
}

/* The Providers UI writes per-model values through agent.set. The distinction
 * this pins is the one a bare number cannot carry: a price of 0 that the
 * operator STATED (a free or subscription seat) versus a price nobody stated.
 * Both read as 0; only the declared flag separates them, and if the handler
 * loses that flag the config layer drops the statement on its next save. */
static void test_set_declares_prices_and_limits(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"]}]}\n");
   reset_capture();
   /* --price-in 0 is the statement "this seat is free"; --price-out is simply
    * not mentioned, which states nothing. */
   const char *args[] = {"a1",    "--context-window", "200000", "--max-output",
                         "64000", "--price-in",       "0"};
   cJSON *req = args_request_array(args, 7);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);

   /* The declared capacities survive and report themselves as the operator's. */
   assert(cJSON_GetObjectItemCaseSensitive(a1, "effective_context_window")->valueint == 200000);
   assert(cJSON_GetObjectItemCaseSensitive(a1, "effective_max_output")->valueint == 64000);
   assert(
       strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a1, "context_window_source")),
              "declared") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a1, "max_output_source")),
                 "declared") == 0);

   /* The stated-free price is flagged; the unmentioned one is not. Without the
    * flag these two are the same byte. */
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a1, "price_in_declared")));
   assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a1, "price_out_declared")));
   printf("  PASS: agent set records declared prices and limits\n");
}

/* A capacity of 0 is not a capacity. The Agents edit form always sends
 * "--context-window 0" for an unset field, so keying the declaration on the
 * option being PRESENT would stamp an explicit 0 into every config it touched
 * and assert a limit nobody chose. */
static void test_set_zero_capacity_is_not_a_declaration(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"]}]}\n");
   reset_capture();
   const char *args[] = {"a1", "--context-window", "0"};
   cJSON *req = args_request_array(args, 3);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0');

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);
   /* Not "declared" -- either resolved from the catalog, or honestly unknown. */
   const char *src =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a1, "context_window_source"));
   assert(src && strcmp(src, "declared") != 0);
   printf("  PASS: a zero capacity is not recorded as a declaration\n");
}

/* A price that does not parse must not be accepted as 0 -- that would assert
 * "this model is free" from a typo, which is a worse outcome than ignoring it. */
static void test_set_rejects_unparseable_price(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"]}]}\n");
   reset_capture();
   const char *args[] = {"a1", "--price-in", "abc"};
   cJSON *req = args_request_array(args, 3);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);
   assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a1, "price_in_declared")));
   printf("  PASS: an unparseable price is not accepted as free\n");
}

/* Clearing a field in the UI must actually clear it. agent.set is a PATCH, so
 * an omitted option changes nothing -- which means "I no longer state this"
 * cannot be expressed by omission. An option present with an EMPTY value is
 * that withdrawal, and the UI sends all three prices for exactly this reason. */
static void test_set_empty_price_withdraws_the_declaration(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"],\"price_in_per_mtok\":3.0}]}\n");
   reset_capture();
   const char *declared[] = {"a1", "--price-in", "0"};
   cJSON *req = args_request_array(declared, 3);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *a1 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(g_last_response, "agents"), 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a1, "price_in_declared")));

   /* Now withdraw it. */
   reset_capture();
   const char *cleared[] = {"a1", "--price-in", ""};
   req = args_request_array(cleared, 3);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   a1 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(g_last_response, "agents"), 0);
   assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a1, "price_in_declared")));
   printf("  PASS: an empty price withdraws the declaration\n");
}

/* Review is gate authority, not a generic model capability. Registering an
 * agent without --roles must not silently authorize it; explicitly naming the
 * review role still does. */
static void test_add_requires_explicit_review_role(void)
{
   set_home_empty();
   reset_capture();
   const char *plain[] = {"plain", "http://model.test/v1", "model"};
   cJSON *req = args_request_array(plain, 3);
   assert(handle_agent_add(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);
   assert(response_has_role("code"));
   assert(!response_has_role("review"));

   set_home_empty();
   reset_capture();
   const char *reviewer[] = {"reviewer", "http://model.test/v1", "model", "--roles", "code,review"};
   req = args_request_array(reviewer, 5);
   assert(handle_agent_add(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);
   assert(response_has_role("code") && response_has_role("review"));
   printf("  PASS: agent add requires an explicit review role\n");
}

/* The Agents UI always sends --roles, including an empty string when every chip
 * is off. That empty value used to expand to the full default set and silently
 * re-enable review. It must round-trip as an empty role list. */
static void test_set_empty_roles_stays_empty(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"review\"]}]}\n");
   reset_capture();
   const char *clear[] = {"a1", "--roles", ""};
   cJSON *req = args_request_array(clear, 3);
   assert(handle_agent_set(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0' && g_last_response != NULL);
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(g_last_response, "roles");
   assert(cJSON_IsArray(roles) && cJSON_GetArraySize(roles) == 0);

   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);
   roles = cJSON_GetObjectItemCaseSensitive(a1, "roles");
   assert(cJSON_IsArray(roles) && cJSON_GetArraySize(roles) == 0);
   printf("  PASS: agent set preserves an explicitly empty role selection\n");
}

/* `agent roles <name>` with no csv reads as a query and was the only way to ask
 * what an agent's roles were — but it RESET the agent to the default list,
 * silently dropping every role outside it (notably the `all` wildcard). Run
 * against a live appliance it destroyed operator configuration with no output
 * saying so. Omitting the csv must now report and write nothing; a reset has to
 * be named explicitly. */
static void test_roles_without_csv_reports_and_does_not_write(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\",\"diagnose\",\"all\"]}]}\n");
   reset_capture();

   cJSON *req = args_request("a1", NULL);
   assert(handle_agent_roles(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   assert(g_last_error[0] == '\0');
   assert(g_last_response != NULL);
   /* Reported verbatim... */
   assert(response_has_role("code"));
   assert(response_has_role("diagnose"));
   assert(response_has_role("all"));
   /* ...and NOT replaced by the default set. */
   assert(!response_has_role("summarize"));

   /* Nothing was persisted: re-reading still shows the operator's roles. */
   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(a1, "roles");
   assert(cJSON_GetArraySize(roles) == 3);
   printf("  PASS: agent roles with no csv reports and does not write\n");
}

/* An explicit csv still sets, and `--reset` still restores the default set. */
static void test_roles_csv_sets_and_reset_restores_defaults(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"]}]}\n");
   reset_capture();

   cJSON *req = args_request("a1", "review,validate,all");
   assert(handle_agent_roles(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0');
   assert(response_has_role("review") && response_has_role("validate") && response_has_role("all"));
   assert(!response_has_role("code"));

   reset_capture();
   req = args_request("a1", "--reset");
   assert(handle_agent_roles(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_last_error[0] == '\0');
   assert(response_has_role("summarize") && response_has_role("code"));
   assert(!response_has_role("review"));
   /* --reset is the default set, which does not include the wildcard. */
   assert(!response_has_role("all"));
   printf("  PASS: agent roles csv sets, --reset restores defaults\n");
}

static void test_personas_without_csv_reports_and_does_not_write(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"a1\",\"provider\":\"anthropic\",\"model\":\"m\","
                "\"roles\":[\"code\"],\"personas\":[\"engineer\",\"qa\"]}]}\n");
   reset_capture();

   cJSON *req = args_request("a1", NULL);
   assert(handle_agent_personas(NULL, NULL, req) == 0);
   cJSON_Delete(req);

   assert(g_last_error[0] == '\0');
   cJSON *personas = cJSON_GetObjectItemCaseSensitive(g_last_response, "personas");
   assert(cJSON_GetArraySize(personas) == 2);

   /* Not flattened to ["all"] on disk either. */
   reset_capture();
   assert(handle_agent_list(NULL, NULL, NULL) == 0);
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(g_last_response, "agents");
   cJSON *a1 = cJSON_GetArrayItem(agents, 0);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(a1, "personas")) == 2);
   printf("  PASS: agent personas with no csv reports and does not write\n");
}

int main(void)
{
   printf("agent_list_handler:\n");
   test_load_failure_is_an_error_not_empty();
   test_empty_config_is_ok_with_empty_array();
   test_populated_config_lists_agents();
   test_primary_only_round_trips();
   test_cli_probe_uses_backend_executor_and_config_discovery();
   test_cli_probe_failure_and_no_run_are_observable();
   test_unknown_backend_fails_closed();
   test_http_probe_preserves_discovery_and_plain_execution();
   test_list_exposes_catalog_identity_and_pricing();
   test_add_requires_explicit_review_role();
   test_set_empty_roles_stays_empty();
   test_set_declares_prices_and_limits();
   test_set_zero_capacity_is_not_a_declaration();
   test_set_rejects_unparseable_price();
   test_set_empty_price_withdraws_the_declaration();
   test_roles_without_csv_reports_and_does_not_write();
   test_roles_csv_sets_and_reset_restores_defaults();
   test_personas_without_csv_reports_and_does_not_write();
   printf("all agent_list_handler tests passed\n");
   return 0;
}
