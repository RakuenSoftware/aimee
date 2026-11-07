#include "server_unknown_effect_policy.h"
#include "server_native_history.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "request_context.h"
#include "agent_config.h"
#include "aimee_sha256.h"
#include "util.h"
#include <aimee/tools/module_api.h>
#include <aimee/tools/agent_tools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int refuse(char *reason, size_t cap, const char *detail)
{
   if (reason && cap)
      snprintf(reason, cap, "%s", detail);
   return -1;
}
static int emit(const char *id, aimee_session_event_kind_t kind, const cJSON *payload)
{
   aimee_session_event_t event = {
       .kind = kind, .request_id = id, .control_kind = "permission", .control = payload};
   return !aimee_ir_session_get_sink(NULL) || aimee_ir_session_emit(&event);
}
/* A complete digest binds authority. The bounded references are display only;
 * omitted old inputs never become an empty or smaller authority set. */
static cJSON *permission_display(const cJSON *action)
{
   const cJSON *receipts = cJSON_GetObjectItemCaseSensitive(action, "unknown_receipts");
   cJSON *canonical = aimee_ir_json_canonical_copy(receipts);
   char *wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   char digest[65];
   int valid = wire && !aimee_sha256_hex(wire, strlen(wire), digest);
   free(wire);
   if (!valid)
      return NULL;
   cJSON *display = cJSON_Duplicate(action, 1), *summary = cJSON_CreateObject();
   if (!display || !summary)
   {
      cJSON_Delete(display);
      cJSON_Delete(summary);
      return NULL;
   }
   cJSON *refs = cJSON_AddArrayToObject(summary, "sources");
   if (!refs || !cJSON_AddStringToObject(summary, "sha256", digest) ||
       !cJSON_AddNumberToObject(summary, "count", cJSON_GetArraySize(receipts)))
      goto fail;
   const cJSON *entry;
   int shown = 0;
   cJSON_ArrayForEach(entry, receipts)
   {
      if (shown == 4)
         break;
      const cJSON *receipt = cJSON_GetObjectItemCaseSensitive(entry, "receipt"),
                  *source = cJSON_GetObjectItemCaseSensitive(receipt, "source");
      cJSON *ref = cJSON_CreateObject();
      const char *keys[] = {"session_id", "turn_id", "sequence", "tool_id", "name"};
      int ok =
          ref && cJSON_AddStringToObject(
                     ref, "event_id",
                     cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "event_id")));
      for (unsigned i = 0; i < sizeof keys / sizeof keys[0] && ok; i++)
      {
         const char *value =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, keys[i]));
         ok = value && cJSON_AddStringToObject(ref, keys[i], value);
      }
      if (!ok)
      {
         cJSON_Delete(ref);
         goto fail;
      }
      if (!cJSON_AddItemToArray(refs, ref))
      {
         cJSON_Delete(ref);
         goto fail;
      }
      char *bounded = cJSON_PrintUnformatted(summary);
      int fits = bounded && strlen(bounded) <= 16384;
      free(bounded);
      if (!fits)
      {
         cJSON_DeleteItemFromArray(refs, shown);
         break;
      }
      shown++;
   }
   if (!cJSON_AddNumberToObject(summary, "displayed", shown) ||
       !cJSON_ReplaceItemInObjectCaseSensitive(display, "unknown_receipts", summary))
      goto fail;
   return display;
fail:
   cJSON_Delete(display);
   cJSON_Delete(summary);
   return NULL;
}

static _Thread_local char approved_action[65];
static _Thread_local int final_credit;
int server_unknown_effect_policy_stage(const char *tool, const char *arguments, const char *attempt,
                                       uint32_t categories, int final_stage, char *reason,
                                       size_t cap)
{
   char prior_action[65] = "";
   if (final_stage == 1 && final_credit)
      snprintf(prior_action, sizeof prior_action, "%s", approved_action);
   approved_action[0] = 0;
   final_credit = 0;
   if (final_stage == 2)
      return 0;
   if (final_stage != 0 && final_stage != 1)
      return -1;
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->model_execution)
      return 0;
   /* Only registry-owned read metadata can omit this extra mutation gate. */
   if ((categories & AIMEE_TOOL_POLICY_READ) &&
       !(categories & (AIMEE_TOOL_POLICY_WRITE | AIMEE_TOOL_POLICY_EXECUTION |
                       AIMEE_TOOL_POLICY_CONTROL | AIMEE_TOOL_POLICY_EXTERNAL)))
      return 0;
   server_session_capture_t *capture = server_session_capture_current();
   const char *session = server_session_capture_session(capture),
              *turn = server_session_capture_turn(capture);
   if (!session || !turn || !ctx->vault_principal[0] || !tool || !arguments)
      return refuse(reason, cap, "unknown-effect history authority unavailable");
   if (agent_request_cancelled() || ctx->context_refused ||
       !server_session_capture_accepting(capture))
      return refuse(reason, cap, "unknown-effect action owner is no longer live");
   cJSON *messages = NULL, *receipts = NULL;
   if (server_native_history_load(session, ctx->vault_principal, turn, 0, &messages, &receipts))
      return refuse(reason, cap, "unknown-effect history could not be verified");
   cJSON_Delete(messages);
   messages = NULL;
   if (!server_native_history_requires_permission(receipts))
   {
      cJSON_Delete(receipts);
      return 0;
   }
   if (!attempt || !attempt[0] || !ctx->request_id[0])
   {
      cJSON_Delete(receipts);
      return refuse(reason, cap, "unknown-effect retry requires a host action identity");
   }
   server_control_scope_t *scope = NULL;
   if (server_control_scope_capture(session, ctx->vault_principal, &scope) || !scope)
   {
      cJSON_Delete(receipts);
      return refuse(reason, cap, "unknown-effect permission owner unavailable");
   }
   int result = -1, published = 0;
   cJSON *action = cJSON_CreateObject(),
         *args = aimee_ir_json_parse_frame(arguments, strlen(arguments));
   cJSON *request = NULL, *reply = NULL;
   char *wire = NULL, *taken_id = NULL, *kind = NULL;
   cJSON *original = NULL;
   char digest[65], id[96];
   if (server_control_unknown_effect_denied(scope))
      goto finish;
   /* The governed owner resolves the actual resource and arguments before its
    * final policy gate. Ask there once, rather than approve a relative spelling
    * here and then ask again for the canonical action. Baseline authorization,
    * source verification and sticky denial still run before this deferral. */
   if (!final_stage && cJSON_IsObject(args) && agent_tools_action_managed(tool, args))
   {
      result = 0;
      goto finish;
   }
   if (!action || !args || !cJSON_AddStringToObject(action, "session_id", session) ||
       !cJSON_AddStringToObject(action, "turn_id", turn) ||
       !cJSON_AddStringToObject(action, "actor", ctx->vault_principal) ||
       !cJSON_AddStringToObject(action, "request_id", ctx->request_id) ||
       !cJSON_AddStringToObject(action, "attempt_id", attempt) ||
       !cJSON_AddStringToObject(action, "tool", tool) ||
       !cJSON_AddItemToObject(action, "arguments", args))
      goto finish;
   args = NULL;
   if (!cJSON_AddItemToObject(action, "unknown_receipts", receipts))
      goto finish;
   receipts = NULL;
   cJSON *canonical = aimee_ir_json_canonical_copy(action);
   wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   if (!wire || aimee_sha256_hex(wire, strlen(wire), digest))
      goto finish;
   if (final_stage)
   {
      int same = prior_action[0] && !strcmp(prior_action, digest);
      approved_action[0] = 0;
      final_credit = 0;
      if (same)
      {
         result = 0;
         goto finish;
      }
   }
   snprintf(id, sizeof id, "unknown-effect:%s", digest);
   request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "tool", tool) ||
       !cJSON_AddStringToObject(
           request, "description",
           final_stage && prior_action[0]
               ? "The action or earlier uncertain outcomes changed during preparation. "
                 "Allow the exact action below once? Earlier effects remain unknown."
               : "An earlier tool's effects are unknown. Allow this requested action once? "
                 "This does not confirm or retry the earlier action automatically.") ||
       !cJSON_AddItemToObject(request, "input", permission_display(action)))
      goto finish;
   if (server_control_reserve(scope, id, "permission", request))
      goto finish;
   if (emit(id, AIMEE_SESSION_CONTROL_REQUEST, request))
      goto abandon;
   published = 1;
   struct timespec start, now;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
      goto abandon;
   for (;;)
   {
      if (agent_request_cancelled() || server_control_scope_failed(scope) ||
          clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - start.tv_sec >= 300)
         goto abandon;
      int taken = server_control_take(scope, id, &taken_id, &kind, &original, &reply);
      if (taken < 0)
         goto abandon;
      if (taken > 0)
      {
         const char *decision =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "decision"));
         if (!emit(id, AIMEE_SESSION_CONTROL_RESPONSE, reply) && decision &&
             !strcmp(decision, "allow") && !agent_request_cancelled())
         {
            result = 0;
            if (!final_stage)
            {
               snprintf(approved_action, sizeof approved_action, "%s", digest);
               final_credit = 1;
            }
         }
         if (result)
            server_control_deny_unknown_effects(scope);
         goto finish;
      }
      const struct timespec tick = {.tv_nsec = 20000000};
      nanosleep(&tick, NULL);
   }
abandon:
   server_control_deny_unknown_effects(scope);
   if (!server_control_abandon(scope, id) && published)
   {
      cJSON *cancel = cJSON_CreateObject();
      if (cancel && cJSON_AddStringToObject(cancel, "decision", "cancel"))
         emit(id, AIMEE_SESSION_CONTROL_RESPONSE, cancel);
      cJSON_Delete(cancel);
   }
finish:
   if (result)
      server_control_deny_unknown_effects(scope);
   cJSON_Delete(action);
   cJSON_Delete(args);
   cJSON_Delete(messages);
   cJSON_Delete(receipts);
   cJSON_Delete(request);
   cJSON_Delete(reply);
   cJSON_Delete(original);
   free(wire);
   free(taken_id);
   free(kind);
   server_control_scope_release(scope);
   return result ? refuse(reason, cap, "explicit permission required after an unknown tool outcome")
                 : 0;
}

int server_unknown_effect_policy(const char *tool, const char *arguments, const char *attempt,
                                 uint32_t categories, char *reason, size_t cap)
{
   return server_unknown_effect_policy_stage(tool, arguments, attempt, categories, 0, reason, cap);
}

int server_native_history_admit_current(const cJSON *messages)
{
   server_session_capture_t *capture = server_session_capture_current();
   const request_context_t *ctx = request_context_get();
   const char *session = capture ? server_session_capture_session(capture) : session_id();
   const char *turn = capture ? server_session_capture_turn(capture) : "";
   const char *actor =
       ctx && ctx->vault_principal[0] ? ctx->vault_principal : agent_get_request_vault_principal();
   return server_native_history_admit_source(session, actor, turn, capture != NULL, messages);
}
