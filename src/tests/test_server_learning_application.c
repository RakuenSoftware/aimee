#include "server_learning_application.h"
#include "db1_client/procedure_events.h"
#include "module_commands.h"
#include <aimee/learning/module_api.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int admissions, projections, loads, writes, retry_count, fail_admission, fail_projection;
static int fail_write, duplicate;
static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
cJSON *aimee_module_command_call_context(uint32_t event, uint32_t stage, const char *verb,
                                         const cJSON *args, const cJSON *context)
{
   assert(event == AIMEE_LEARNING_EVENT_EXPERIENCE && stage == AIMEE_LEARNING_STAGE_EXPERIENCE);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(context, "authenticated")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(context, "user_authority")));
   assert(!strcmp(field(context, "principal"), "alice"));
   assert(!strcmp(field(context, "transport_identity"), "host:local-user-feedback"));
   if (!strcmp(verb, "admit"))
   {
      admissions++;
      const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(args, "event");
      assert(!strcmp(field(candidate, "actor"), "alice"));
      assert(!strcmp(field(candidate, "authority"), "user_feedback"));
      assert(!strcmp(field(cJSON_GetObjectItemCaseSensitive(args, "exposure"), "producer"),
                     "local_host_ledger"));
      return fail_admission ? NULL : cJSON_Duplicate(candidate, 1);
   }
   assert(!strcmp(verb, "project"));
   projections++;
   const cJSON *events = cJSON_GetObjectItemCaseSensitive(args, "events");
   assert(cJSON_IsArray(events) && cJSON_GetArraySize(events) == 1);
   assert(!strcmp(field(cJSON_GetArrayItem(events, 0), "actor"), "alice"));
   return fail_projection ? NULL : cJSON_Parse("[{\"verified_failure\":1}]");
}
char *db1_procedure_event_load(const char *principal, const char *procedure)
{
   loads++;
   assert(!strcmp(principal, "alice"));
   assert(strstr(procedure, "procedure_id"));
   cJSON *snapshot = cJSON_CreateObject();
   char revision[65];
   memset(revision, 'a', 64);
   revision[64] = 0;
   cJSON_AddStringToObject(snapshot, "revision", revision);
   cJSON_AddArrayToObject(snapshot, "events");
   char *raw = cJSON_PrintUnformatted(snapshot);
   cJSON_Delete(snapshot);
   return raw;
}
int db1_procedure_event_append(const char *principal, const char *procedure, const char *revision,
                               const char *event, const char *exposure, const char *scope,
                               const char *id)
{
   writes++;
   assert(!strcmp(principal, "alice") && strstr(procedure, "procedure_id"));
   assert(strlen(revision) == 64);
   cJSON *candidate = cJSON_Parse(event);
   assert(!strcmp(field(candidate, "actor"), "alice"));
   assert(!strcmp(field(candidate, "state"), "verified_failure"));
   cJSON_Delete(candidate);
   assert(strstr(exposure, "local_host_ledger"));
   assert(!strcmp(scope, "global") && !id[0]);
   if (retry_count > 0)
   {
      retry_count--;
      return 2;
   }
   return fail_write ? -1 : duplicate ? 1 : 0;
}
static void reset(void)
{
   admissions = projections = loads = writes = retry_count = fail_admission = 0;
   fail_projection = fail_write = duplicate = 0;
}
int main(void)
{
   request_context_t owner = {.vault_transport = ATTEST_MTLS_CLIENT};
   strcpy(owner.vault_principal, "alice");
   cJSON *event = cJSON_Parse(
       "{\"event_id\":\"e1\",\"actor\":\"forged\","
       "\"authority\":\"user_feedback\",\"state\":\"verified_failure\","
       "\"procedure\":{\"owner_id\":\"local\",\"procedure_id\":\"p1\",\"revision\":\"r1\"}}");
   cJSON *exposure = cJSON_Parse("{\"producer\":\"local_host_ledger\"}");
   assert(event && exposure);
   cJSON *reply;
   /* Each gate refuses before a Learning/DB1 call; payload cannot attest a user. */
   for (int gate = 0; gate < 6; gate++)
   {
      reset();
      request_context_t rejected = owner;
      if (gate == 0)
         rejected.model_execution = 1;
      if (gate == 1)
         rejected.vault_transport = ATTEST_TCP_BEARER;
      if (gate == 2)
         rejected.vault_principal[0] = 0;
      if (gate == 3)
         rejected.vault_transport = ATTEST_TLS_BEARER;
      if (gate == 4)
         rejected.vault_transport = (attested_transport_t)(ATTEST_MTLS_CLIENT + 1);
      reply = server_learning_application_local(gate == 5 ? NULL : &rejected, event, exposure,
                                                "global", "");
      assert(!strcmp(field(reply, "reason"), "user_feedback_authority_required"));
      assert(!admissions && !loads && !writes);
      cJSON_Delete(reply);
   }
   for (int scenario = 0; scenario < 6; scenario++)
   {
      reset();
      duplicate = scenario == 1;
      retry_count = scenario == 2 ? 2 : scenario == 3 ? 9 : 0;
      fail_projection = scenario == 4;
      fail_write = scenario == 5;
      reply = server_learning_application_local(&owner, event, exposure, "global", "");
      assert(admissions == 1);
      if (scenario < 3)
      {
         assert(!strcmp(field(reply, "status"), "ok"));
         assert(!strcmp(field(reply, "store"), "local"));
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "duplicate")) == duplicate);
         assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(reply, "projection")));
      }
      else
      {
         assert(!strcmp(field(reply, "status"), "error"));
         assert(!cJSON_GetObjectItemCaseSensitive(reply, "projection"));
      }
      if (scenario == 2)
         assert(writes == 3 && projections == 3 && loads == 3);
      if (scenario == 3)
         assert(writes == 4 && loads == 4);
      if (scenario == 4)
         assert(!writes);
      assert(!strcmp(field(event, "actor"), "forged"));
      cJSON_Delete(reply);
   }
   reset();
   fail_admission = 1;
   reply = server_learning_application_local(&owner, event, exposure, "global", "");
   assert(!strcmp(field(reply, "reason"), "learning_admission_refused"));
   assert(!loads && !writes);
   cJSON_Delete(reply);
   cJSON_Delete(event);
   cJSON_Delete(exposure);
   puts(
       "local learning: host feedback authority, source ownership, CAS retries and refusal passed");
   return 0;
}
