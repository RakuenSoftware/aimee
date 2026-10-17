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

int aimee_claude_frame_ir(const cJSON *f, aimee_harness_ir_t *s)
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
      const cJSON *payload = cJSON_GetObjectItemCaseSensitive(
          f, !strcmp(type, "control_request") ? "request" : "response");
      const char *id = str(f, "request_id");
      if (!id[0])
         id = str(payload, "request_id");
      aimee_session_event_t e = {.kind = !strcmp(type, "control_request")
                                             ? AIMEE_SESSION_CONTROL_REQUEST
                                             : AIMEE_SESSION_CONTROL_RESPONSE,
                                 .request_id = id,
                                 .control_kind = "permission",
                                 .control = payload};
      return aimee_ir_session_emit(&e);
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
int aimee_codex_frame_ir(const cJSON *f, aimee_harness_ir_t *s)
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
       (strstr(method, "requestApproval") || !strcmp(method, "item/tool/requestUserInput")))
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
      cJSON_AddTrueToObject(s->pending, id);
      aimee_session_event_t e = {.kind = AIMEE_SESSION_CONTROL_REQUEST,
                                 .request_id = id,
                                 .control_kind = "permission",
                                 .control = params};
      int rc = aimee_ir_session_emit(&e);
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

int aimee_codex_reply_ir(const cJSON *f, aimee_harness_ir_t *s)
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
         aimee_session_event_t e = {.kind = AIMEE_SESSION_CONTROL_RESPONSE,
                                    .request_id = id,
                                    .control_kind = "permission",
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
int aimee_acp_frame_ir(const cJSON *f, aimee_harness_ir_t *s)
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
      cJSON_AddTrueToObject(s->pending, id);
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
