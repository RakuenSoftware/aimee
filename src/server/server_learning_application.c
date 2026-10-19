#include "server_learning_application.h"
#include "db1_client/procedure_events.h"
#include "module_commands.h"
#include <aimee/learning/module_api.h>
#include <stdlib.h>
#include <string.h>

static const char *string_field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}

cJSON *server_learning_application_local(const request_context_t *owner, const cJSON *event,
                                         const cJSON *exposure, const char *scope_kind,
                                         const char *scope_id)
{
   cJSON *context = NULL, *args = NULL, *admitted = NULL, *snapshot = NULL;
   cJSON *projected = NULL, *reply = NULL;
   char *procedure_json = NULL, *event_json = NULL, *exposure_json = NULL;
   const char *reason = "user_feedback_authority_required";
   if (!owner || owner->vault_transport <= ATTEST_TCP_BEARER ||
       owner->vault_transport > ATTEST_MTLS_CLIENT || owner->vault_transport == ATTEST_TLS_BEARER ||
       owner->model_execution || !owner->vault_principal[0] || !cJSON_IsObject(event) ||
       !cJSON_IsObject(exposure) || strcmp(string_field(event, "authority"), "user_feedback"))
      goto done;
   reason = "learning_allocation_failed";
   context = cJSON_CreateObject();
   args = cJSON_CreateObject();
   cJSON *candidate = cJSON_Duplicate(event, 1);
   if (!context || !args || !candidate)
   {
      cJSON_Delete(candidate);
      goto done;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(candidate, "actor");
   if (!cJSON_AddStringToObject(candidate, "actor", owner->vault_principal) ||
       !cJSON_AddBoolToObject(context, "authenticated", 1) ||
       !cJSON_AddBoolToObject(context, "user_authority", 1) ||
       !cJSON_AddStringToObject(context, "principal", owner->vault_principal) ||
       !cJSON_AddStringToObject(context, "transport_identity", "host:local-user-feedback") ||
       !cJSON_AddItemToObject(args, "event", candidate))
   {
      cJSON_Delete(candidate);
      goto done;
   }
   if (!cJSON_AddItemToObject(args, "exposure", cJSON_Duplicate(exposure, 1)))
      goto done;
   reason = "learning_admission_refused";
   admitted = aimee_module_command_call_context(
       AIMEE_LEARNING_EVENT_EXPERIENCE, AIMEE_LEARNING_STAGE_EXPERIENCE, "admit", args, context);
   if (!cJSON_IsObject(admitted))
      goto done;
   procedure_json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(admitted, "procedure"));
   event_json = cJSON_PrintUnformatted(admitted);
   exposure_json = cJSON_PrintUnformatted(exposure);
   if (!procedure_json || !event_json || !exposure_json)
      goto done;
   for (int attempt = 0; attempt < 4; attempt++)
   {
      reason = "local_learning_store_unavailable";
      char *raw = db1_procedure_event_load(owner->vault_principal, procedure_json);
      cJSON_Delete(snapshot);
      snapshot = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
      free(raw);
      cJSON *events = cJSON_GetObjectItemCaseSensitive(snapshot, "events");
      const char *revision = string_field(snapshot, "revision");
      if (!cJSON_IsArray(events) || strlen(revision) != 64 ||
          !cJSON_AddItemToArray(events, cJSON_Duplicate(admitted, 1)))
         goto done;
      cJSON_Delete(args);
      args = cJSON_CreateObject();
      if (!args || !cJSON_AddItemReferenceToObject(args, "events", events))
         goto done;
      reason = "learning_projection_refused";
      cJSON_Delete(projected);
      projected = aimee_module_command_call_context(AIMEE_LEARNING_EVENT_EXPERIENCE,
                                                    AIMEE_LEARNING_STAGE_EXPERIENCE, "project",
                                                    args, context);
      if (!cJSON_IsArray(projected))
         goto done;
      reason = "local_learning_write_refused";
      int rc = db1_procedure_event_append(owner->vault_principal, procedure_json, revision,
                                          event_json, exposure_json, scope_kind, scope_id);
      if (rc == 2)
         continue;
      if (rc < 0)
         goto done;
      reply = cJSON_CreateObject();
      if (!reply || !cJSON_AddStringToObject(reply, "status", "ok") ||
          !cJSON_AddStringToObject(reply, "store", "local") ||
          !cJSON_AddStringToObject(reply, "event_id", string_field(admitted, "event_id")) ||
          !cJSON_AddBoolToObject(reply, "duplicate", rc == 1) ||
          !cJSON_AddItemToObject(reply, "projection", projected))
      {
         cJSON_Delete(reply);
         reply = NULL;
         goto done;
      }
      projected = NULL;
      goto done;
   }
   reason = "local_learning_snapshot_changed";
done:
   if (!reply)
   {
      reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "status", "error");
      cJSON_AddStringToObject(reply, "reason", reason);
   }
   free(procedure_json);
   free(event_json);
   free(exposure_json);
   cJSON_Delete(context);
   cJSON_Delete(args);
   cJSON_Delete(admitted);
   cJSON_Delete(snapshot);
   cJSON_Delete(projected);
   return reply;
}
