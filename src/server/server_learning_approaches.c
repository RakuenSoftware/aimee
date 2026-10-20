#include "server_learning_approaches.h"
#include "db1_client/interaction_events.h"
#include "modules/workspace/workspace_turn.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int server_learning_project_from_cwd(const char *cwd, char *out, size_t capacity);
static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
static cJSON *failure(const char *kind)
{
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "error");
   cJSON_AddStringToObject(reply, "kind", kind);
   cJSON_AddStringToObject(reply, "message", kind);
   return reply;
}
static int scoped(const cJSON *snapshot, const char *principal, const char *project)
{
   cJSON *version = cJSON_GetObjectItemCaseSensitive(snapshot, "schema_version");
   cJSON *items = cJSON_GetObjectItemCaseSensitive(snapshot, "items");
   return cJSON_IsNumber(version) && version->valuedouble == 1 &&
          !strcmp(field(snapshot, "principal"), principal) &&
          !strcmp(field(snapshot, "scope_kind"), "project") &&
          !strcmp(field(snapshot, "scope_id"), project) &&
          !strcmp(field(snapshot, "coverage"), "recent_bounded") && cJSON_IsArray(items) &&
          cJSON_GetArraySize(items) <= 8;
}
cJSON *server_learning_approaches_local(const request_context_t *owner, const char *cwd,
                                        const char *goal)
{
   const char *reason = "authenticated_owner_required";
   workspace_turn_scope_t *scope = NULL;
   int fd = -1;
   cJSON *snapshot = NULL, *fresh = NULL, *reply = NULL;
   char *raw = NULL, *checked = NULL;
   char identity[65], project[257] = "";
   if (!owner || owner->vault_transport <= ATTEST_NONE ||
       owner->vault_transport > ATTEST_MTLS_CLIENT || !owner->vault_principal[0])
      goto done;
   reason = "scope_required";
   if (!cwd || !cwd[0] || workspace_turn_scope_begin(cwd, &scope) < 0)
      goto done;
   const char *held = workspace_turn_active_cwd();
   if (!held || !held[0])
      held = cwd;
   if (workspace_turn_resource_identity(held, &fd, identity) ||
       server_learning_project_from_cwd(held, project, sizeof project) || !project[0])
      goto done;
   reason = "learning_history_unavailable";
   raw = db1_execution_test_grade_recall(owner->vault_principal, project, NULL);
   snapshot = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   if (!scoped(snapshot, owner->vault_principal, project))
      goto done;
   /* Same whole source receipts and targeted live-source check as canonical
    * model context. An erased parent/job/action must not survive a public read.
    * This read-only API never grants provider-send authority to its caller. */
   checked = db1_execution_test_grade_recall(owner->vault_principal, project, raw);
   fresh = checked ? cJSON_ParseWithOpts(checked, NULL, 1) : NULL;
   reason = "stale_context";
   if (!scoped(fresh, owner->vault_principal, project) || !cJSON_Compare(snapshot, fresh, 1))
      goto done;
   reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "ok");
   cJSON_AddStringToObject(reply, "goal", goal ? goal : "");
   cJSON_AddStringToObject(reply, "scope_kind", "project");
   cJSON_AddStringToObject(reply, "scope_id", project);
   cJSON_AddStringToObject(reply, "coverage", "recent_bounded_project_history");
   cJSON_AddStringToObject(reply, "goal_match", "not_inferred");
   cJSON_AddStringToObject(
       reply, "advisory",
       "Observed tool calls and configured-test results are historical evidence. "
       "They establish neither task correctness nor that an approach caused success; "
       "similarity to this goal has not been inferred.");
   cJSON_AddStringToObject(reply, "advisory_arm", "");
   cJSON *approaches = cJSON_AddArrayToObject(reply, "approaches");
   const cJSON *grade;
   cJSON_ArrayForEach(grade, cJSON_GetObjectItemCaseSensitive(snapshot, "items"))
   {
      const cJSON *experience = cJSON_GetObjectItemCaseSensitive(grade, "delegate_experience");
      const char *verdict = field(grade, "verdict");
      const cJSON *source = cJSON_GetObjectItemCaseSensitive(grade, "source");
      const cJSON *proof = cJSON_GetObjectItemCaseSensitive(grade, "proof");
      if (!cJSON_IsObject(experience))
         continue; /* Primary receipts and legacy goal-only rows are not approaches. */
      if (strcmp(field(grade, "qualification"), "configured_tests_only") ||
          strcmp(field(grade, "verification"), "tests_bound_revision") ||
          (strcmp(verdict, "passed") && strcmp(verdict, "failed")) ||
          strcmp(field(experience, "principal"), owner->vault_principal) ||
          strcmp(field(experience, "scope_kind"), "project") ||
          strcmp(field(experience, "scope_id"), project) ||
          strcmp(field(experience, "qualification"), "configured_tests_only") ||
          strcmp(field(experience, "action_coverage"), "bounded_canonical_tool_calls") ||
          strcmp(field(experience, "verdict"), verdict) || !cJSON_IsObject(source) ||
          strcmp(field(source, "actor"), owner->vault_principal) ||
          strcmp(field(proof, "scope_kind"), "project") ||
          strcmp(field(proof, "scope_id"), project) ||
          !cJSON_Compare(source, cJSON_GetObjectItemCaseSensitive(experience, "source"), 1) ||
          strcmp(field(cJSON_GetObjectItemCaseSensitive(experience, "parent_source"), "actor"),
                 owner->vault_principal) ||
          !field(grade, "verification_ref")[0] ||
          strcmp(field(grade, "verification_ref"), field(experience, "verification_ref")) ||
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(experience, "actions")) > 32 ||
          !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(experience, "actions")))
      {
         cJSON_Delete(reply);
         reply = NULL;
         reason = "unbound_learning_history";
         goto done;
      }
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "approach", "Observed canonical delegate tool calls");
      cJSON_AddStringToObject(item, "qualification", "configured_tests_only");
      cJSON_AddStringToObject(item, "verdict", verdict);
      cJSON_AddStringToObject(item, "source_ref", field(grade, "verification_ref"));
      cJSON_AddNumberToObject(item, "occurrences", 1);
      cJSON_AddItemToObject(
          item, "actions",
          cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(experience, "actions"), 1));
      cJSON_AddItemToObject(item, "receipt", cJSON_Duplicate(grade, 1));
      cJSON_AddItemToArray(approaches, item);
   }
done:
   if (fd >= 0)
      close(fd);
   if (workspace_turn_scope_end(scope))
   {
      cJSON_Delete(reply);
      reply = NULL;
      reason = "workspace_restore_failed";
   }
   free(raw);
   free(checked);
   cJSON_Delete(snapshot);
   cJSON_Delete(fresh);
   return reply ? reply : failure(reason);
}
