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
#include "agent_admission.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "provider_catalog.h"

/* --- capture layer: stub the transport so we can read what the handler sent --- */

static cJSON *g_last_response = NULL;
static char g_last_error[256];

/* server_send_ok is a static inline in server.h that calls this and then frees
 * resp itself, so this stub must NOT delete resp — it only snapshots it. Same
 * contract the real transport and every other handler test rely on. */
int server_send_response(void *conn, cJSON *resp)
{
   (void)conn;
   if (g_last_response)
      cJSON_Delete(g_last_response);
   g_last_response = cJSON_Duplicate(resp, 1);
   return 0;
}

int server_send_error(void *conn, const char *message, const char *request_id)
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
int server_send_error_kind(void *conn, const char *kind, const char *message,
                           const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error_kind, sizeof(g_last_error_kind), "%s", kind ? kind : "");
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

/* The real handler under test. Declared here to avoid dragging server.h. */
int handle_agent_list(void *ctx, void *conn, cJSON *req);
int handle_agent_probe(void *ctx, void *conn, cJSON *req);
int server_agent_route_is_down(const char *agent_name);
int server_agent_route_has_capacity(const agent_t *ag);

/* Probe transport seams. This target intentionally links no production agent
 * transport: the handler's observable response and backend selection are under
 * test, while these stubs record the exact executor contract it chose. */
static int g_cli_calls, g_http_calls, g_execute_failure, g_models_status = 200;
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
   {
      if (g_models_status < 200 || g_models_status >= 400)
      {
         *response_buf = NULL;
         return g_models_status;
      }
      *response_buf = strdup("{\"data\":[{\"id\":\"http-model\"}]}");
   }
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
   g_models_status = 200;
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

static void test_aimee_synth_route_tracks_real_endpoint_probe(void)
{
   set_home_empty();
   write_agents("{\"agents\":[{\"name\":\"local-gemma4\",\"provider\":\"openai\","
                "\"endpoint\":\"http://aimee-llm:8742/v1\",\"model\":\"aimee-synth\","
                "\"auth_type\":\"none\",\"roles\":[\"review\"]}]}\n");
   reset_capture();

   agent_config_t cfg;
   assert(agent_load_config(&cfg) == 0 && cfg.agent_count == 1);
   provider_catalog_init(cfg.agents, cfg.agent_count);
   agent_set_route_health_filter(server_agent_route_is_down);

   /* Exercise the production route filter through its real /models probe. A
    * refused connection excludes the backend immediately and also feeds the
    * existing catalog's failure state; repeated routing never assigns it. */
   g_models_status = -1;
   for (int i = 0; i < 3; i++)
      assert(agent_route(&cfg, "review") == NULL);
   assert(provider_catalog_get_health("local-gemma4") == CATALOG_HEALTH_DOWN);

   /* Restoring the same endpoint records recovery through that same path and
    * makes the agent routable again without a parallel health authority. */
   g_models_status = 200;
   assert(agent_route(&cfg, "review") == &cfg.agents[0]);
   assert(provider_catalog_get_health("local-gemma4") == CATALOG_HEALTH_HEALTHY);

   /* Now saturate the actual admission controller and route through the
    * server's production capacity callback. The atomic refusal remains a load
    * outcome: it cannot mutate provider health, and release makes the same
    * agent immediately routable again. */
   cfg.agents[0].max_parallel = 1;
   agent_admission_configure(4, 4, NULL, 0);
   agent_admit_req_t held_req = {
       .ctx_handle = "held-panel-seat",
       .agent = cfg.agents[0].name,
       .model = cfg.agents[0].model,
       .per_agent_max = cfg.agents[0].max_parallel,
       .flags = AGENT_ADMIT_NONBLOCKING,
   };
   agent_admit_status_t held_status = AGENT_ADMIT_INVALID;
   agent_slot_t *held = agent_admission_acquire(&held_req, &held_status);
   assert(held && held_status == AGENT_ADMIT_OK);
   agent_set_route_capacity_probe(server_agent_route_has_capacity);
   assert(agent_route(&cfg, "review") == NULL);
   agent_admit_req_t raced_req = held_req;
   raced_req.ctx_handle = "raced-panel-seat";
   assert(agent_admission_acquire(&raced_req, &held_status) == NULL &&
          held_status == AGENT_ADMIT_AT_LIMIT);
   assert(provider_catalog_get_health("local-gemma4") == CATALOG_HEALTH_HEALTHY);
   agent_admission_release(held);
   assert(agent_route(&cfg, "review") == &cfg.agents[0]);
   agent_set_route_capacity_probe(NULL);
   agent_set_route_health_filter(NULL);
   printf("  PASS: aimee-synth routing follows production endpoint failure and recovery\n");
}

/* primary_only survives parse -> agent_load_config -> server_agent_to_json (the
 * /v1/agent/list surface the Web GUI reads to render the checkbox). An agent that
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

int main(void)
{
   printf("agent_list_handler:\n");
   test_load_failure_is_an_error_not_empty();
   test_empty_config_is_ok_with_empty_array();
   test_populated_config_lists_agents();
   test_aimee_synth_route_tracks_real_endpoint_probe();
   test_primary_only_round_trips();
   test_cli_probe_uses_backend_executor_and_config_discovery();
   test_cli_probe_failure_and_no_run_are_observable();
   test_unknown_backend_fails_closed();
   test_http_probe_preserves_discovery_and_plain_execution();
   test_list_exposes_catalog_identity_and_pricing();
   printf("all agent_list_handler tests passed\n");
   return 0;
}
