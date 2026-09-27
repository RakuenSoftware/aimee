/* Host transport for tool-owner resources, memory-owner freshness and durable
 * execution-policy decisions. This file makes no memory eligibility decision. */
#include "agent_exec.h"
#include "server.h"
#include "db1_client/session_state.h"
#include "ingress_preinject.h"
#include "module_commands.h"
#include "request_context.h"
#include "util.h"
#include <aimee/tools/module_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static __thread char action_attempt[128];
static __thread char action_session[129];
static __thread char action_principal[129];
static __thread cJSON *action_intent;
static __thread cJSON *action_verification_request;
static __thread cJSON *action_existing_receipt;
static __thread char action_refusal[128];

void policy_action_attempt(const char *attempt)
{
   snprintf(action_attempt, sizeof(action_attempt), "%s", attempt ? attempt : "");
   action_refusal[0] = 0;
   cJSON_Delete(action_existing_receipt);
   action_existing_receipt = NULL;
   cJSON_Delete(action_intent);
   action_intent = NULL;
   cJSON_Delete(action_verification_request);
   action_verification_request = NULL;
}

static const char *action_text(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}

static cJSON *action_store(const char *principal, const char *sid, cJSON *request)
{
   char *wire = cJSON_PrintUnformatted(request);
   char *reply = malloc(65536);
   cJSON *result = NULL;
   if (wire && reply && db1_governed_action_apply(principal, sid, wire, reply, 65536) == 0)
      result = cJSON_Parse(reply);
   free(wire);
   free(reply);
   return result;
}

/* A copied host binding carries the parent across native task/session forks.
 * Bind before exploration replaces that binding with the child's fresh one. */
int policy_action_inherit(const char *session)
{
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->exploration_binding[0] || !session || !session[0])
      return 0;
   cJSON *binding = cJSON_Parse(ctx->exploration_binding);
   const char *parent = action_text(binding, "session");
   if (!parent[0] || !strcmp(parent, session))
   {
      cJSON_Delete(binding);
      return 0;
   }
   int accepted = 0;
   if (!strcmp(action_text(binding, "principal"), ctx->principal))
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "operation", "fork");
      cJSON_AddStringToObject(request, "parent_session", parent);
      cJSON *reply = action_store(ctx->principal, session, request);
      accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "allowed"));
      cJSON_Delete(reply);
      cJSON_Delete(request);
   }
   cJSON_Delete(binding);
   if (!accepted)
      (void)request_context_refuse_assembly("action_lineage_unavailable");
   return accepted ? 0 : -1;
}

static cJSON *action_evidence(const request_context_t *ctx, void **guard)
{
   if (ctx->memory_source_release[0])
   {
      if (ingress_preinject_acquire_send_guard(guard) != 0)
         return NULL;
      cJSON *request = cJSON_CreateObject(), *result = NULL;
      cJSON_AddStringToObject(request, "operation", "action-evidence");
      cJSON_AddStringToObject(request, "source_release_ticket", ctx->memory_source_release);
      cJSON_AddStringToObject(request, "request_id", ctx->request_id);
      cJSON_AddStringToObject(request, "principal", ctx->principal);
      cJSON_AddStringToObject(request, "caller_subject", ctx->caller_subject);
      int rc =
          aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 500, &result);
      cJSON_Delete(request);
      if (rc != 1 || strcmp(action_text(result, "status"), "ok") != 0)
      {
         cJSON_Delete(result);
         return NULL;
      }
      return result;
   }
   if (ctx->memory_receipt_required || ctx->context_refused)
      return NULL;
   /* Explicit absence of a retained memory handle is not a verified receipt. */
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "evidence_sha256", "");
   cJSON_AddStringToObject(result, "context_receipt", "");
   cJSON_AddStringToObject(result, "revocation_generation", "no_retained_memory");
   cJSON_AddBoolToObject(result, "memory_required", 0);
   time_t now = time(NULL);
   struct tm utc;
   char stamp[32];
   gmtime_r(&now, &utc);
   strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
   cJSON_AddStringToObject(result, "checked_at", stamp);
   now += 2;
   gmtime_r(&now, &utc);
   strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
   cJSON_AddStringToObject(result, "expires_at", stamp);
   return result;
}

int policy_action_begin(const char *name, cJSON **arguments, const char *cwd, const char *sid,
                        int authorized)
{
   const request_context_t *ctx = request_context_get();
   snprintf(action_refusal, sizeof(action_refusal), "host_action_binding_unavailable");
   if (!ctx || ctx->context_refused || !ctx->principal[0] || !ctx->request_id[0] || !sid ||
       !sid[0] || !action_attempt[0] || (!authorized && !(ctx->capabilities & CAP_TOOL_EXECUTE)) ||
       action_intent)
      return -1;
   if (policy_action_inherit(sid) != 0)
   {
      snprintf(action_refusal, sizeof(action_refusal), "action_lineage_unavailable");
      return -1;
   }
   int rc = -1;
   void *guard = NULL;
   cJSON *caller = cJSON_CreateObject(), *request = cJSON_CreateObject();
   cJSON_AddBoolToObject(caller, "authenticated", 1);
   cJSON_AddStringToObject(caller, "principal", ctx->principal);
   cJSON_AddStringToObject(request, "tool", name);
   cJSON_AddStringToObject(request, "directory", cwd ? cwd : "");
   cJSON_AddItemToObject(request, "arguments", cJSON_Duplicate(*arguments, 1));
   snprintf(action_refusal, sizeof(action_refusal), "exact_resource_adapter_unavailable");
   cJSON *resource = aimee_module_command_call_context(AIMEE_TOOLS_EVENT_ACTION_RESOURCE,
                                                       AIMEE_TOOLS_STAGE_ACTION_RESOURCE,
                                                       "describe", request, caller);
   cJSON_Delete(caller);
   cJSON_Delete(request);
   request = NULL;
   cJSON *effective = cJSON_GetObjectItemCaseSensitive(resource, "effective_arguments");
   cJSON *evidence = NULL, *intent = NULL, *decision = NULL, *next = NULL;
   if (!cJSON_IsObject(effective) || !action_text(resource, "class")[0] ||
       (!strcmp(action_text(resource, "class"), "external_publish") &&
        !ctx->memory_source_release[0]))
      goto done;
   /* Execute exactly the owner-canonical arguments, after all host rewrites. */
   cJSON_Delete(*arguments);
   *arguments = cJSON_Duplicate(effective, 1);
   char *wire = cJSON_PrintUnformatted(*arguments);
   char reason[256];
   snprintf(action_refusal, sizeof(action_refusal), "current_operator_policy_refused");
   if (!wire || policy_recheck_action_tool(name, "filesystem", wire, reason, sizeof(reason)) != 0)
   {
      free(wire);
      goto done;
   }
   free(wire);
   snprintf(action_refusal, sizeof(action_refusal), "current_memory_evidence_unavailable");
   evidence = action_evidence(ctx, &guard);
   if (!evidence)
      goto done;
   intent = cJSON_CreateObject();
   cJSON_AddStringToObject(intent, "request_id", ctx->request_id);
   cJSON *task_binding = cJSON_Parse(ctx->exploration_binding);
   const char *task = !strcmp(action_text(task_binding, "principal"), ctx->principal)
                          ? action_text(task_binding, "task")
                          : "";
   cJSON_AddStringToObject(intent, "task_id", task[0] ? task : sid);
   cJSON_Delete(task_binding);
   cJSON_AddStringToObject(intent, "attempt_id", action_attempt);
   cJSON_AddStringToObject(intent, "tool", name);
   cJSON_AddStringToObject(intent, "action_class", action_text(resource, "class"));
   cJSON_AddStringToObject(intent, "destination", action_text(resource, "destination"));
   cJSON_AddStringToObject(intent, "payload_sha256", action_text(resource, "payload_sha256"));
   cJSON_AddStringToObject(intent, "work_units", action_text(resource, "request_bytes"));
   cJSON_AddStringToObject(intent, "purpose", "authorized tool invocation");
   const char *fields[] = {"evidence_sha256", "context_receipt", "revocation_generation"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      cJSON_AddStringToObject(intent, fields[i], action_text(evidence, fields[i]));
   request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "issue");
   cJSON_AddItemToObject(request, "arguments", cJSON_Duplicate(*arguments, 1));
   cJSON_AddStringToObject(request, "registry_class", action_text(resource, "class"));
   cJSON_AddItemToObject(request, "intent", intent);
   intent = NULL;
   snprintf(action_refusal, sizeof(action_refusal), "action_owner_unavailable");
   decision = action_store(ctx->principal, sid, request);
   next = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(decision, "receipt"), "intent"),
                          1);
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(decision, "allowed")) || !next)
   {
      if (action_text(decision, "reason")[0])
         snprintf(action_refusal, sizeof(action_refusal), "%s", action_text(decision, "reason"));
      goto done;
   }
   cJSON_ReplaceItemInObjectCaseSensitive(request, "intent", cJSON_Duplicate(next, 1));
   cJSON_AddBoolToObject(evidence, "authorized", 1);
   cJSON_AddStringToObject(evidence, "policy_generation", action_text(next, "policy_generation"));
   cJSON_AddItemToObject(request, "freshness", evidence);
   evidence = NULL;
   const char *operations[] = {"admit", "dispatch"};
   for (size_t i = 0; i < 2; i++)
   {
      cJSON_ReplaceItemInObjectCaseSensitive(request, "operation",
                                             cJSON_CreateString(operations[i]));
      cJSON_Delete(decision);
      decision = action_store(ctx->principal, sid, request);
      if (!cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(decision, i ? "dispatch_allowed" : "allowed")))
      {
         if (action_text(decision, "reason")[0])
            snprintf(action_refusal, sizeof(action_refusal), "%s", action_text(decision, "reason"));
         if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(decision, "receipt")))
            action_existing_receipt = cJSON_Duplicate(decision, 1);
         goto done;
      }
   }
   action_intent = next;
   next = NULL;
   action_verification_request = cJSON_CreateObject();
   cJSON_AddStringToObject(action_verification_request, "tool", name);
   cJSON_AddStringToObject(action_verification_request, "directory", cwd);
   cJSON_AddItemToObject(action_verification_request, "arguments", cJSON_Duplicate(*arguments, 1));
   cJSON_AddStringToObject(action_verification_request, "destination",
                           action_text(action_intent, "destination"));
   cJSON_AddStringToObject(action_verification_request, "payload_sha256",
                           action_text(action_intent, "payload_sha256"));
   snprintf(action_session, sizeof(action_session), "%s", sid);
   snprintf(action_principal, sizeof(action_principal), "%s", ctx->principal);
   rc = 0;
   action_refusal[0] = 0;
done:
   ingress_preinject_release_send_guard(guard);
   cJSON_Delete(request);
   cJSON_Delete(resource);
   cJSON_Delete(evidence);
   cJSON_Delete(intent);
   cJSON_Delete(decision);
   cJSON_Delete(next);
   return rc;
}

char *policy_action_finish(const char *verdict, char *result)
{
   if (action_existing_receipt)
   {
      action_refusal[0] = 0;
      char *prior = cJSON_PrintUnformatted(action_existing_receipt);
      cJSON_Delete(action_existing_receipt);
      action_existing_receipt = NULL;
      free(result);
      return prior ? prior : safe_strdup("error: prior action receipt unavailable; do not replay");
   }
   if (!action_intent)
   {
      if (!action_refusal[0])
         return result;
      cJSON *refusal = cJSON_CreateObject();
      cJSON_AddStringToObject(refusal, "status", "refused");
      cJSON_AddStringToObject(refusal, "reason", action_refusal);
      cJSON_AddBoolToObject(refusal, "dispatch_allowed", 0);
      char *encoded = cJSON_PrintUnformatted(refusal);
      cJSON_Delete(refusal);
      action_refusal[0] = 0;
      free(result);
      return encoded ? encoded : safe_strdup("error: governed action refused");
   }
   cJSON *request = cJSON_CreateObject(), *outcome = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "outcome");
   cJSON_AddStringToObject(request, "registry_class", action_text(action_intent, "action_class"));
   cJSON_AddItemToObject(request, "intent", cJSON_Duplicate(action_intent, 1));
   const char *state =
       result && verdict && strcmp(verdict, "ok") == 0 ? "acknowledged" : "outcome_unknown";
   cJSON *verification = NULL;
   if (strcmp(state, "acknowledged") == 0 &&
       strcmp(action_text(action_intent, "tool"), "write_file") == 0)
   {
      cJSON *caller = cJSON_CreateObject();
      cJSON_AddBoolToObject(caller, "authenticated", 1);
      cJSON_AddStringToObject(caller, "principal", action_principal);
      verification = aimee_module_command_call_context(AIMEE_TOOLS_EVENT_ACTION_RESOURCE,
                                                       AIMEE_TOOLS_STAGE_ACTION_RESOURCE, "verify",
                                                       action_verification_request, caller);
      cJSON_Delete(caller);
      if (strcmp(action_text(verification, "state"), "effect_confirmed") == 0 &&
          strcmp(action_text(verification, "destination"),
                 action_text(action_intent, "destination")) == 0 &&
          strcmp(action_text(verification, "payload_sha256"),
                 action_text(action_intent, "payload_sha256")) == 0 &&
          action_text(verification, "object_version")[0])
      {
         state = "effect_confirmed";
         cJSON_AddStringToObject(outcome, "object_version",
                                 action_text(verification, "object_version"));
      }
      else
         state = "outcome_unknown";
   }
   cJSON_Delete(verification);
   cJSON_Delete(action_verification_request);
   action_verification_request = NULL;
   cJSON_AddStringToObject(outcome, "state", state);
   cJSON_AddStringToObject(outcome, "evidence_ref", action_text(action_intent, "action_id"));
   cJSON_AddStringToObject(outcome, "destination", action_text(action_intent, "destination"));
   cJSON_AddStringToObject(outcome, "payload_sha256", action_text(action_intent, "payload_sha256"));
   cJSON_AddItemToObject(request, "outcome", outcome);
   cJSON *decision = action_store(action_principal, action_session, request);
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(decision, "allowed")))
      state = "outcome_unknown";
   cJSON *envelope = cJSON_CreateObject();
   cJSON_AddStringToObject(envelope, "action_id", action_text(action_intent, "action_id"));
   cJSON_AddStringToObject(envelope, "status", state);
   cJSON_AddStringToObject(envelope, "completion_claim",
                           strcmp(state, "acknowledged") == 0 ? "provider_acknowledged" : state);
   cJSON_AddStringToObject(envelope, "tool_result", result ? result : "");
   char *wrapped = cJSON_PrintUnformatted(envelope);
   free(result);
   cJSON_Delete(envelope);
   cJSON_Delete(request);
   cJSON_Delete(decision);
   cJSON_Delete(action_intent);
   action_intent = NULL;
   return wrapped ? wrapped : safe_strdup("error: action outcome unknown; reconcile before retry");
}

/* Reconciliation accepts only an owned receipt and material to be checked by
 * the resource owner. Public JSON cannot submit an outcome or object version. */
cJSON *action_receipt_command(cJSON *input)
{
   const char *principal = request_context_principal();
   const char *sid = action_text(input, "session_id");
   const char *id = action_text(input, "action_id");
   const char *operation = action_text(input, "operation");
   if (!principal || !principal[0] || !sid[0] || !id[0] ||
       (strcmp(operation, "inspect") && strcmp(operation, "reconcile") &&
        strcmp(operation, "cancel")))
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "owned session, action and operation required", NULL);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "inspect");
   cJSON_AddStringToObject(request, "action_id", id);
   cJSON *result = action_store(principal, sid, request);
   cJSON_Delete(request);
   if (!result)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "owned action receipt unavailable",
                                    NULL);
   if (!strcmp(operation, "inspect"))
      return result;
   const cJSON *receipt = cJSON_GetObjectItemCaseSensitive(result, "receipt");
   const cJSON *intent = cJSON_GetObjectItemCaseSensitive(receipt, "intent");
   const char *state = action_text(receipt, "state");
   if (!strcmp(operation, "cancel"))
   {
      request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "operation", "cancel");
      cJSON_AddStringToObject(request, "registry_class", action_text(intent, "action_class"));
      cJSON_AddItemToObject(request, "intent", cJSON_Duplicate(intent, 1));
      cJSON_Delete(result);
      result = action_store(principal, sid, request);
      cJSON_Delete(request);
      return result ? result
                    : server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                             "action cancellation unavailable", NULL);
   }

   if (!strcmp(state, "effect_confirmed"))
      return result;
   if (strcmp(state, "dispatching") && strcmp(state, "outcome_unknown") &&
       strcmp(state, "acknowledged"))
   {
      cJSON_Delete(result);
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "action has no dispatched effect to reconcile", NULL);
   }
   cJSON *arguments = cJSON_ParseWithOpts(action_text(input, "arguments_json"), NULL, 1);
   cJSON *probe = cJSON_CreateObject(), *caller = cJSON_CreateObject();
   cJSON_AddBoolToObject(caller, "authenticated", 1);
   cJSON_AddStringToObject(caller, "principal", principal);
   cJSON_AddStringToObject(probe, "tool", action_text(intent, "tool"));
   cJSON_AddStringToObject(probe, "directory", action_text(input, "directory"));
   cJSON_AddStringToObject(probe, "destination", action_text(intent, "destination"));
   cJSON_AddStringToObject(probe, "payload_sha256", action_text(intent, "payload_sha256"));
   cJSON_AddItemToObject(probe, "arguments", arguments ? arguments : cJSON_CreateNull());
   cJSON *verification = aimee_module_command_call_context(AIMEE_TOOLS_EVENT_ACTION_RESOURCE,
                                                           AIMEE_TOOLS_STAGE_ACTION_RESOURCE,
                                                           "verify", probe, caller);
   cJSON_Delete(probe);
   cJSON_Delete(caller);
   if (strcmp(action_text(verification, "state"), "effect_confirmed") ||
       strcmp(action_text(verification, "destination"), action_text(intent, "destination")) ||
       strcmp(action_text(verification, "payload_sha256"), action_text(intent, "payload_sha256")) ||
       !action_text(verification, "object_version")[0])
   {
      cJSON_Delete(verification);
      cJSON_AddStringToObject(result, "reconciliation",
                              "exact effect unverified; receipt unchanged");
      return result;
   }
   request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "reconcile");
   cJSON_AddStringToObject(request, "registry_class", action_text(intent, "action_class"));
   cJSON_AddItemToObject(request, "intent", cJSON_Duplicate(intent, 1));
   cJSON *outcome = cJSON_CreateObject();
   const char *fields[] = {"state", "destination", "payload_sha256", "object_version"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      cJSON_AddStringToObject(outcome, fields[i], action_text(verification, fields[i]));
   cJSON_AddStringToObject(outcome, "evidence_ref", action_text(verification, "object_version"));
   cJSON_AddItemToObject(request, "outcome", outcome);
   cJSON_Delete(verification);
   cJSON_Delete(result);
   result = action_store(principal, sid, request);
   cJSON_Delete(request);
   if (result)
      cJSON_AddStringToObject(result, "completion_claim",
                              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "allowed"))
                                  ? "effect_confirmed"
                                  : "outcome_unknown");
   return result ? result
                 : server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                          "reconciliation owner unavailable", NULL);
}

int handle_action_receipt(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, action_receipt_command(req));
}
