/* Admission glue delegates policy; these peers assert identity, argument and
 * ordering contracts. The actual permission/policy owners have their own suites. */
#include "server_native_tool_admission.h"
#include "request_context.h"
#include <aimee/tools/agent_tools.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int isolated = 1, cancelled, directive_denied, policy_denied, cancel_in_policy;
static int refuse_in_policy;
static agent_tool_permission_t permission;
static const char *bound_session = "session-a";
static char stages[8], attempt_seen[96];
static int n_stages;
static request_context_t owner;
static const cJSON *input;

const char *session_id(void)
{
   return bound_session;
}
int agent_tools_scope_isolated(void)
{
   return isolated;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
static void stage(char value)
{
   assert(n_stages < 6);
   stages[n_stages++] = value;
   stages[n_stages] = 0;
}
agent_tool_permission_t agent_tools_permission_check(const char *tool, const cJSON *arguments)
{
   assert(!strcmp(tool, "git_push") && arguments == input);
   stage('R');
   return permission;
}
char *agent_tools_permission_error(agent_tool_permission_t decision)
{
   assert(decision == permission && decision != AGENT_TOOL_PERMISSION_ALLOWED);
   return strdup("permission owner refusal");
}
static void check_arguments(const char *tool, const char *wire)
{
   assert(!strcmp(tool, "git_push"));
   cJSON *arguments = cJSON_Parse(wire);
   assert(arguments && cJSON_Compare(arguments, input, 1));
   cJSON_Delete(arguments);
}
int directive_check_tool(const char *tool, const char *wire, char *reason, size_t capacity)
{
   assert(!strcmp(stages, "R"));
   check_arguments(tool, wire);
   stage('D');
   snprintf(reason, capacity, "directive owner refusal");
   return directive_denied ? -1 : 0;
}
static int metadata_refused;
int tool_policy_metadata(const char *tool, aimee_tool_metadata_t *metadata, char *effect,
                         size_t effect_len, char *reason, size_t reason_len)
{
   assert(!strcmp(tool, "git_push") && !strcmp(stages, "RD"));
   stage('M');
   if (metadata_refused)
   {
      snprintf(reason, reason_len, "metadata owner unavailable");
      return -1;
   }
   *metadata = (aimee_tool_metadata_t){AIMEE_TOOL_CLASS_REMOTE, AIMEE_TOOL_POLICY_EXTERNAL,
                                       AIMEE_TOOL_POLICY_EXTERNAL | AIMEE_TOOL_POLICY_NETWORK};
   snprintf(effect, effect_len, "external");
   return 0;
}
int policy_check_tool_metadata_attempt(const char *tool, const aimee_tool_metadata_t *metadata,
                                       const char *effect, const char *wire, const char *attempt,
                                       char *reason, size_t capacity)
{
   assert(!strcmp(stages, "RDM") && !strcmp(effect, "external"));
   check_arguments(tool, wire);
   assert(metadata->categories == (AIMEE_TOOL_POLICY_EXTERNAL | AIMEE_TOOL_POLICY_NETWORK));
   stage('P');
   snprintf(attempt_seen, sizeof attempt_seen, "%s", attempt);
   snprintf(reason, capacity, "policy owner refusal");
   if (cancel_in_policy)
      cancelled = 1;
   if (refuse_in_policy)
      (void)request_context_refuse_assembly("revoked");
   return policy_denied ? -1 : 0;
}
static const char *admit(const char *session, const cJSON *arguments, char *error, size_t capacity)
{
   stages[0] = 0;
   n_stages = 0;
   return server_native_tool_admit("git_push", arguments, session, error, capacity);
}
static void expect_refusal(const char *session, const cJSON *arguments, const char *reason,
                           const char *expected_stages)
{
   char error[256];
   const char *actual = admit(session, arguments, error, sizeof error);
   assert(actual && !strcmp(actual, reason) && error[0]);
   assert(!strcmp(stages, expected_stages));
}
int main(void)
{
   snprintf(owner.principal, sizeof owner.principal, "alice");
   snprintf(owner.request_id, sizeof owner.request_id, "host-callback-one");
   owner.model_execution = 1;
   request_context_set(&owner);
   cJSON *arguments = cJSON_Parse("{\"remote\":\"origin\",\"attempt_id\":\"forged\","
                                  "\"side_effect\":\"read\",\"authorized\":true}");
   assert(arguments);
   input = arguments;
   char error[256] = "stale refusal";
   const char *result = admit("session-a", arguments, error, sizeof error);
   assert(!result && !error[0]);
   assert(!strcmp(stages, "RDMP") && !strcmp(attempt_seen, "native-mcp:host-callback-one"));
   snprintf(owner.request_id, sizeof owner.request_id, "host-callback-two");
   request_context_set(&owner);
   result = admit("session-a", arguments, error, sizeof error);
   assert(!result);
   assert(!strcmp(attempt_seen, "native-mcp:host-callback-two"));

   stages[0] = 0;
   n_stages = 0;
   result = server_native_tool_admit_attempt("git_push", arguments, "session-a",
                                             "native-ir:verified-host-source", error, sizeof error);
   assert(!result && !strcmp(stages, "RDMP"));
   assert(!strcmp(attempt_seen, "native-ir:verified-host-source"));
   stages[0] = 0;
   n_stages = 0;
   result = server_native_tool_admit_attempt("git_push", arguments, "session-a", "", error,
                                             sizeof error);
   assert(result && !strcmp(result, "identity") && !stages[0]);

   permission = AGENT_TOOL_PERMISSION_ROLE;
   expect_refusal("session-a", arguments, "role", "R");
   permission = AGENT_TOOL_PERMISSION_SHELL;
   expect_refusal("session-a", arguments, "permission", "R");
   permission = AGENT_TOOL_PERMISSION_ALLOWED;
   directive_denied = 1;
   expect_refusal("session-a", arguments, "directive", "RD");
   directive_denied = 0;
   metadata_refused = 1;
   expect_refusal("session-a", arguments, "policy", "RDM");
   metadata_refused = 0;
   policy_denied = 1;
   expect_refusal("session-a", arguments, "policy", "RDMP");
   policy_denied = 0;
   cancel_in_policy = 1;
   expect_refusal("session-a", arguments, "cancelled", "RDMP");
   cancel_in_policy = 0;
   expect_refusal("session-a", arguments, "cancelled", "");
   cancelled = 0;
   refuse_in_policy = 1;
   expect_refusal("session-a", arguments, "context", "RDMP");
   refuse_in_policy = 0;
   expect_refusal("session-a", arguments, "context", "");
   request_context_set(&owner);

   expect_refusal("session-b", arguments, "identity", "");
   bound_session = NULL;
   expect_refusal("session-a", arguments, "identity", "");
   bound_session = "session-a";
   isolated = 0;
   expect_refusal("session-a", arguments, "identity", "");
   isolated = 1;
   owner.model_execution = 0;
   request_context_set(&owner);
   expect_refusal("session-a", arguments, "identity", "");
   owner.model_execution = 1;
   owner.principal[0] = 0;
   request_context_set(&owner);
   expect_refusal("session-a", arguments, "identity", "");
   snprintf(owner.principal, sizeof owner.principal, "alice");
   owner.request_id[0] = 0;
   request_context_set(&owner);
   expect_refusal("session-a", arguments, "identity", "");
   request_context_clear();
   expect_refusal("session-a", arguments, "identity", "");
   snprintf(owner.request_id, sizeof owner.request_id, "host-request");
   request_context_set(&owner);
   expect_refusal("session-a", NULL, "bad_args", "");
   cJSON *array = cJSON_CreateArray();
   expect_refusal("session-a", array, "bad_args", "");
   cJSON_Delete(array);
   cJSON_Delete(arguments);
   request_context_clear();
   puts("native MCP admission preserves owner policy, argument binding and cancellation");
   return 0;
}
