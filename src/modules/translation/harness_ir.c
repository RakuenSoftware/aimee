#include <aimee/translation/harness_ir.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
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
static int message(const char *id, const char *role, aimee_block_t *blocks, int count)
{
   aimee_message_t m = {.role = (char *)role, .blocks = blocks, .n_blocks = count};
   aimee_session_event_t e = {.kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &m};
   return aimee_ir_session_emit(&e);
}
static int delta(const char *id, aimee_delta_t *d)
{
   aimee_session_event_t e = {
       .kind = AIMEE_SESSION_DELTA, .message_id = id, .role = "assistant", .delta = d};
   return aimee_ir_session_emit(&e);
}
void aimee_harness_ir_free(aimee_harness_ir_t *s)
{
   if (!s)
      return;
   cJSON_Delete(s->started);
   cJSON_Delete(s->pending);
   cJSON_Delete(s->claude_message);
   cJSON_Delete(s->acp_tools);
   memset(s, 0, sizeof(*s));
}
static int claude_snapshot(const cJSON *m, const char *id)
{
   aimee_response_t ir = {0};
   char error[128];
   if (anthropic_backend_parse(m, &ir, error, sizeof(error)))
      return -1;
   int rc = aimee_ir_session_emit_response(&ir, id);
   aimee_response_free(&ir);
   return rc;
}

static int aimee_claude_frame_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   if (!cJSON_IsObject(f) || !s)
      return -1;
   const char *type = str(f, "type");
   if (!strcmp(type, "stream_event"))
   {
      const cJSON *event = cJSON_GetObjectItemCaseSensitive(f, "event");
      const char *kind = str(event, "type");
      if (!strcmp(kind, "message_start"))
      {
         const char *id = str(cJSON_GetObjectItemCaseSensitive(event, "message"), "id");
         if (!id[0] || strlen(id) >= sizeof(s->message_id))
            return -1;
         snprintf(s->message_id, sizeof(s->message_id), "%s", id);
         anthropic_backend_stream_state_init(&s->anthropic);
         cJSON_Delete(s->claude_message);
         s->claude_message = NULL;
         s->claude_open = 0;
      }
      if (!s->message_id[0])
         return -1;
      aimee_delta_t d = {0};
      int n = anthropic_stream_to_deltas(kind, event, &s->anthropic, &d, 1);
      if (n < 0)
         return -1;
      if (!strcmp(kind, "content_block_start"))
         s->claude_open++;
      if (!strcmp(kind, "content_block_stop"))
         s->claude_open--;
      if (!strcmp(kind, "message_stop") && s->claude_open)
      {
         d = (aimee_delta_t){.type = AIMEE_DELTA_ERROR,
                             .error_message = "Claude response ended with incomplete tool input"};
         return delta(s->message_id, &d);
      }
      if (n && delta(s->message_id, &d))
         return -1;
      if (!strcmp(kind, "message_stop") && s->claude_message)
         return claude_snapshot(s->claude_message, s->message_id);
      return 0;
   }
   if (!strcmp(type, "assistant"))
   {
      const cJSON *m = cJSON_GetObjectItemCaseSensitive(f, "message");
      const char *id = str(m, "id");
      if (!id[0])
         return -1;
      /* Claude emits one assistant snapshot PER BLOCK, sharing a message ID,
       * before that block's stop event. Accumulate it at this native boundary;
       * only publish the complete typed snapshot after the model TURN_STOP. */
      if (s->anthropic.started && !s->anthropic.terminal_emitted && !strcmp(id, s->message_id))
      {
         const cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
         if (!cJSON_IsArray(content))
            return -1;
         if (!s->claude_message)
         {
            s->claude_message = cJSON_Duplicate(m, 1);
            if (!s->claude_message)
               return -1;
         }
         else
         {
            cJSON *all = cJSON_GetObjectItemCaseSensitive(s->claude_message, "content");
            if (cJSON_GetArraySize(all) + cJSON_GetArraySize(content) > 4096)
               return -1;
            const cJSON *part = NULL;
            cJSON_ArrayForEach(part, content)
            {
               cJSON *copy = cJSON_Duplicate(part, 1);
               if (!copy || !cJSON_AddItemToArray(all, copy))
               {
                  cJSON_Delete(copy);
                  return -1;
               }
            }
         }
         return 0;
      }
      return claude_snapshot(m, id);
   }
   if (!strcmp(type, "user"))
   {
      const cJSON *m = cJSON_GetObjectItemCaseSensitive(f, "message");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
      int tools = 0;
      const cJSON *b = NULL;
      cJSON_ArrayForEach(b, content) if (!strcmp(str(b, "type"), "tool_result")) tools++;
      if (!tools)
         return 0; /* admitted prompt has one capture owner, the session */
      cJSON *req = cJSON_CreateObject(), *messages = cJSON_CreateArray();
      cJSON_AddStringToObject(req, "model", "harness");
      cJSON_AddNumberToObject(req, "max_tokens", 1);
      cJSON_AddItemToArray(messages, cJSON_Duplicate(m, 1));
      cJSON_AddItemToObject(req, "messages", messages);
      aimee_request_t ir = {0};
      char error[128], id[64];
      snprintf(id, sizeof(id), "claude-tool-results-%lu", ++s->counter);
      int rc = anthropic_frontend_parse(req, &ir, error, sizeof(error));
      if (!rc && ir.n_messages == 1)
      {
         aimee_session_event_t e = {
             .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &ir.messages[0]};
         rc = aimee_ir_session_emit(&e);
      }
      else
         rc = -1;
      aimee_request_free(&ir);
      cJSON_Delete(req);
      return rc;
   }
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

static int codex_text(const cJSON *item, const char *id, const char *role)
{
   const char *type = str(item, "type");
   if (!strcmp(type, "agentMessage") || !strcmp(type, "plan"))
   {
      aimee_block_t b = {.type = AIMEE_BLK_TEXT, .text = (char *)str(item, "text")};
      return message(id, role, &b, 1);
   }
   const cJSON *content =
       cJSON_GetObjectItemCaseSensitive(item, !strcmp(type, "reasoning") ? "summary" : "content");
   if (!cJSON_IsArray(content))
      return -1;
   int count = cJSON_GetArraySize(content);
   if (count > 4096)
      return -1;
   aimee_block_t *blocks = calloc(count ? count : 1, sizeof(*blocks));
   if (!blocks)
      return -1;
   const cJSON *part = NULL;
   int i = 0, rc = 0;
   cJSON_ArrayForEach(part, content)
   {
      aimee_block_t *b = &blocks[i++];
      const char *kind = str(part, "type");
      if (!strcmp(type, "reasoning"))
      {
         b->type = AIMEE_BLK_THINKING;
         b->text = (char *)(cJSON_IsString(part) ? part->valuestring : str(part, "text"));
      }
      else if (!strcmp(kind, "text"))
      {
         b->type = AIMEE_BLK_TEXT;
         b->text = (char *)str(part, "text");
      }
      else if (!strcmp(kind, "image") || !strcmp(kind, "localImage"))
      {
         b->type = AIMEE_BLK_IMAGE;
         b->media_type = "image/*";
         b->media_ref = (char *)str(part, !strcmp(kind, "image") ? "url" : "path");
      }
      else
      {
         rc = -1;
         break;
      }
   }
   if (!rc)
      rc = message(id, role, blocks, count);
   free(blocks);
   return rc;
}
static int aimee_codex_frame_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   if (!cJSON_IsObject(f) || !s)
      return -1;
   const char *method = str(f, "method");
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(f, "params");
   if (!strcmp(method, "item/agentMessage/delta"))
   {
      const char *id = str(params, "itemId"), *text = str(params, "delta");
      if (!id[0])
         return -1;
      if (!s->started)
         s->started = cJSON_CreateObject();
      if (!s->started)
         return -1;
      if (!cJSON_GetObjectItemCaseSensitive(s->started, id))
      {
         aimee_delta_t d = {.type = AIMEE_DELTA_TURN_START};
         if (delta(id, &d))
            return -1;
         d.type = AIMEE_DELTA_BLOCK_START;
         d.kind = AIMEE_BLK_TEXT;
         if (delta(id, &d))
            return -1;
         if (!cJSON_AddTrueToObject(s->started, id))
            return -1;
      }
      aimee_delta_t d = {
          .type = AIMEE_DELTA_BLOCK_DELTA, .kind = AIMEE_BLK_TEXT, .text_delta = text};
      return delta(id, &d);
   }
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
   int complete = !strcmp(method, "item/completed");
   if (!strcmp(type, "agentMessage") || !strcmp(type, "userMessage") ||
       !strcmp(type, "reasoning") || !strcmp(type, "plan"))
   {
      if (!strcmp(type, "userMessage") && complete && s->admitted_prompt && !s->user_echo_seen)
      {
         const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
         const cJSON *first = cJSON_GetArrayItem(content, 0);
         if (cJSON_GetArraySize(content) == 1 && !strcmp(str(first, "type"), "text") &&
             !strcmp(str(first, "text"), s->admitted_prompt))
         {
            s->user_echo_seen = 1;
            return 0;
         }
      }
      if (!complete)
         return 0;
      if (s->started && cJSON_GetObjectItemCaseSensitive(s->started, id))
      {
         aimee_delta_t d = {.type = AIMEE_DELTA_BLOCK_STOP, .kind = AIMEE_BLK_TEXT};
         if (delta(id, &d))
            return -1;
         d.type = AIMEE_DELTA_TURN_STOP;
         d.stop_reason = AIMEE_STOP_END_TURN;
         if (delta(id, &d))
            return -1;
         cJSON_DeleteItemFromObjectCaseSensitive(s->started, id);
      }
      return codex_text(item, id, !strcmp(type, "userMessage") ? "user" : "assistant");
   }
   if (!strcmp(type, "contextCompaction") || !strcmp(type, "enteredReviewMode") ||
       !strcmp(type, "exitedReviewMode"))
      return 0;
   const char *input_key = NULL, *output_key = NULL, *name = type;
   if (!strcmp(type, "commandExecution"))
   {
      input_key = "command";
      output_key = "aggregatedOutput";
   }
   else if (!strcmp(type, "mcpToolCall") || !strcmp(type, "dynamicToolCall"))
   {
      input_key = "arguments";
      output_key = "result";
      name = str(item, "tool");
   }
   else if (!strcmp(type, "fileChange"))
   {
      input_key = "changes";
      output_key = "changes";
   }
   else if (!strcmp(type, "webSearch"))
   {
      input_key = "action";
      output_key = "action";
   }
   else if (!strcmp(type, "imageView"))
   {
      input_key = "path";
      output_key = "path";
   }
   else
      return -1; /* never turn unknown items into fake tools */
   size_t n = strlen(id) + 16;
   char *mid = malloc(n);
   if (!mid)
      return -1;
   snprintf(mid, n, "%s:%s", id, complete ? "result" : "use");
   aimee_block_t b = {.type = complete ? AIMEE_BLK_TOOL_RESULT : AIMEE_BLK_TOOL_USE,
                      .tool_id = (char *)id,
                      .tool_name = (char *)name,
                      .tool_namespace = (char *)str(item, "server")};
   cJSON *value = cJSON_CreateObject();
   if (!value)
   {
      free(mid);
      return -1;
   }
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, complete ? output_key : input_key);
   if (source)
      cJSON_AddItemToObject(value, complete ? "output" : "arguments", cJSON_Duplicate(source, 1));
   for (int i = 0; i < 3; i++)
   {
      const char *key = complete ? (const char *[]){"exitCode", "status", "error"}[i]
                                 : (const char *[]){"cwd", "server", "tool"}[i];
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, key);
      if (v)
         cJSON_AddItemToObject(value, key, cJSON_Duplicate(v, 1));
   }
   b.tool_is_error =
       !strcmp(str(item, "status"), "failed") || !strcmp(str(item, "status"), "declined");
   if (complete)
      b.tool_result = value;
   else
      b.tool_input = value;
   int rc = message(mid, complete ? "tool" : "assistant", &b, 1);
   cJSON_Delete(value);
   free(mid);
   return rc;
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
         aimee_session_event_t e = {
             .kind = AIMEE_SESSION_CONTROL_RESPONSE,
             .request_id = id,
             .control_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s->pending, id)),
             .control = result};
         int rc = aimee_ir_session_emit(&e);
         cJSON_DeleteItemFromObjectCaseSensitive(s->pending, id);
         free(id);
         return rc;
      }
      free(id);
   }
   return 0;
}

static int acp_delta(aimee_harness_ir_t *s, aimee_delta_t *d)
{
   aimee_session_event_t e = {
       .kind = AIMEE_SESSION_DELTA, .role = s->acp_role, .message_id = s->message_id, .delta = d};
   return aimee_ir_session_emit(&e);
}
static int acp_close(aimee_harness_ir_t *s)
{
   if (!s->acp_open)
      return 0;
   aimee_delta_t d = {.type = AIMEE_DELTA_BLOCK_STOP, .kind = s->acp_kind};
   if (acp_delta(s, &d))
      return -1;
   d.type = AIMEE_DELTA_TURN_STOP;
   d.stop_reason = AIMEE_STOP_END_TURN;
   if (acp_delta(s, &d))
      return -1;
   s->acp_open = 0;
   return 0;
}
static int acp_chunk(aimee_harness_ir_t *s, const char *role, aimee_block_type_t kind,
                     const char *text)
{
   if (s->acp_open && (s->acp_kind != kind || strcmp(s->acp_role, role)))
      if (acp_close(s))
         return -1;
   if (!s->acp_open)
   {
      snprintf(s->message_id, sizeof(s->message_id), "acp-message-%lu", ++s->counter);
      s->acp_role = role;
      s->acp_kind = kind;
      aimee_delta_t d = {.type = AIMEE_DELTA_TURN_START};
      if (acp_delta(s, &d))
         return -1;
      d.type = AIMEE_DELTA_BLOCK_START;
      d.kind = kind;
      if (acp_delta(s, &d))
         return -1;
      s->acp_open = 1;
   }
   aimee_delta_t d = {.type = AIMEE_DELTA_BLOCK_DELTA, .kind = kind, .text_delta = text};
   return acp_delta(s, &d);
}
static int aimee_acp_frame_ir_decode(const cJSON *f, aimee_harness_ir_t *s)
{
   if (!cJSON_IsObject(f) || !s)
      return -1;
   const char *method = str(f, "method");
   const cJSON *p = cJSON_GetObjectItemCaseSensitive(f, "params");
   const cJSON *rpcid = cJSON_GetObjectItemCaseSensitive(f, "id");
   if (!strcmp(method, "session/request_permission") && rpcid)
   {
      if (acp_close(s))
         return -1;
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
      cJSON_AddStringToObject(s->pending, id, "permission");
      aimee_session_event_t e = {.kind = AIMEE_SESSION_CONTROL_REQUEST,
                                 .request_id = id,
                                 .control_kind = "permission",
                                 .control = p};
      int rc = aimee_ir_session_emit(&e);
      free(id);
      return rc;
   }
   if (!method[0] && rpcid)
   {
      return acp_close(s);
   } /* prompt completion/response */
   if (!strcmp(method, "text/delta"))
      return acp_chunk(s, "assistant", AIMEE_BLK_TEXT, str(p, "content"));
   if (strcmp(method, "session/update"))
      return 0;
   const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "update");
   const char *kind = str(u, "sessionUpdate");
   if (!kind[0])
      kind = str(u, "type");
   if ((!kind[0] && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(u, "content"))) ||
       !strcmp(kind, "agent_message_chunk") || !strcmp(kind, "agent_thought_chunk") ||
       !strcmp(kind, "user_message_chunk"))
   {
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(u, "content");
      const char *role = !strcmp(kind, "user_message_chunk") ? "user" : "assistant";
      if (cJSON_IsString(content) || !strcmp(str(content, "type"), "text"))
         return acp_chunk(
             s, role, !strcmp(kind, "agent_thought_chunk") ? AIMEE_BLK_THINKING : AIMEE_BLK_TEXT,
             cJSON_IsString(content) ? content->valuestring : str(content, "text"));
      if (acp_close(s))
         return -1;
      aimee_block_t b = {0};
      char *ref = NULL;
      if (!strcmp(str(content, "type"), "image"))
      {
         const char *mime = str(content, "mimeType"), *data = str(content, "data");
         if (!mime[0] || !data[0])
            return -1;
         size_t n = strlen(mime) + strlen(data) + 16;
         ref = malloc(n);
         if (!ref)
            return -1;
         snprintf(ref, n, "data:%s;base64,%s", mime, data);
         b.type = AIMEE_BLK_IMAGE;
         b.media_type = (char *)mime;
         b.media_ref = ref;
      }
      else if (!strcmp(str(content, "type"), "resource_link"))
      {
         b.type = AIMEE_BLK_DOCUMENT;
         b.media_type = (char *)str(content, "mimeType");
         if (!b.media_type[0])
            b.media_type = "application/octet-stream";
         b.media_ref = (char *)str(content, "uri");
      }
      else
         return -1;
      char id[64];
      snprintf(id, sizeof(id), "acp-media-%lu", ++s->counter);
      int rc = message(id, role, &b, 1);
      free(ref);
      return rc;
   }
   if (!strcmp(kind, "tool_call") || !strcmp(kind, "tool_call_update"))
   {
      if (acp_close(s))
         return -1;
      const char *id = str(u, "toolCallId");
      if (!id[0] || strlen(id) > 1024)
         return -1;
      if (!s->acp_tools)
         s->acp_tools = cJSON_CreateObject();
      if (!s->acp_tools)
         return -1;
      cJSON *all = cJSON_GetObjectItemCaseSensitive(s->acp_tools, id);
      if (!all)
      {
         all = cJSON_CreateObject();
         if (!all || !cJSON_AddItemToObject(s->acp_tools, id, all))
         {
            cJSON_Delete(all);
            return -1;
         }
      }
      const cJSON *field = NULL;
      cJSON_ArrayForEach(field, u)
      {
         cJSON *copy = cJSON_Duplicate(field, 1);
         if (!copy)
            return -1;
         cJSON_DeleteItemFromObjectCaseSensitive(all, field->string);
         if (!cJSON_AddItemToObject(all, field->string, copy))
         {
            cJSON_Delete(copy);
            return -1;
         }
      }
      const char *name = str(all, "title");
      if (!name[0])
         name = str(all, "kind");
      if (!name[0])
         name = "ACP tool";
      aimee_block_t use = {.type = AIMEE_BLK_TOOL_USE,
                           .tool_id = (char *)id,
                           .tool_name = (char *)name,
                           .tool_input = cJSON_GetObjectItemCaseSensitive(all, "rawInput")};
      char mid[1200];
      snprintf(mid, sizeof(mid), "acp-use:%s", id);
      if (message(mid, "assistant", &use, 1))
         return -1;
      const char *status = str(all, "status");
      if (!strcmp(status, "completed") || !strcmp(status, "failed"))
      {
         cJSON *output = cJSON_CreateObject();
         if (!output)
            return -1;
         const cJSON *raw = cJSON_GetObjectItemCaseSensitive(all, "rawOutput"),
                     *content = cJSON_GetObjectItemCaseSensitive(all, "content");
         if (raw)
            cJSON_AddItemToObject(output, "output", cJSON_Duplicate(raw, 1));
         if (content)
            cJSON_AddItemToObject(output, "content", cJSON_Duplicate(content, 1));
         aimee_block_t result = {.type = AIMEE_BLK_TOOL_RESULT,
                                 .tool_id = (char *)id,
                                 .tool_result = output,
                                 .tool_is_error = !strcmp(status, "failed")};
         snprintf(mid, sizeof(mid), "acp-result:%s", id);
         int rc = message(mid, "tool", &result, 1);
         cJSON_Delete(output);
         return rc;
      }
      return 0;
   }
   if (!strcmp(kind, "plan"))
   {
      if (acp_close(s))
         return -1;
      const cJSON *entries = cJSON_GetObjectItemCaseSensitive(u, "entries");
      char *text = cJSON_PrintUnformatted(entries);
      if (!text)
         return -1;
      char id[64];
      snprintf(id, sizeof(id), "acp-plan-%lu", ++s->counter);
      aimee_block_t b = {.type = AIMEE_BLK_TEXT, .text = text};
      int rc = message(id, "assistant", &b, 1);
      free(text);
      return rc;
   }
   if (!strcmp(kind, "available_commands_update") || !strcmp(kind, "current_mode_update") ||
       !strcmp(kind, "config_option_update") || !strcmp(kind, "session_info_update") ||
       !strcmp(kind, "usage_update"))
      return 0;
   return -1;
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

int aimee_acp_frame_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = aimee_acp_frame_ir_decode(frame, state);
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
      // Form acceptance requires a schema-aware content renderer; decline/cancel
      // are always representable without inventing an empty accepted answer.
      if (allow)
      {
         cJSON_Delete(result);
         return NULL;
      }
      cJSON_AddStringToObject(result, "action",
                              !strcmp(str(decision, "decision"), "cancel") ? "cancel" : "decline");
      cJSON_AddNullToObject(result, "content");
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
