/* Actual worker/production publisher contract. PG/source policy is tested in Go;
 * this fixture never pretends its module stubs independently verified a task. */
#include "server_learning_outcome.h"
#include "db1_client/interaction_events.h"
#include <aimee/learning/module_api.h>
#include "module_commands.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int failed_write = 1, retries, delivered, observations, claims;
/* Set: the claim reply says nothing is pending, as a current module does. */
static atomic_int nothing_pending;
/* Set: the store refuses the next delivered ack, as it does while a grade the
 * delivery depends on has not landed yet. */
static atomic_int refuse_delivered, refused, released;
static const char *record_wire =
    "{\"version\":1,\"session_id\":\"s\",\"turn_id\":\"turn\",\"sequence\":\"9\",\"event_id\":"
    "\"terminal\",\"actor\":\"alice\",\"event\":{\"kind\":\"execution\",\"payload\":{\"status\":"
    "\"completed\"}}}";
static const char *lease = "0123456789abcdef0123456789abcdef";

char *db1_learning_projection_claim(void)
{
   atomic_fetch_add(&claims, 1);
   cJSON *batch = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(batch, "items");
   cJSON_AddStringToObject(batch, "cutoff", "fixture-cutover");
   if (atomic_load(&nothing_pending))
      cJSON_AddNumberToObject(batch, "next_due_ms", -1);
   if (!atomic_load(&delivered))
   {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "lease", lease);
      cJSON_AddItemToObject(item, "record", cJSON_Parse(record_wire));
      cJSON_AddItemToArray(items, item);
   }
   char *wire = cJSON_PrintUnformatted(batch);
   cJSON_Delete(batch);
   return wire;
}
int db1_learning_projection_ack(const char *token, const char *result)
{
   assert(!strcmp(token, lease));
   if (atomic_load(&refuse_delivered))
   {
      if (!strcmp(result, "delivered"))
      {
         atomic_fetch_add(&refused, 1);
         return -1;
      }
      assert(!strcmp(result, "retry") && atomic_load(&refused) == 1);
      atomic_fetch_add(&released, 1);
      atomic_store(&refuse_delivered, 0);
      return 0;
   }
   if (!strcmp(result, "retry"))
   {
      assert(!atomic_load(&observations));
      atomic_fetch_add(&retries, 1);
      atomic_store(&failed_write, 0);
   }
   else
   {
      assert(!strcmp(result, "delivered") && atomic_load(&observations) == 1);
      atomic_fetch_add(&delivered, 1);
   }
   return 0;
}
cJSON *aimee_module_command_call_context(uint32_t kind, uint32_t stage, const char *verb,
                                         const cJSON *record, const cJSON *caller)
{
   assert(kind == AIMEE_LEARNING_EVENT_EXECUTION && stage == AIMEE_LEARNING_STAGE_EXECUTION);
   assert(!strcmp(verb, "observe"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(caller, "principal")),
                  "alice"));
   cJSON *source = cJSON_Duplicate(record, 1);
   cJSON_DeleteItemFromObjectCaseSensitive(source, "version");
   cJSON_DeleteItemFromObjectCaseSensitive(source, "event");
   cJSON_AddStringToObject(source, "kind", "canonical_session_log");
   cJSON *result = cJSON_CreateObject(),
         *observation = cJSON_AddObjectToObject(result, "observation");
   cJSON_AddStringToObject(result, "status", "ok");
   cJSON_AddItemToObject(observation, "source", source);
   cJSON_AddStringToObject(observation, "verification", "ungraded");
   return result;
}
int db1_interaction_event_record(const char *session, const char *type, const char *actor,
                                 const char *payload, const char *outcome)
{
   (void)payload;
   assert(!strcmp(session, "s") && !strcmp(type, "task_outcome") && !strcmp(actor, "system") &&
          !strcmp(outcome, "observed"));
   if (atomic_load(&failed_write))
      return -1;
   atomic_store(&observations, 1);
   return 1;
}
int db1_execution_test_grade_record(const char *actor, const char *grade)
{
   (void)actor;
   (void)grade;
   assert(!"ordinary observation must never acquire a fabricated grade");
   return -1;
}
char *db1_delegate_execution_terminal_lookup(const char *actor, const char *source)
{
   (void)actor;
   (void)source;
   assert(!"unexpected delegate source");
   return NULL;
}
int db1_delegate_execution_experience_record(const char *actor, const char *source)
{
   (void)actor;
   (void)source;
   assert(!"unexpected delegate source");
   return -1;
}
static void settle(int tenths)
{
   for (int n = 0; n < tenths; n++)
   {
      struct timespec pause = {.tv_nsec = 100000000L};
      nanosleep(&pause, NULL);
   }
}
static int cancelled_delivery(void)
{
   return 1;
}
int main(void)
{
   cJSON *cancelled = cJSON_Parse(record_wire);
   assert(cancelled && !server_learning_deliver_record(cancelled, cancelled_delivery));
   cJSON_Delete(cancelled);
   assert(!atomic_load(&observations));
   server_learning_recovery_init();
   server_learning_recovery_init(); /* One lifecycle owner, not two publishers. */
   for (int n = 0; n < 80 && !atomic_load(&delivered); n++)
   {
      struct timespec pause = {.tv_nsec = 100000000L};
      nanosleep(&pause, NULL);
   }
   server_learning_recovery_shutdown();
   assert(atomic_load(&retries) == 1 && atomic_load(&delivered) == 1);
   assert(atomic_load(&claims) >= 2 && atomic_load(&observations) == 1);
   server_learning_recovery_init();
   server_learning_recovery_shutdown();
   assert(atomic_load(&delivered) == 1);

   /* An empty queue with nothing due: the worker claims once, then sleeps until
    * a queued projection is announced, rather than every few seconds. */
   atomic_store(&nothing_pending, 1);
   int before = atomic_load(&claims);
   server_learning_recovery_init();
   for (int n = 0; n < 50 && atomic_load(&claims) == before; n++)
      settle(1);
   int first = atomic_load(&claims);
   assert(first == before + 1);
   settle(35); /* past the old 3s cadence */
   assert(atomic_load(&claims) == first);
   server_learning_recovery_wake();
   for (int n = 0; n < 50 && atomic_load(&claims) == first; n++)
      settle(1);
   assert(atomic_load(&claims) == first + 1);
   server_learning_recovery_shutdown();

   /* A refused delivered ack hands the lease back at once with a retry, so the
    * record is redelivered after the store's backoff rather than held for the
    * whole lease. */
   atomic_store(&nothing_pending, 0);
   atomic_store(&delivered, 0);
   atomic_store(&refuse_delivered, 1);
   server_learning_recovery_init();
   for (int n = 0; n < 50 && !atomic_load(&delivered); n++)
      settle(1);
   server_learning_recovery_shutdown();
   assert(atomic_load(&refused) == 1 && atomic_load(&released) == 1);
   assert(atomic_load(&delivered) == 1);
   puts("learning recovery: actual timer retries failed publication and acknowledges only explicit "
        "delivery; no browser, model or tool execution");
   return 0;
}

cJSON *aimee_module_command_call_context_timeout(uint32_t kind, uint32_t stage, const char *verb,
                                                 const cJSON *record, const cJSON *caller,
                                                 int timeout_ms)
{
   assert(timeout_ms == 1000);
   return aimee_module_command_call_context(kind, stage, verb, record, caller);
}
