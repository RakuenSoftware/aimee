#include "server_learning_outcome.h"
#include "module_commands.h"
#include "db1_client/interaction_events.h"
#include <aimee/learning/module_api.h>
#include "log.h"
#include <stdlib.h>
#include <string.h>

static const char *text_field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}

void server_learning_observe_terminal(const char *session, const char *turn, const char *sequence,
                                      const char *event_id, const char *actor, const cJSON *event)
{
   cJSON *record = cJSON_CreateObject(), *caller = cJSON_CreateObject();
   cJSON *result = NULL;
   char *wire = NULL;
   int accepted = 0;
   if (!record || !caller || !session || !turn || !sequence || !event_id || !actor || !actor[0] ||
       !cJSON_AddNumberToObject(record, "version", 1) ||
       !cJSON_AddStringToObject(record, "session_id", session) ||
       !cJSON_AddStringToObject(record, "turn_id", turn) ||
       !cJSON_AddStringToObject(record, "sequence", sequence) ||
       !cJSON_AddStringToObject(record, "event_id", event_id) ||
       !cJSON_AddStringToObject(record, "actor", actor) ||
       !cJSON_AddItemToObject(record, "event", cJSON_Duplicate(event, 1)) ||
       !cJSON_AddBoolToObject(caller, "authenticated", 1) ||
       !cJSON_AddStringToObject(caller, "principal", actor) ||
       !cJSON_AddStringToObject(caller, "transport_identity", "host:canonical-execution"))
      goto done;
   result = aimee_module_command_call_context(
       AIMEE_LEARNING_EVENT_EXECUTION, AIMEE_LEARNING_STAGE_EXECUTION, "observe", record, caller);
   const cJSON *observation = cJSON_GetObjectItemCaseSensitive(result, "observation");
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(observation, "source");
   if (strcmp(text_field(result, "status"), "ok") ||
       strcmp(text_field(source, "kind"), "canonical_session_log") ||
       strcmp(text_field(source, "session_id"), session) ||
       strcmp(text_field(source, "turn_id"), turn) ||
       strcmp(text_field(source, "sequence"), sequence) ||
       strcmp(text_field(source, "event_id"), event_id) ||
       strcmp(text_field(source, "actor"), actor) ||
       strcmp(text_field(observation, "verification"), "ungraded"))
      goto done;
   wire = cJSON_PrintUnformatted(observation);
   accepted = wire && db1_interaction_event_record(session, ie_event_type_name(IE_TASK_OUTCOME),
                                                   "system", wire, "observed") >= 0;
done:
   if (!accepted)
      LOG_WARN("learning",
               "canonical task %s/%s: local outcome observation unavailable; source retained",
               session ? session : "", turn ? turn : "");
   free(wire);
   cJSON_Delete(result);
   cJSON_Delete(caller);
   cJSON_Delete(record);
}
