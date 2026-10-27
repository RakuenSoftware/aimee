#include <aimee/translation/harness_ir.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str(const cJSON *o, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}
/* Public controls use one question schema; provider-specific request fields
 * remain opaque tool input and are interpreted only by the native renderer. */
static cJSON *control_payload(const char *tool, const cJSON *input, const cJSON *questions)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON_AddStringToObject(out, "tool_name", tool);
   cJSON_AddStringToObject(out, "title", tool);
   cJSON_AddItemToObject(out, "input", input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject());
   if (cJSON_IsArray(questions))
   {
      cJSON *list = cJSON_AddArrayToObject(out, "questions");
      const cJSON *q;
      cJSON_ArrayForEach(q, questions)
      {
         const char *id = str(q, "id");
         if (!id[0])
            id = str(q, "question");
         cJSON *entry = cJSON_CreateObject();
         if (!entry)
         {
            cJSON_Delete(out);
            return NULL;
         }
         cJSON_AddStringToObject(entry, "id", id);
         cJSON_AddStringToObject(entry, "question", str(q, "question"));
         cJSON_AddBoolToObject(entry, "multiple",
                               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "multiSelect")));
         const cJSON *options = cJSON_GetObjectItemCaseSensitive(q, "options");
         cJSON_AddItemToObject(entry, "options",
                               cJSON_IsArray(options) ? cJSON_Duplicate(options, 1)
                                                      : cJSON_CreateArray());
         cJSON_AddItemToArray(list, entry);
      }
   }
   return out;
}
void aimee_harness_ir_free(aimee_harness_ir_t *s)
{
   if (!s)
      return;
   cJSON_Delete(s->pending);
   memset(s, 0, sizeof(*s));
}
static int native_compaction(const char *harness)
{
   aimee_session_event_t event = {.kind = AIMEE_SESSION_NATIVE_COMPACTION,
                                  .native_harness = harness};
   return aimee_ir_session_emit(&event);
}
static int aimee_claude_frame_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   if (!cJSON_IsObject(f) || !s)
      return -1;
   const char *type = str(f, "type");
   if (!strcmp(type, "assistant"))
   {
      s->failure = AIMEE_HARNESS_FAILURE_NONE;
      const cJSON *flag = cJSON_GetObjectItemCaseSensitive(f, "is_api_error_message");
      const cJSON *error = cJSON_GetObjectItemCaseSensitive(f, "error");
      if (cJSON_IsTrue(flag) || error)
      {
         const cJSON *message = cJSON_GetObjectItemCaseSensitive(f, "message");
         if (!cJSON_IsObject(message) || strcmp(str(message, "role"), "assistant"))
            return -1;
         if (!cJSON_IsTrue(flag))
         {
            /* The 2.1.177 print serializer retains error but omits its internal
             * diagnostic flag. Only its main-assistant envelope is admitted;
             * persisted camel-case records and contradictory markers are not. */
            const cJSON *parent = cJSON_GetObjectItemCaseSensitive(f, "parent_tool_use_id");
            if (flag || cJSON_GetObjectItemCaseSensitive(f, "isApiErrorMessage") ||
                !cJSON_IsString(error) || !error->valuestring[0] ||
                strlen(error->valuestring) > 128 || !cJSON_IsNull(parent) ||
                !str(f, "session_id")[0] || strlen(str(f, "session_id")) > 1024 ||
                !str(f, "uuid")[0] || strlen(str(f, "uuid")) > 1024)
               return -1;
         }
         /* Reserved SDK diagnostics are never canonical model observations.
          * Unknown codes stay generic; diagnostic text is never authority. */
         if (!strcmp(str(f, "error"), "rate_limit"))
            s->failure = AIMEE_HARNESS_FAILURE_RATE_LIMIT;
         else if (!strcmp(str(f, "error"), "invalid_request"))
            s->failure = AIMEE_HARNESS_FAILURE_PROVIDER_REJECTED;
         return 0;
      }
   }
   else if ((!strcmp(type, "result") &&
             cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(f, "is_error"))) ||
            (!strcmp(type, "stream_event") &&
             !strcmp(str(cJSON_GetObjectItemCaseSensitive(f, "event"), "type"), "message_start")))
      s->failure = AIMEE_HARNESS_FAILURE_NONE;
   if (!strcmp(type, "stream_event") || !strcmp(type, "assistant") || !strcmp(type, "user") ||
       !strcmp(type, "result"))
      return 0; /* Original host capture owns model/tool/history observations. */

   if (!strcmp(type, "system") && !strcmp(str(f, "subtype"), "compact_boundary"))
      return native_compaction("claude");
   if (!strcmp(type, "control_request") || !strcmp(type, "control_response"))
   {
      int incoming = !strcmp(type, "control_request");
      const cJSON *payload = cJSON_GetObjectItemCaseSensitive(f, incoming ? "request" : "response");
      const char *id = str(f, "request_id");
      if (!id[0])
         id = str(payload, "request_id");
      if (!id[0])
         return -1;
      if (!s->pending)
         s->pending = cJSON_CreateObject();
      if (!s->pending)
         return -1;
      const char *kind;
      if (incoming)
      {
         if (strcmp(str(payload, "subtype"), "can_use_tool"))
            return 0;
         if (cJSON_GetObjectItemCaseSensitive(s->pending, id))
            return -1;
         kind = !strcmp(str(payload, "tool_name"), "AskUserQuestion") ? "question" : "permission";
         if (!cJSON_AddStringToObject(s->pending, id, kind))
            return -1;
      }
      else
      {
         kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s->pending, id));
         if (!kind)
            return 0; /* host initialization acknowledgement */
      }
      const cJSON *input = cJSON_GetObjectItemCaseSensitive(payload, "input");
      cJSON *normalized =
          incoming ? control_payload(str(payload, "tool_name"), input,
                                     !strcmp(kind, "question")
                                         ? cJSON_GetObjectItemCaseSensitive(input, "questions")
                                         : NULL)
                   : NULL;
      aimee_session_event_t e = {.kind = incoming ? AIMEE_SESSION_CONTROL_REQUEST
                                                  : AIMEE_SESSION_CONTROL_RESPONSE,
                                 .request_id = id,
                                 .control_kind = kind,
                                 .control = incoming ? normalized : payload};
      int rc = incoming && !normalized ? -1 : aimee_ir_session_emit(&e);
      cJSON_Delete(normalized);
      if (!incoming)
         cJSON_DeleteItemFromObjectCaseSensitive(s->pending, id);
      return rc;
   }
   return 0; /* init, result and lifecycle metadata are owned by the host */
}

static int aimee_codex_frame_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   if (!cJSON_IsObject(f) || !s)
      return -1;
   const char *method = str(f, "method");
   if (!strcmp(method, "thread/compacted"))
      return native_compaction("codex");
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(f, "params");
   if (!strcmp(method, "item/agentMessage/delta"))
      return 0; /* The original gateway capture owns model observations. */
   const cJSON *rpcid = cJSON_GetObjectItemCaseSensitive(f, "id");
   if (rpcid &&
       (strstr(method, "requestApproval") || !strcmp(method, "item/tool/requestUserInput") ||
        !strcmp(method, "mcpServer/elicitation/request") ||
        !strcmp(method, "execCommandApproval") || !strcmp(method, "applyPatchApproval")))
   {
      char *id = cJSON_PrintUnformatted(rpcid);
      if (!id)
         return -1;
      if (!s->pending)
         s->pending = cJSON_CreateObject();
      if (!s->pending)
      {
         free(id);
         return -1;
      }
      const char *kind = !strcmp(method, "item/tool/requestUserInput")      ? "question"
                         : !strcmp(method, "mcpServer/elicitation/request") ? "elicitation"
                                                                            : "permission";
      if (cJSON_GetObjectItemCaseSensitive(s->pending, id) ||
          !cJSON_AddStringToObject(s->pending, id, kind))
      {
         free(id);
         return -1;
      }
      cJSON *normalized =
          control_payload(method, params, cJSON_GetObjectItemCaseSensitive(params, "questions"));
      if (normalized && !strcmp(kind, "elicitation") && !strcmp(str(params, "mode"), "form"))
      {
         cJSON_ReplaceItemInObjectCaseSensitive(normalized, "title",
                                                cJSON_CreateString("Information requested"));
         cJSON *form = cJSON_AddObjectToObject(normalized, "form");
         const cJSON *schema = cJSON_GetObjectItemCaseSensitive(params, "requestedSchema");
         if (!form || !cJSON_AddStringToObject(form, "message", str(params, "message")) ||
             !cJSON_AddStringToObject(form, "server", str(params, "serverName")) ||
             !cJSON_AddItemToObject(form, "schema", cJSON_Duplicate(schema, 1)))
         {
            cJSON_Delete(normalized);
            normalized = NULL;
         }
      }
      if (normalized && !strcmp(kind, "elicitation"))
      {
         /* Native correlation stays at the adapter. Browser controls consume only
          * the normalized form/URL contract, never the native parameter frame. */
         cJSON_DeleteItemFromObjectCaseSensitive(normalized, "input");
         if (!strcmp(str(params, "mode"), "url"))
         {
            cJSON *url = cJSON_AddObjectToObject(normalized, "url");
            cJSON_ReplaceItemInObjectCaseSensitive(
                normalized, "title", cJSON_CreateString("External interaction requested"));
            if (!url || !cJSON_AddStringToObject(url, "href", str(params, "url")) ||
                !cJSON_AddStringToObject(url, "message", str(params, "message")) ||
                !cJSON_AddStringToObject(url, "server", str(params, "serverName")) ||
                !cJSON_AddStringToObject(url, "elicitation_id", str(params, "elicitationId")))
            {
               cJSON_Delete(normalized);
               normalized = NULL;
            }
         }
      }
      aimee_session_event_t e = {.kind = AIMEE_SESSION_CONTROL_REQUEST,
                                 .request_id = id,
                                 .control_kind = kind,
                                 .control = normalized};
      int rc = normalized ? aimee_ir_session_emit(&e) : -1;
      cJSON_Delete(normalized);
      free(id);
      return rc;
   }
   if (strcmp(method, "item/started") && strcmp(method, "item/completed"))
      return 0;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "item");
   const char *type = str(item, "type"), *id = str(item, "id");
   if (!id[0] || !type[0])
      return -1;
   if (!strcmp(type, "agentMessage") || !strcmp(type, "userMessage") ||
       !strcmp(type, "reasoning") || !strcmp(type, "plan") || !strcmp(type, "commandExecution") ||
       !strcmp(type, "mcpToolCall") || !strcmp(type, "dynamicToolCall") ||
       !strcmp(type, "fileChange") || !strcmp(type, "webSearch") || !strcmp(type, "imageView"))
      return 0;
   if (!strcmp(type, "contextCompaction"))
      return native_compaction("codex");
   if (!strcmp(type, "enteredReviewMode") || !strcmp(type, "exitedReviewMode"))
      return 0;
   return -1; /* Unsupported item kinds retain the existing refusal. */
}

static int aimee_codex_reply_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   const cJSON *rpcid = cJSON_GetObjectItemCaseSensitive(f, "id");
   if (rpcid && s->pending)
   {
      char *id = cJSON_PrintUnformatted(rpcid);
      if (!id)
         return -1;
      if (cJSON_GetObjectItemCaseSensitive(s->pending, id))
      {
         const cJSON *result = cJSON_GetObjectItemCaseSensitive(f, "result");
         if (!result)
            result = cJSON_GetObjectItemCaseSensitive(f, "error");
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s->pending, id));
         cJSON *normalized = NULL;
         if (kind && !strcmp(kind, "elicitation"))
         {
            const char *action = str(result, "action");
            const char *decision = !strcmp(action, "accept")    ? "allow"
                                   : !strcmp(action, "decline") ? "deny"
                                   : !strcmp(action, "cancel")  ? "cancel"
                                                                : NULL;
            normalized = cJSON_CreateObject();
            if (!decision || !normalized ||
                !cJSON_AddStringToObject(normalized, "decision", decision))
            {
               cJSON_Delete(normalized);
               free(id);
               return -1;
            }
            const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
            if (!strcmp(decision, "allow") && content && !cJSON_IsNull(content) &&
                !cJSON_AddItemToObject(normalized, "content", cJSON_Duplicate(content, 1)))
            {
               cJSON_Delete(normalized);
               free(id);
               return -1;
            }
         }
         aimee_session_event_t e = {
             .kind = AIMEE_SESSION_CONTROL_RESPONSE,
             .request_id = id,
             .control_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s->pending, id)),
             .control = normalized ? normalized : result};
         int rc = aimee_ir_session_emit(&e);
         cJSON_Delete(normalized);
         cJSON_DeleteItemFromObjectCaseSensitive(s->pending, id);
         free(id);
         return rc;
      }
      free(id);
   }
   return 0;
}

int aimee_claude_frame_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = aimee_claude_frame_ir_decode(frame, state);
   if (rc < 0)
      aimee_ir_session_fail_capture();
   return rc;
}

int aimee_codex_frame_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = aimee_codex_frame_ir_decode(frame, state);
   if (rc < 0)
      aimee_ir_session_fail_capture();
   return rc;
}

int aimee_codex_reply_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = aimee_codex_reply_ir_decode(frame, state);
   if (rc < 0)
      aimee_ir_session_fail_capture();
   return rc;
}

static int operator_allowed(const cJSON *decision)
{
   const char *value = str(decision, "decision");
   return !strcmp(value, "allow")                                ? 1
          : (!strcmp(value, "deny") || !strcmp(value, "cancel")) ? 0
                                                                 : -1;
}
int aimee_harness_control_decision_ir(aimee_harness_ir_t *state, const char *id,
                                      const cJSON *decision)
{
   const char *kind =
       state && id ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state->pending, id))
                   : NULL;
   if (!kind || !cJSON_IsObject(decision) || operator_allowed(decision) < 0)
      return -1;
   aimee_session_event_t event = {.kind = AIMEE_SESSION_CONTROL_RESPONSE,
                                  .request_id = id,
                                  .control_kind = kind,
                                  .control = decision};
   int rc = aimee_ir_session_emit(&event);
   if (!rc)
      cJSON_DeleteItemFromObjectCaseSensitive(state->pending, id);
   else
      aimee_ir_session_fail_capture();
   return rc;
}

static int answer_strings(const cJSON *answers)
{
   if (!cJSON_IsArray(answers) || !cJSON_GetArraySize(answers))
      return 0;
   const cJSON *value;
   cJSON_ArrayForEach(value, answers) if (!cJSON_IsString(value)) return 0;
   return 1;
}

cJSON *aimee_claude_control_reply(const char *id, const cJSON *request, const cJSON *decision)
{
   int allow = operator_allowed(decision);
   if (!id || !id[0] || allow < 0 || !str(request, "tool_name")[0])
      return NULL;
   cJSON *frame = cJSON_CreateObject(), *response = cJSON_CreateObject(),
         *result = cJSON_CreateObject();
   if (!frame || !response || !result)
   {
      cJSON_Delete(frame);
      cJSON_Delete(response);
      cJSON_Delete(result);
      return NULL;
   }
   cJSON_AddStringToObject(frame, "type", "control_response");
   cJSON_AddStringToObject(response, "subtype", "success");
   cJSON_AddStringToObject(response, "request_id", id);
   cJSON_AddItemToObject(frame, "response", response);
   cJSON_AddItemToObject(response, "response", result);
   cJSON_AddStringToObject(result, "behavior", allow ? "allow" : "deny");
   if (!allow)
   {
      cJSON_AddStringToObject(result, "message", "Operator declined the request");
      return frame;
   }
   const cJSON *original = cJSON_GetObjectItemCaseSensitive(request, "input");
   cJSON *input = cJSON_IsObject(original) ? cJSON_Duplicate(original, 1) : NULL;
   if (!input)
   {
      cJSON_Delete(frame);
      return NULL;
   }
   cJSON_AddItemToObject(result, "updatedInput", input);
   const cJSON *answers = cJSON_GetObjectItemCaseSensitive(decision, "answers");
   if (!strcmp(str(request, "tool_name"), "AskUserQuestion"))
   {
      const cJSON *questions = cJSON_GetObjectItemCaseSensitive(original, "questions");
      if (!cJSON_IsArray(questions) || !cJSON_IsObject(answers))
      {
         cJSON_Delete(frame);
         return NULL;
      }
      cJSON *mapped = cJSON_CreateObject();
      if (!mapped)
      {
         cJSON_Delete(frame);
         return NULL;
      }
      cJSON_DeleteItemFromObjectCaseSensitive(input, "answers");
      cJSON_AddItemToObject(input, "answers", mapped);
      const cJSON *question;
      cJSON_ArrayForEach(question, questions)
      {
         const char *key = str(question, "question");
         const cJSON *values = cJSON_GetObjectItemCaseSensitive(answers, key);
         if (!key[0] || !answer_strings(values))
         {
            cJSON_Delete(frame);
            return NULL;
         }
         size_t size = 1;
         const cJSON *value;
         cJSON_ArrayForEach(value, values) size += strlen(value->valuestring) + 2;
         char *joined = calloc(size, 1);
         if (!joined)
         {
            cJSON_Delete(frame);
            return NULL;
         }
         cJSON_ArrayForEach(value, values)
         {
            if (joined[0])
               strcat(joined, ", ");
            strcat(joined, value->valuestring);
         }
         cJSON_AddStringToObject(mapped, key, joined);
         free(joined);
      }
   }
   else if (answers)
   {
      cJSON_Delete(frame);
      return NULL;
   }
   return frame;
}

cJSON *aimee_codex_control_reply(const char *method, const cJSON *params, const cJSON *decision)
{
   int allow = operator_allowed(decision);
   if (!method || allow < 0)
      return NULL;
   cJSON *result = cJSON_CreateObject();
   if (!result)
      return NULL;
   if (!strcmp(method, "item/tool/requestUserInput"))
   {
      cJSON *mapped = cJSON_AddObjectToObject(result, "answers");
      if (!allow)
         return result;
      const cJSON *questions = cJSON_GetObjectItemCaseSensitive(params, "questions");
      const cJSON *answers = cJSON_GetObjectItemCaseSensitive(decision, "answers"), *question;
      if (!mapped || !cJSON_IsArray(questions) || !cJSON_IsObject(answers))
      {
         cJSON_Delete(result);
         return NULL;
      }
      cJSON_ArrayForEach(question, questions)
      {
         const char *id = str(question, "id");
         const cJSON *values = cJSON_GetObjectItemCaseSensitive(answers, id);
         if (!id[0] || !answer_strings(values))
         {
            cJSON_Delete(result);
            return NULL;
         }
         cJSON *entry = cJSON_AddObjectToObject(mapped, id);
         if (!entry)
         {
            cJSON_Delete(result);
            return NULL;
         }
         cJSON_AddItemToObject(entry, "answers", cJSON_Duplicate(values, 1));
      }
   }
   else if (!strcmp(method, "item/permissions/requestApproval"))
   {
      const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(params, "permissions");
      cJSON_AddItemToObject(result, "permissions",
                            allow && cJSON_IsObject(permissions) ? cJSON_Duplicate(permissions, 1)
                                                                 : cJSON_CreateObject());
      cJSON_AddStringToObject(result, "scope", "turn");
   }
   else if (!strcmp(method, "mcpServer/elicitation/request"))
   {
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(decision, "content");
      int form_mode = !strcmp(str(params, "mode"), "form");
      int url_mode = !strcmp(str(params, "mode"), "url");
      cJSON *url = NULL;
      if (allow && url_mode)
      {
         url = cJSON_CreateObject();
         if (url)
         {
            cJSON_AddStringToObject(url, "href", str(params, "url"));
            cJSON_AddStringToObject(url, "message", str(params, "message"));
            cJSON_AddStringToObject(url, "server", str(params, "serverName"));
            cJSON_AddStringToObject(url, "elicitation_id", str(params, "elicitationId"));
         }
      }
      int valid =
          !allow ||
          (form_mode ? aimee_ir_form_validate(
                           cJSON_GetObjectItemCaseSensitive(params, "requestedSchema"), content)
                     : url_mode && !content && aimee_ir_elicitation_url_valid(url));
      cJSON_Delete(url);
      if (!valid)
      {
         cJSON_Delete(result);
         return NULL;
      }
      cJSON_AddStringToObject(result, "action",
                              allow                                          ? "accept"
                              : !strcmp(str(decision, "decision"), "cancel") ? "cancel"
                                                                             : "decline");
      cJSON_AddItemToObject(result, "content",
                            allow && form_mode ? cJSON_Duplicate(content, 1) : cJSON_CreateNull());
      cJSON_AddNullToObject(result, "_meta");
   }
   else if (!strcmp(method, "item/commandExecution/requestApproval") ||
            !strcmp(method, "item/fileChange/requestApproval") ||
            !strcmp(method, "execCommandApproval") || !strcmp(method, "applyPatchApproval"))
   {
      int legacy = !strcmp(method, "execCommandApproval") || !strcmp(method, "applyPatchApproval");
      cJSON_AddStringToObject(result, "decision",
                              allow ? (legacy ? "approved" : "accept")
                                    : (legacy ? "denied" : "decline"));
   }
   else
   {
      cJSON_Delete(result);
      return NULL;
   }
   return result;
}
