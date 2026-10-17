/* Exercise the production primary owner across model switches and restart. */
#include "primary_session_adapter.h"
#include "agent_exec.h"
#include "headers/context_engine.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *stored, *canonical_stored;
static int fail_checkpoint;
static const char *owner = "alice";
const char *agent_get_request_vault_principal(void)
{
   return "alice";
}
int db1_server_session_get(const char *id, db1_server_session_t *row)
{
   assert(!strcmp(id, "stable"));
   memset(row, 0, sizeof *row);
   snprintf(row->principal, sizeof row->principal, "%s", owner);
   return 0;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   (void)id;
   (void)client;
   (void)principal;
   assert(0);
   return -1;
}
int db1_session_ir_history_load(const char *id, const char *scope, const char *actor, char **out)
{
   assert(!strcmp(id, "stable") && !strcmp(scope, "primary") && !strcmp(actor, "alice"));
   *out = canonical_stored ? strdup(canonical_stored) : NULL;
   return *out ? 0 : 1;
}
int db1_session_ir_history_save(const char *id, const char *checkpoint, const char *scope,
                                const char *actor, const char *json)
{
   assert(!strcmp(id, "stable") && strlen(checkpoint) == 32 && !strcmp(scope, "primary") &&
          !strcmp(actor, "alice"));
   if (fail_checkpoint)
      return -1;
   free(canonical_stored);
   canonical_stored = strdup(json);
   return 0;
}
static char stored_provider[64];
static int expected_count, executions;

int db1_primary_session_get_latest(const char *id, db1_primary_session_row_t *row)
{
   assert(!strcmp(id, "stable"));
   memset(row, 0, sizeof(*row));
   if (!stored)
      return 1;
   row->messages_json = strdup(stored);
   snprintf(row->provider, sizeof(row->provider), "%s", stored_provider);
   return 0;
}
void db1_primary_session_row_clear(db1_primary_session_row_t *row)
{
   free(row->messages_json);
}
char *db1_primary_session_load(const char *id, const char *agent, const char *provider)
{
   (void)id;
   (void)agent;
   (void)provider;
   return NULL;
}
const context_engine_t *context_engine_get_active(void)
{
   return NULL;
}
const context_engine_t *context_engine_get_default(void)
{
   return NULL;
}
int session_compact(cJSON *messages, const session_compact_config_t *cfg,
                    session_compact_result_t *result)
{
   (void)messages;
   (void)cfg;
   (void)result;
   return -1;
}
int model_context_window(const char *model)
{
   (void)model;
   return 0;
}
void ingress_preinject_set_session_id(const char *id)
{
   (void)id;
}
void ingress_preinject_set_task_requirements(const cJSON *req)
{
   (void)req;
}
void session_id_set_override(const char *id)
{
   (void)id;
}
void session_id_clear_override(void)
{
}
int session_compact_estimate_tokens(cJSON *messages)
{
   (void)messages;
   return 0;
}
int session_compact_pressure(int a, int b, int c, const session_compact_config_t *cfg)
{
   (void)a;
   (void)b;
   (void)c;
   (void)cfg;
   return SESSION_PRESSURE_OK;
}
int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system, const char *prompt, int max_tokens,
                                     double temperature, cJSON *initial, cJSON **updated,
                                     agent_result_t *out)
{
   (void)agent;
   (void)network;
   (void)system;
   (void)prompt;
   (void)max_tokens;
   (void)temperature;
   (void)out;
   executions++;
   agent_t other = {0};
   strcpy(other.name, "mistral");
   strcpy(other.provider, "mistral");
   primary_session_request_t busy = {
       .agent = &other, .aimee_session_id = "stable", .user_prompt = "overlap"};
   agent_result_t overlap;
   assert(primary_session_adapter_turn(&busy, &overlap, NULL, 0) != 0);

   assert(cJSON_GetArraySize(initial) == expected_count);
   cJSON *message;
   cJSON_ArrayForEach(message, initial)
   {
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(message, "blocks")));
      assert(!cJSON_GetObjectItemCaseSensitive(message, "content"));
   }
   *updated = initial ? cJSON_Duplicate(initial, 1) : cJSON_CreateArray();
   cJSON_AddItemToArray(
       *updated,
       cJSON_Parse("{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\",\"text\":\"reply\"}]}"));
   return 0;
}
static void turn(agent_t *agent, int count, int ok)
{
   expected_count = count;
   primary_session_request_t req = {
       .agent = agent, .aimee_session_id = "stable", .user_prompt = "continue"};
   agent_result_t out;
   int before = executions;
   assert((primary_session_adapter_turn(&req, &out, NULL, 0) == 0) == ok);
   assert(executions == before + ok);
}
int main(void)
{
   agent_t a = {0}, b = {0};
   strcpy(a.name, "codex");
   strcpy(a.provider, "chatgpt");
   strcpy(b.name, "mistral");
   strcpy(b.provider, "mistral");
   turn(&a, 0, 1);
   turn(&b, 1, 1);
   turn(&a, 2, 1); /* Returning to A must include B, not A's stale cache. */
   primary_session_adapter_reset();
   turn(&b, 3, 1); /* Restart reload is canonical even for a different provider. */
   free(canonical_stored);
   canonical_stored = strdup("[{\"role\":\"user\",\"blocks\":[{\"type\":\"bogus\"}]}]");
   turn(&a, 0, 0); /* A broken durable transcript cannot start an empty turn. */
   primary_session_adapter_reset();
   free(stored);
   free(canonical_stored);
   canonical_stored = NULL;
   stored = strdup("[{\"role\":\"user\",\"content\":\"legacy\"}]");
   strcpy(stored_provider, "anthropic");
   turn(&a, 1, 1); /* Import at load uses the stored provider, not the selected one. */
   primary_session_adapter_reset();
   owner = "mallory";
   turn(&a, 0, 0);
   owner = "alice";
   fail_checkpoint = 1;
   primary_session_request_t req = {
       .agent = &a, .aimee_session_id = "stable", .user_prompt = "continue"};
   agent_result_t out;
   expected_count = 2;
   int before = executions;
   assert(primary_session_adapter_turn(&req, &out, NULL, 0) == AGENT_RC_CAPTURE_FAILED);
   assert(executions == before + 1);
   fail_checkpoint = 0;
   turn(&a, 0, 0); /* A failed checkpoint cannot replay a turn after tools ran. */
   primary_session_adapter_reset();
   free(stored);
   free(canonical_stored);
   puts("primary canonical history: switch, switch back, reload, legacy import and refusal passed");
   return 0;
}
