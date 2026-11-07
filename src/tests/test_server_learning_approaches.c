#include "server_learning_approaches.h"
#include "modules/workspace/workspace_turn.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reads, begins, ends, reject_scope, reject_resource, reject_project, lost_source,
    no_experience;
static const char *snapshot_principal = "alice", *snapshot_project = "project-a";
static int scope_token, foreign_source, foreign_parent, foreign_proof;
static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
int workspace_turn_scope_begin(const char *cwd, workspace_turn_scope_t **out)
{
   begins++;
   assert(!strcmp(cwd, "/client/registered-project"));
   *out = (workspace_turn_scope_t *)&scope_token;
   return reject_scope ? -1 : 1;
}
int workspace_turn_scope_end(workspace_turn_scope_t *scope)
{
   assert(!scope || scope == (workspace_turn_scope_t *)&scope_token);
   if (scope)
      ends++;
   return 0;
}
const char *workspace_turn_active_cwd(void)
{
   return "/held/approved-project";
}
int workspace_turn_resource_identity(const char *cwd, int *fd, char digest[65])
{
   assert(!strcmp(cwd, "/held/approved-project"));
   *fd = open("/dev/null", O_RDONLY);
   assert(*fd >= 0);
   memset(digest, 'a', 64);
   digest[64] = '\0';
   return reject_resource ? -1 : 0;
}
int server_learning_project_from_cwd(const char *cwd, char *out, size_t capacity)
{
   assert(!strcmp(cwd, "/held/approved-project"));
   snprintf(out, capacity, "project-a");
   return reject_project ? -1 : 0;
}
char *db1_execution_test_grade_recall(const char *principal, const char *project,
                                      const char *sources)
{
   assert(!strcmp(principal, "alice"));
   assert(!strcmp(project, "project-a"));
   assert((reads == 0) == (sources == NULL));
   reads++;
   cJSON *snapshot = cJSON_Parse(
       "{\"schema_version\":1,\"principal\":\"alice\",\"scope_kind\":\"project\","
       "\"scope_id\":\"project-a\",\"coverage\":\"recent_bounded\",\"items\":[{"
       "\"qualification\":\"configured_tests_only\",\"verification\":\"tests_bound_revision\","
       "\"verdict\":\"passed\",\"verification_ref\":\"sha256:fixture\",\"proof\":{"
       "\"snapshot_commit\":\"whole-revision\",\"configuration_sha256\":\"whole-config\"},"
       "\"delegate_experience\":{\"principal\":\"alice\",\"scope_kind\":\"project\","
       "\"scope_id\":\"project-a\",\"qualification\":\"configured_tests_only\","
       "\"verdict\":\"passed\",\"action_coverage\":\"bounded_canonical_tool_calls\","
       "\"actions\":[{\"name\":\"git_verify\",\"input_sha256\":\"bound-input\"}]}}]}");
   cJSON_ReplaceItemInObject(snapshot, "principal", cJSON_CreateString(snapshot_principal));
   cJSON_ReplaceItemInObject(snapshot, "scope_id", cJSON_CreateString(snapshot_project));
   cJSON *grade = cJSON_GetArrayItem(cJSON_GetObjectItem(snapshot, "items"), 0);
   cJSON *source = cJSON_CreateObject();
   cJSON_AddStringToObject(source, "actor", foreign_source ? "foreign" : "alice");
   cJSON_AddStringToObject(source, "event_id", "actual-terminal");
   cJSON_AddItemToObject(grade, "source", source);
   cJSON *proof = cJSON_GetObjectItem(grade, "proof");
   cJSON_AddStringToObject(proof, "scope_kind", "project");
   cJSON_AddStringToObject(proof, "scope_id", foreign_proof ? "foreign-project" : "project-a");
   cJSON *experience = cJSON_GetObjectItem(grade, "delegate_experience");
   cJSON_AddItemToObject(experience, "source", cJSON_Duplicate(source, 1));
   cJSON_AddItemToObject(experience, "parent_source", cJSON_Duplicate(source, 1));
   cJSON_AddStringToObject(experience, "verification_ref", "sha256:fixture");
   if (foreign_parent)
      cJSON_ReplaceItemInObject(cJSON_GetObjectItem(experience, "parent_source"), "actor",
                                cJSON_CreateString("foreign"));
   if (no_experience || (lost_source && sources))
      cJSON_DeleteItemFromObject(cJSON_GetArrayItem(cJSON_GetObjectItem(snapshot, "items"), 0),
                                 "delegate_experience");
   char *raw = cJSON_PrintUnformatted(snapshot);
   cJSON_Delete(snapshot);
   return raw;
}
int main(void)
{
   request_context_t owner = {.vault_transport = ATTEST_MTLS_CLIENT};
   strcpy(owner.vault_principal, "alice");
   strcpy(owner.principal, "forged-display-name");
   const char *cwd = "/client/registered-project";
   cJSON *reply = server_learning_approaches_local(&owner, cwd, "new user goal");
   assert(!strcmp(field(reply, "status"), "ok"));
   assert(!strcmp(field(reply, "goal_match"), "not_inferred"));
   assert(!strcmp(field(reply, "scope_id"), "project-a"));
   cJSON *approach = cJSON_GetArrayItem(cJSON_GetObjectItem(reply, "approaches"), 0);
   assert(!strcmp(field(approach, "qualification"), "configured_tests_only"));
   assert(!cJSON_HasObjectItem(approach, "similarity"));
   assert(!cJSON_HasObjectItem(approach, "failure_mode"));
   assert(!strcmp(field(cJSON_GetObjectItem(cJSON_GetObjectItem(approach, "receipt"), "proof"),
                        "snapshot_commit"),
                  "whole-revision"));
   assert(reads == 2 && begins == 1 && ends == 1);
   cJSON_Delete(reply);
   reads = 0;
   no_experience = 1;
   reply = server_learning_approaches_local(&owner, cwd, "same goal");
   assert(!strcmp(field(reply, "status"), "ok"));
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply, "approaches")) == 0);
   cJSON_Delete(reply);
   no_experience = 0;
   for (int test = 0; test < 11; test++)
   {
      request_context_t candidate = owner;
      reads = 0;
      reject_scope = test == 2;
      reject_resource = test == 3;
      reject_project = test == 4;
      snapshot_principal = test == 5 ? "foreign" : "alice";
      snapshot_project = test == 6 ? "foreign-project" : "project-a";
      lost_source = test == 7;
      foreign_source = test == 8;
      foreign_parent = test == 9;
      foreign_proof = test == 10;
      if (test == 0)
         candidate.vault_transport = ATTEST_NONE;
      if (test == 1)
         candidate.vault_principal[0] = '\0';
      reply = server_learning_approaches_local(&candidate, cwd, "same goal");
      assert(!strcmp(field(reply, "status"), "error"));
      assert(!cJSON_HasObjectItem(reply, "approaches"));
      assert(reads == (test < 5 ? 0 : test < 7 ? 1 : 2));
      if (lost_source)
         assert(!strcmp(field(reply, "kind"), "stale_context"));
      cJSON_Delete(reply);
   }
   puts("scoped approaches: host actor/project, whole receipts, erasure fence and legacy exclusion "
        "pass");
   return 0;
}
