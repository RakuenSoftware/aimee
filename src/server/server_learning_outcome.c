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

/* Recovery is a projection of the proof atomically retained in the terminal,
 * never a native claim, worktree reinspection or invented successful task. */
static void observe_verification_grades(const cJSON *record, const cJSON *caller,
                                        const cJSON *event)
{
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   const cJSON *proofs = cJSON_GetObjectItemCaseSensitive(payload, "verification_proofs");
   if (strcmp(text_field(payload, "status"), "completed") || !cJSON_IsArray(proofs) ||
       cJSON_GetArraySize(proofs) < 1 || cJSON_GetArraySize(proofs) > 8)
      return;
   const cJSON *proof;
   cJSON_ArrayForEach(proof, proofs)
   {
      cJSON *request = cJSON_CreateObject(), *context = cJSON_Duplicate(caller, 1);
      cJSON *reply = NULL;
      char *wire = NULL;
      int accepted = 0;
      if (!request || !context ||
          !cJSON_AddItemToObject(request, "record", cJSON_Duplicate(record, 1)) ||
          !cJSON_AddItemToObject(request, "proof", cJSON_Duplicate(proof, 1)) ||
          !cJSON_ReplaceItemInObjectCaseSensitive(
              context, "transport_identity",
              cJSON_CreateString("host:isolated-project-verification")))
         goto next;
      reply = aimee_module_command_call_context(AIMEE_LEARNING_EVENT_EXECUTION,
                                                AIMEE_LEARNING_STAGE_EXECUTION, "grade", request,
                                                context);
      const cJSON *grade = cJSON_GetObjectItemCaseSensitive(reply, "grade");
      if (strcmp(text_field(reply, "status"), "ok") || !cJSON_IsObject(grade))
         goto next;
      wire = cJSON_PrintUnformatted(grade);
      accepted = wire && db1_execution_test_grade_record(text_field(record, "actor"), wire) >= 0;
   next:
      if (!accepted)
         LOG_WARN(
             "learning",
             "canonical task %s/%s: configured-test grade unavailable; durable receipt retained",
             text_field(record, "session_id"), text_field(record, "turn_id"));
      free(wire);
      cJSON_Delete(reply);
      cJSON_Delete(context);
      cJSON_Delete(request);
   }
}
void server_learning_observe_terminal(const char *session, const char *turn, const char *sequence,
                                      const char *event_id, const char *actor, const cJSON *event)
{
   cJSON *record = cJSON_CreateObject(), *caller = cJSON_CreateObject();
   cJSON *result = NULL;
   char *wire = NULL;
   int accepted = 0, record_ready = 0;
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
   record_ready = 1;
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
   if (record_ready)
      observe_verification_grades(record, caller, event);
   if (!accepted)
      LOG_WARN("learning",
               "canonical task %s/%s: local outcome observation unavailable; source retained",
               session ? session : "", turn ? turn : "");
   free(wire);
   cJSON_Delete(result);
   cJSON_Delete(caller);
   cJSON_Delete(record);
}
