/* C enforcement caller -> required Go execution-policy process over the bus. */
#include "agent_exec.h"

#include <aimee/execution-policy/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "computer_use.h"
#include "headers/module_json_call.h"

#include "request_context.h"
#include "util.h"
#include <limits.h>
#include <unistd.h>
#include "agent_tasks.h"
#include "db1_client/session_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define EXECUTION_POLICY_TIMEOUT_MS 5000

int policy_load(void)
{
   return obs_bus_module_available(AIMEE_EXECUTION_POLICY_EVENT_TOOL) ? 0 : -1;
}

static void set_reason(char *out, size_t cap, const char *reason)
{
   if (out && cap > 0)
      snprintf(out, cap, "%s", reason ? reason : "execution policy denied the action");
}

static int policy_baseline(const char *tool_name, const char *side_effect, const char *args_json,
                           char *reason_out, size_t reason_len, int *discovery);

static int policy_check_exploration(const char *tool, const char *effect, const char *args,
                                    const char *attempt, char *reason, size_t reason_len)
{
   const request_context_t *ctx = request_context_get();
   cJSON *binding = ctx ? cJSON_Parse(ctx->exploration_binding) : NULL;
   const char *session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "session"));
   const char *workspace =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "working_directory"));
   if (!session || !workspace || !ctx->principal[0] || !attempt || !attempt[0])
   {
      cJSON_Delete(binding);
      return 1; /* no adaptive state: use the required baseline policy module */
   }
   cJSON *request = cJSON_CreateObject();
   cJSON *arguments = cJSON_Parse(args);
   if (!request || !arguments)
   {
      cJSON_Delete(request);
      cJSON_Delete(arguments);
      cJSON_Delete(binding);
      set_reason(reason, reason_len, "invalid tool policy request");
      return -1;
   }
   char id[256];
   int n = snprintf(id, sizeof(id), "%s:%s", ctx->request_id, attempt);
   if (n < 0 || (size_t)n >= sizeof(id))
   {
      cJSON_Delete(arguments);
      cJSON_Delete(request);
      cJSON_Delete(binding);
      set_reason(reason, reason_len, "tool attempt identity is too long");
      return -1;
   }
   cJSON_AddStringToObject(request, "operation", "check");
   cJSON_AddItemToObject(request, "binding", binding);
   cJSON_AddStringToObject(request, "tool", tool);
   cJSON_AddStringToObject(request, "side_effect", effect ? effect : "");
   cJSON_AddStringToObject(request, "attempt_id", id);
   cJSON_AddStringToObject(request, "canonical_path", workspace);
   cJSON_AddNumberToObject(request, "bytes", (double)agent_tool_output_cap());
   cJSON_AddNumberToObject(request, "tokens", (double)agent_tool_output_cap());
   cJSON_AddItemToObject(request, "tool_arguments", arguments);
   char *wire = cJSON_PrintUnformatted(request);
   char *reply = malloc(65536);
   cJSON *result = NULL;
   if (wire && reply &&
       db1_session_exploration_apply(ctx->principal, session, wire, reply, 65536) == 0)
      result = cJSON_Parse(reply);
   const cJSON *allowed = cJSON_GetObjectItemCaseSensitive(result, "allowed");
   const char *why = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "reason"));
   int valid = cJSON_IsBool(allowed) && why;
   int permit = valid && cJSON_IsTrue(allowed);
   if (!valid)
      fprintf(stderr, "[execution-policy] adaptive session state unavailable; using baseline\n");
   set_reason(reason, reason_len, why ? why : "session exploration accounting unavailable");
   cJSON_Delete(result);
   free(reply);
   free(wire);
   cJSON_Delete(request);
   return valid ? (permit ? 0 : -1) : 1;
}

int policy_check_tool_attempt(const char *tool, const char *effect, const char *args,
                              const char *attempt, char *reason, size_t reason_len)
{
   /* Baseline authorization always runs, including computer-use restrictions. */
   int discovery = 0;
   if (policy_baseline(tool, effect, args, reason, reason_len, &discovery) != 0)
      return -1;
   if (!discovery)
      return 0;
   int decision = policy_check_exploration(tool, effect, args, attempt, reason, reason_len);
   if (decision == 1 && discovery == 2)
   {
      set_reason(reason, reason_len,
                 "operator exploration accounting requires an authenticated session");
      return -1;
   }
   return decision == 1 ? 0 : decision;
}

static int policy_baseline(const char *tool_name, const char *side_effect, const char *args_json,
                           char *reason_out, size_t reason_len, int *discovery)
{
   if (!tool_name || !tool_name[0] || !args_json)
   {
      set_reason(reason_out, reason_len, "execution-policy input is invalid");
      return -1;
   }

   cJSON *request = cJSON_CreateObject();
   cJSON *arguments = cJSON_Parse(args_json);
   cJSON *computer = cJSON_CreateObject();
   cJSON *domains = cJSON_CreateArray();
   if (!request || !arguments || !computer || !domains)
   {
      cJSON_Delete(request);
      cJSON_Delete(arguments);
      cJSON_Delete(computer);
      cJSON_Delete(domains);
      set_reason(reason_out, reason_len, "execution-policy request could not be built");
      return -1;
   }

   computer_use_policy_t policy;
   computer_use_policy_from_config(&policy);
   cJSON_AddStringToObject(request, "tool", tool_name);
   cJSON_AddStringToObject(request, "side_effect", side_effect ? side_effect : "");
   cJSON_AddItemToObject(request, "arguments", arguments);
   cJSON_AddBoolToObject(computer, "enabled", policy.enabled);
   cJSON_AddStringToObject(computer, "default_navigation", policy.default_navigation);
   cJSON_AddBoolToObject(computer, "redact_sensitive_screenshots",
                         policy.redact_sensitive_screenshots);
   for (int i = 0; i < policy.allowed_domain_count; ++i)
      cJSON_AddItemToArray(domains, cJSON_CreateString(policy.allowed_domains[i]));
   cJSON_AddItemToObject(computer, "allowed_domains", domains);
   cJSON_AddItemToObject(request, "computer_use", computer);

   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(
       AIMEE_EXECUTION_POLICY_EVENT_TOOL, AIMEE_EXECUTION_POLICY_STAGE_TOOL, request,
       AIMEE_MODULE_MESSAGE_MAX_BODY, EXECUTION_POLICY_TIMEOUT_MS, &result);
   if (!response)
   {
      char failure[160];
      snprintf(failure, sizeof failure, "execution-policy module failed: %s",
               aimee_module_call_result_name(result));
      set_reason(reason_out, reason_len, failure);
      return -1; /* required authorization is fail-closed */
   }

   const cJSON *allowed = cJSON_GetObjectItemCaseSensitive(response, "allowed");
   const cJSON *reason = cJSON_GetObjectItemCaseSensitive(response, "reason");
   if (!cJSON_IsBool(allowed) || !cJSON_IsString(reason) || strlen(reason->valuestring) > 255)
   {
      cJSON_Delete(response);
      set_reason(reason_out, reason_len, "execution-policy returned an invalid decision");
      return -1;
   }
   const cJSON *exploration = cJSON_GetObjectItemCaseSensitive(response, "exploration");
   if (discovery && cJSON_IsObject(exploration))
      *discovery =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(exploration, "accounting_required")) ? 2
                                                                                             : 1;
   int permit = cJSON_IsTrue(allowed);
   set_reason(reason_out, reason_len, reason->valuestring);
   cJSON_Delete(response);
   return permit ? 0 : -1;
}

/* The host forwards memory-owned metadata to the session owner. It does not
 * decide coverage, calibration or source eligibility here. */
int policy_prepare_exploration(const cJSON *offer, const char *session, const char *workspace,
                               const char *project)
{
   const request_context_t *ctx = request_context_get();
   char budget_task[129] = "";
   cJSON *parent = ctx ? cJSON_Parse(ctx->exploration_binding) : NULL;
   const char *parent_session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent, "session"));
   const char *parent_principal =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent, "principal"));
   const char *parent_task =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent, "budget_task"));
   if (!parent_task || !parent_task[0])
      parent_task = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent, "task"));
   if (ctx && session && parent_session && parent_principal && parent_task &&
       strcmp(session, parent_session) == 0 && strcmp(ctx->principal, parent_principal) == 0 &&
       strlen(parent_task) < sizeof(budget_task))
      snprintf(budget_task, sizeof(budget_task), "%s", parent_task);
   cJSON_Delete(parent);
   (void)request_context_set_exploration_binding("");
   if (!ctx || !ctx->principal[0] || !session || !session[0] || !workspace || !workspace[0] ||
       !project || !project[0] || !cJSON_IsObject(offer))
      return -1;
   const char *owner =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "memory_owner"));
   const char *generation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "index_generation"));
   if (!owner || !generation)
      return -1;
   cJSON *binding = cJSON_CreateObject();
   cJSON_AddStringToObject(binding, "principal", ctx->principal);
   cJSON_AddStringToObject(binding, "session", session);
   char task[64] = "session-task";
   int job = agent_get_durable_job_id();
   if (job > 0)
      snprintf(task, sizeof(task), "job:%d", job);
   cJSON_AddStringToObject(binding, "task", task);
   cJSON_AddStringToObject(binding, "budget_task", budget_task[0] ? budget_task : task);
   cJSON_AddStringToObject(binding, "project", project);
   cJSON_AddStringToObject(binding, "workspace", workspace);
   /* Workspace is a namespace URI, never a filesystem path. Bind the actual
    * host execution directory separately for path-scoped recovery. */
   char cwd[PATH_MAX], canonical[PATH_MAX];
   const char *effective_cwd = run_cmd_get_cwd();
   if ((!effective_cwd || !effective_cwd[0]) && getcwd(cwd, sizeof(cwd)))
      effective_cwd = cwd;
   cJSON_AddStringToObject(binding, "working_directory",
                           effective_cwd && realpath(effective_cwd, canonical) ? canonical : "");
   cJSON_AddStringToObject(binding, "worktree_generation", "unavailable");
   cJSON_AddStringToObject(binding, "index_generation", generation);
   cJSON_AddStringToObject(binding, "memory_owner", owner);
   const char *plan_digest =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "plan_digest"));
   const char *versions =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "source_versions_digest"));
   cJSON_AddStringToObject(binding, "plan_digest", plan_digest ? plan_digest : "");
   cJSON_AddStringToObject(binding, "source_versions_digest", versions ? versions : "");
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "prepare");
   cJSON_AddItemToObject(request, "binding", cJSON_Duplicate(binding, 1));
   cJSON_AddItemToObject(request, "offer", cJSON_Duplicate(offer, 1));
   char *wire = cJSON_PrintUnformatted(request);
   char *serialized = NULL;
   char *reply = malloc(65536);
   int rc = -1;
   if (wire && reply &&
       db1_session_exploration_apply(ctx->principal, session, wire, reply, 65536) == 0)
   {
      cJSON *result = cJSON_Parse(reply);
      const cJSON *contract = cJSON_GetObjectItemCaseSensitive(result, "contract");
      const cJSON *issued_binding = cJSON_GetObjectItemCaseSensitive(contract, "binding");
      if (cJSON_IsObject(issued_binding))
         serialized = cJSON_PrintUnformatted(issued_binding);
      if (serialized)
         rc = request_context_set_exploration_binding(serialized);
      cJSON_Delete(result);
   }
   free(reply);
   free(serialized);
   free(wire);
   cJSON_Delete(request);
   cJSON_Delete(binding);
   return rc;
}

int policy_check_session_tool(const char *session, const char *tool, const char *arguments,
                              const char *attempt, char *reason, size_t reason_len)
{
   int discovery = 0;
   if (policy_baseline(tool, "filesystem", arguments, reason, reason_len, &discovery) != 0)
      return -1;
   if (!discovery)
      return 0;
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->principal[0] || !session || !session[0] || !attempt || !attempt[0])
   {
      if (discovery == 2)
      {
         set_reason(reason, reason_len,
                    "operator exploration accounting requires an authenticated session");
         return -1;
      }
      return 0;
   }
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "check_session");
   cJSON_AddStringToObject(request, "tool", tool);
   cJSON_AddStringToObject(request, "side_effect", "filesystem");
   cJSON_AddStringToObject(request, "attempt_id", attempt);
   cJSON_AddBoolToObject(request, "unknown_output", 1);
   cJSON_AddItemToObject(request, "tool_arguments", cJSON_Parse(arguments));
   char *wire = cJSON_PrintUnformatted(request);
   char *reply = malloc(65536);
   cJSON *result = NULL;
   if (wire && reply &&
       db1_session_exploration_apply(ctx->principal, session, wire, reply, 65536) == 0)
      result = cJSON_Parse(reply);
   /* A hook may precede the first context plan. No adaptive contract is not
    * an authorization bypass: the baseline verdict above remains required. */
   int denied =
       cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "allowed")) ||
       (discovery == 2 && !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(result, "allowed")));
   const char *why = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "reason"));
   if (denied)
      set_reason(reason, reason_len, why ? why : "exploration budget exhausted");
   free(reply);
   free(wire);
   cJSON_Delete(request);
   cJSON_Delete(result);
   return denied ? -1 : 0;
}

int policy_check_tool(const char *tool, const char *effect, const char *args, char *reason,
                      size_t reason_len)
{
   int discovery = 0;
   int rc = policy_baseline(tool, effect, args, reason, reason_len, &discovery);
   if (rc == 0 && discovery == 2)
   {
      set_reason(reason, reason_len,
                 "operator exploration accounting requires a task-bound dispatch");
      return -1;
   }
   return rc;
}

static char *policy_session_operation(cJSON *request)
{
   const request_context_t *ctx = request_context_get();
   cJSON *binding = ctx ? cJSON_Parse(ctx->exploration_binding) : NULL;
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "session"));
   char *reply = NULL;
   if (sid && ctx->principal[0])
   {
      cJSON_AddItemToObject(request, "binding", binding);
      binding = NULL;
      char *wire = cJSON_PrintUnformatted(request);
      reply = malloc(65536);
      if (!wire || !reply ||
          db1_session_exploration_apply(ctx->principal, sid, wire, reply, 65536) != 0)
      {
         free(reply);
         reply = NULL;
      }
      free(wire);
   }
   cJSON_Delete(binding);
   cJSON_Delete(request);
   return reply;
}

char *policy_observe_indexed(const char *tool, const char *arguments, const char *attempt,
                             const char *result)
{
   if (!tool || (strcmp(tool, "code_search") && strcmp(tool, "find_symbol")) || !result ||
       strlen(result) > 4096 || !attempt)
      return NULL;
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->exploration_binding[0])
      return NULL;
   char id[256];
   int n = snprintf(id, sizeof(id), "%s:%s", ctx->request_id, attempt);
   if (n < 0 || (size_t)n >= sizeof(id))
      return NULL;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "observe_indexed");
   cJSON_AddStringToObject(request, "tool", tool);
   cJSON_AddStringToObject(request, "attempt_id", id);
   cJSON_AddStringToObject(request, "tool_result", result);
   cJSON_AddItemToObject(request, "tool_arguments", cJSON_Parse(arguments));
   return policy_session_operation(request);
}

char *policy_expand_exploration(const char *reason, const char *gap, const char *outcome)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "expand");
   cJSON_AddStringToObject(request, "reason", reason ? reason : "");
   cJSON_AddStringToObject(request, "gap", gap ? gap : "");
   cJSON_AddStringToObject(request, "outcome_id", outcome ? outcome : "");
   char *reply = policy_session_operation(request);
   return reply
              ? reply
              : strdup("error: expansion requires a recent host-recorded indexed gap in this task");
}

void policy_complete_exploration_turn(int turn)
{
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->exploration_binding[0] || turn < 0)
      return;
   char id[128];
   snprintf(id, sizeof(id), "%s:%d", ctx->request_id, turn);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "observe_turn");
   cJSON_AddStringToObject(request, "turn_id", id);
   free(policy_session_operation(request));
}

/* Resolve the protected session contract at an authenticated external tool
 * boundary. Client JSON supplies no binding, allowance or observed outcome. */
int policy_bind_session_exploration(const char *session)
{
   (void)request_context_set_exploration_binding("");
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->principal[0] || !session || !session[0])
      return -1;
   char cwd[PATH_MAX], canonical[PATH_MAX];
   const char *effective = run_cmd_get_cwd();
   if ((!effective || !effective[0]) && getcwd(cwd, sizeof(cwd)))
      effective = cwd;
   if (!effective || !realpath(effective, canonical))
      return -1;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "bind_session");
   cJSON_AddStringToObject(request, "canonical_path", canonical);
   char *wire = cJSON_PrintUnformatted(request);
   char *reply = malloc(65536);
   int rc = -1;
   if (wire && reply &&
       db1_session_exploration_apply(ctx->principal, session, wire, reply, 65536) == 0)
   {
      cJSON *result = cJSON_Parse(reply);
      cJSON *binding = cJSON_GetObjectItemCaseSensitive(result, "binding");
      char *serialized = cJSON_IsObject(binding) ? cJSON_PrintUnformatted(binding) : NULL;
      if (serialized)
         rc = request_context_set_exploration_binding(serialized);
      free(serialized);
      cJSON_Delete(result);
   }
   free(reply);
   free(wire);
   cJSON_Delete(request);
   return rc;
}

char *policy_annotate_indexed(const char *tool, const char *arguments, const char *attempt,
                              char *result)
{
   char *outcome = policy_observe_indexed(tool, arguments, attempt, result);
   if (!outcome)
      return result;
   cJSON *observation = cJSON_Parse(outcome);
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(observation, "expansion_available")))
   {
      size_t n = (result ? strlen(result) : 0) + strlen(outcome) + 32;
      char *with_gap = malloc(n);
      if (with_gap)
      {
         snprintf(with_gap, n, "%s\nExploration recovery: %s", result ? result : "", outcome);
         free(result);
         result = with_gap;
      }
   }
   cJSON_Delete(observation);
   free(outcome);
   return result;
}
