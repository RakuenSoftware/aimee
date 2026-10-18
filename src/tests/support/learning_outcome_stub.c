#include "cJSON.h"
#include "db1_client/interaction_events.h"
#include <aimee/learning/module_api.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

void test_learning_assert_source(const cJSON *record);
static atomic_int observations;
void test_learning_observations_reset(void)
{
   atomic_store(&observations, 0);
}
int test_learning_observations(void)
{
   return atomic_load(&observations);
}

cJSON *aimee_module_command_call_context(uint32_t kind, uint32_t stage, const char *verb,
                                         const cJSON *record, const cJSON *caller)
{
   assert(kind == AIMEE_LEARNING_EVENT_EXECUTION && stage == AIMEE_LEARNING_STAGE_EXECUTION);
   assert(!strcmp(verb, "observe"));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caller, "authenticated")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(caller, "principal")),
                  cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "actor"))));
   test_learning_assert_source(record);
   cJSON *source = cJSON_Duplicate(record, 1);
   cJSON_DeleteItemFromObjectCaseSensitive(source, "event");
   cJSON_DeleteItemFromObjectCaseSensitive(source, "version");
   cJSON_AddStringToObject(source, "kind", "canonical_session_log");
   cJSON *observation = cJSON_CreateObject();
   cJSON_AddItemToObject(observation, "source", source);
   cJSON_AddStringToObject(observation, "verification", "ungraded");
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "status", "ok");
   cJSON_AddItemToObject(result, "observation", observation);
   return result;
}

int db1_interaction_event_record(const char *session, const char *type, const char *actor,
                                 const char *payload, const char *outcome)
{
   assert(!strcmp(type, "task_outcome") && !strcmp(actor, "system") &&
          !strcmp(outcome, "observed"));
   cJSON *observation = cJSON_Parse(payload);
   assert(observation);
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(observation, "source");
   assert(!strcmp(session,
                  cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "session_id"))));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observation, "verification")),
               "ungraded"));
   cJSON_Delete(observation);
   atomic_fetch_add(&observations, 1);
   return 42;
}
