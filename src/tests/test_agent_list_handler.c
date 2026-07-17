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

/* The real handler under test. Declared here to avoid dragging server.h. */
int handle_agent_list(void *ctx, void *conn, cJSON *req);

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

int main(void)
{
   printf("agent_list_handler:\n");
   test_load_failure_is_an_error_not_empty();
   test_empty_config_is_ok_with_empty_array();
   test_populated_config_lists_agents();
   test_primary_only_round_trips();
   printf("all agent_list_handler tests passed\n");
   return 0;
}
