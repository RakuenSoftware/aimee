#include <aimee/translation/harness_ir.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include "cJSON.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The compatibility result consumes the same admitted events as session capture.
 * It is a display/result view, never a second history or native tool authority. */
typedef struct
{
   char *id;
   aimee_ir_response_builder_t builder;
   aimee_response_t snapshot;
   int has_snapshot;
} harness_output_entry_t;
struct aimee_harness_output
{
   harness_output_entry_t *entries;
   int count, failed;
   size_t bytes;
   char *raw_stop_reason;
   aimee_stop_reason_t stop_reason;
};
static void harness_output_clear(struct aimee_harness_output *output)
{
   if (!output)
      return;
   for (int i = 0; i < output->count; i++)
   {
      free(output->entries[i].id);
      aimee_ir_response_builder_free(&output->entries[i].builder);
      aimee_response_free(&output->entries[i].snapshot);
   }
   free(output->entries);
   output->entries = NULL;
   output->count = 0;
   output->bytes = 0;
   free(output->raw_stop_reason);
   output->raw_stop_reason = NULL;
   output->stop_reason = AIMEE_STOP_UNKNOWN;
}
static int harness_output_complete(const struct aimee_harness_output *output)
{
   if (!output || output->failed)
      return 0;
   for (int i = 0; i < output->count; i++)
      if (output->entries[i].builder.started && !output->entries[i].builder.complete)
         return 0;
   return 1;
}
static harness_output_entry_t *harness_output_entry(struct aimee_harness_output *output,
                                                    const char *id)
{
   if (!id || !id[0] || strlen(id) > 4096)
      return NULL;
   for (int i = 0; i < output->count; i++)
      if (!strcmp(output->entries[i].id, id))
         return &output->entries[i];
   if (output->count == AIMEE_IR_MAX_BLOCKS)
      return NULL;
   harness_output_entry_t *entries =
       realloc(output->entries, ((size_t)output->count + 1) * sizeof *entries);
   if (!entries)
      return NULL;
   output->entries = entries;
   harness_output_entry_t *entry = &entries[output->count];
   *entry = (harness_output_entry_t){.id = strdup(id)};
   if (!entry->id)
      return NULL;
   output->count++;
   return entry;
}
static int harness_snapshot_matches(const aimee_response_t *response,
                                    const aimee_message_t *message, int prefix)
{
   if (response->n_content > message->n_blocks ||
       (!prefix && response->n_content != message->n_blocks))
      return 0;
   for (int i = 0; i < response->n_content; i++)
   {
      const aimee_block_t *before = &response->content[i], *after = &message->blocks[i];
      cJSON *a = aimee_ir_block_json(before), *b = aimee_ir_block_json(after);
      if (prefix && i == response->n_content - 1 &&
          (before->type == AIMEE_BLK_TEXT || before->type == AIMEE_BLK_THINKING) && before->text &&
          after->text && !strncmp(before->text, after->text, strlen(before->text)))
      {
         cJSON *text = cJSON_CreateString(after->text);
         if (!text || !cJSON_ReplaceItemInObjectCaseSensitive(a, "text", text))
            cJSON_Delete(text);
      }
      int equal = a && b && cJSON_Compare(a, b, 1);
      cJSON_Delete(a);
      cJSON_Delete(b);
      if (!equal)
         return 0;
   }
   return 1;
}

typedef struct
{
   struct aimee_harness_output *output;
   aimee_ir_session_sink_fn parent;
   void *parent_context;
} harness_output_capture_t;
static int harness_output_observe(const aimee_session_event_t *event, void *context)
{
   harness_output_capture_t *capture = context;
   struct aimee_harness_output *output = capture->output;
   if (output->failed)
      return -1;
   if (event->kind == AIMEE_SESSION_MESSAGE || event->kind == AIMEE_SESSION_DELTA)
   {
      const char *role = event->kind == AIMEE_SESSION_MESSAGE && event->message
                             ? event->message->role
                             : event->role;
      if (!role)
         return -1;
      if (strcmp(role, "assistant"))
      {
         if (!harness_output_complete(output))
            return -1;
         harness_output_clear(output);
      }
      else
      {
         cJSON *json = aimee_ir_session_event_json(event);
         char *wire = json ? cJSON_PrintUnformatted(json) : NULL;
         cJSON_Delete(json);
         if (!wire)
            return -1;
         size_t bytes = strlen(wire);
         free(wire);
         if (bytes > 64u * 1024u * 1024u - output->bytes)
            return -1;
         output->bytes += bytes;
         harness_output_entry_t *entry = harness_output_entry(output, event->message_id);
         if (!entry)
            return -1;
         if (event->kind == AIMEE_SESSION_DELTA)
         {
            if (entry->has_snapshot ||
                aimee_ir_response_builder_apply(&entry->builder, event->delta))
               return -1;
         }
         else
         {
            aimee_response_t view = entry->builder.response, admitted = {0};
            view.role = "assistant";
            view.id = entry->id;
            view.content = event->message->blocks;
            view.n_content = event->message->n_blocks;
            if (aimee_ir_response_clone(&view, &admitted) || aimee_ir_response_admit(&admitted))
            {
               aimee_response_free(&admitted);
               return -1;
            }
            aimee_message_t snapshot = {
                .role = "assistant", .blocks = admitted.content, .n_blocks = admitted.n_content};
            if ((entry->has_snapshot &&
                 !harness_snapshot_matches(&entry->snapshot, &snapshot, 0)) ||
                (entry->builder.started &&
                 (!entry->builder.complete ||
                  !harness_snapshot_matches(&entry->builder.response, &snapshot, 1))))
            {
               aimee_response_free(&admitted);
               return -1;
            }
            aimee_response_free(&entry->snapshot);
            entry->snapshot = admitted;
            entry->has_snapshot = 1;
         }
      }
   }
   return capture->parent ? capture->parent(event, capture->parent_context) : 0;
}
static int harness_output_decode(const cJSON *frame, aimee_harness_ir_t *state,
                                 int (*decode)(const cJSON *, aimee_harness_ir_t *))
{
   if (!state)
      return -1;
   if (state->host_model_capture)
      return decode(frame, state); /* Original host model observation remains authoritative. */
   if (!state->output)
      state->output = calloc(1, sizeof *state->output);
   if (!state->output || state->output->failed)
      return -1;
   harness_output_capture_t capture = {.output = state->output};
   capture.parent = aimee_ir_session_get_sink(&capture.parent_context);
   int inherited = aimee_ir_session_capture_failed();
   aimee_ir_session_restore_sink(harness_output_observe, &capture, inherited);
   int rc = decode(frame, state);
   int failed = inherited || aimee_ir_session_capture_failed() || rc;
   aimee_ir_session_restore_sink(capture.parent, capture.parent_context, failed);
   if (failed)
   {
      state->output->failed = 1;
      return -1;
   }
   return rc;
}
int aimee_harness_ir_response(const aimee_harness_ir_t *state, aimee_response_t *out)
{
   if (!state || !out || !harness_output_complete(state->output))
      return -1;
   aimee_response_t view = {.role = "assistant"};
   for (int i = 0; i < state->output->count; i++)
   {
      const harness_output_entry_t *entry = &state->output->entries[i];
      const aimee_response_t *response =
          entry->has_snapshot ? &entry->snapshot : &entry->builder.response;
      if ((!entry->has_snapshot && !entry->builder.complete) ||
          response->n_content > AIMEE_IR_MAX_BLOCKS - view.n_content)
         goto fail;
      aimee_block_t *blocks =
          realloc(view.content, (size_t)(view.n_content + response->n_content) * sizeof *blocks);
      if (!blocks && response->n_content)
         goto fail;
      view.content = blocks;
      for (int j = 0; j < response->n_content; j++)
      {
         view.content[view.n_content] = response->content[j];
         view.content[view.n_content++].message_group = i + 1;
      }
      view.id = entry->id;
      view.stop_reason = response->stop_reason;
      view.raw_stop_reason = response->raw_stop_reason;
      view.usage_in = response->usage_in;
      view.usage_out = response->usage_out;
      view.usage_cache_read = response->usage_cache_read;
      view.usage_cache_write = response->usage_cache_write;
      view.usage_reasoning = response->usage_reasoning;
   }
   if (state->output->raw_stop_reason)
   {
      view.raw_stop_reason = state->output->raw_stop_reason;
      view.stop_reason = state->output->stop_reason;
   }
   int rc = view.n_content ? aimee_ir_response_clone(&view, out) : -1;
   free(view.content); /* Its blocks borrow into the original canonical observations. */
   return rc;
fail:
   free(view.content);
   return -1;
}

static const char *str(const cJSON *o, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}
/* ACP client resources are distinct from model-visible read_file/write_file:
 * their byte/range fidelity is the resource owner's contract, without model
 * output truncation or inferred filesystem authority. */
static char *acp_rpc_key(const cJSON *id)
{
   if (cJSON_IsString(id))
   {
      if (!id->valuestring[0] || strlen(id->valuestring) > 4096)
         return NULL;
   }
   else if (!cJSON_IsNumber(id) || !isfinite(id->valuedouble) ||
            id->valuedouble < -9007199254740991.0 || id->valuedouble > 9007199254740991.0 ||
            (double)(int64_t)id->valuedouble != id->valuedouble)
      return NULL;
   return cJSON_PrintUnformatted(id);
}
static int acp_request_once(aimee_harness_ir_t *state, const char *key)
{
   if (!state->acp_client_requests)
      state->acp_client_requests = cJSON_CreateObject();
   return !key || !state->acp_client_requests ||
                  cJSON_GetArraySize(state->acp_client_requests) >= AIMEE_IR_MAX_BLOCKS ||
                  cJSON_GetObjectItemCaseSensitive(state->acp_client_requests, key) ||
                  !cJSON_AddTrueToObject(state->acp_client_requests, key)
              ? -1
              : 0;
}
int aimee_acp_client_tool(const cJSON *frame, aimee_block_t *out)
{
   if (!out || !cJSON_IsObject(frame) || !aimee_ir_json_unique(frame))
      return -1;
   const char *method = str(frame, "method");
   const char *name = !strcmp(method, "fs/read_text_file")    ? "read_text_file"
                      : !strcmp(method, "fs/write_text_file") ? "write_text_file"
                                                              : NULL;
   if (!name)
      return 0;
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(frame, "params");
   const cJSON *path = cJSON_GetObjectItemCaseSensitive(params, "path");
   const cJSON *content = cJSON_GetObjectItemCaseSensitive(params, "content");
   char *key = acp_rpc_key(cJSON_GetObjectItemCaseSensitive(frame, "id"));
   if (!key || !cJSON_IsObject(params) || !cJSON_IsString(path) || !path->valuestring[0] ||
       (!strcmp(name, "write_text_file") && !cJSON_IsString(content)))
   {
      free(key);
      return -1;
   }
   aimee_block_t call = {.type = AIMEE_BLK_TOOL_USE,
                         .tool_namespace = strdup("acp"),
                         .tool_name = strdup(name),
                         .tool_input = cJSON_Duplicate(params, 1)};
   size_t size = strlen(key) + sizeof "acp-rpc:";
   call.tool_id = malloc(size);
   if (call.tool_id)
      snprintf(call.tool_id, size, "acp-rpc:%s", key);
   free(key);
   aimee_response_t view = {.role = "assistant", .content = &call, .n_content = 1};
   if (!call.tool_id || !call.tool_namespace || !call.tool_name || !call.tool_input ||
       aimee_ir_response_admit(&view))
   {
      aimee_block_free_contents(&call);
      return -1;
   }
   aimee_block_free_contents(out);
   *out = call;
   return 1;
}
int aimee_acp_take_client_tool(aimee_harness_ir_t *state, aimee_block_t *out)
{
   if (!state || !out || !state->acp_client_call.tool_id)
      return -1;
   aimee_block_free_contents(out);
   *out = state->acp_client_call;
   state->acp_client_call = (aimee_block_t){0};
   return 0;
}
int aimee_acp_client_tool_result_ir(aimee_harness_ir_t *state, const aimee_block_t *result)
{
   if (!state || !result || result->type != AIMEE_BLK_TOOL_RESULT || !aimee_ir_block_valid(result))
      return -1;
   aimee_message_t message = {.role = "tool", .blocks = (aimee_block_t *)result, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = result->tool_id, .message = &message};
   harness_output_capture_t capture = {.output = state->output};
   capture.parent = aimee_ir_session_get_sink(&capture.parent_context);
   int rc = state->host_model_capture ? aimee_ir_session_emit(&event)
            : !capture.output         ? -1
                                      : harness_output_observe(&event, &capture);
   if (rc)
   {
      if (state->output)
         state->output->failed = 1;
      aimee_ir_session_fail_capture();
   }
   return rc;
}
static cJSON *acp_rpc_envelope(const cJSON *id)
{
   char *key = acp_rpc_key(id);
   if (!key)
      return NULL;
   free(key);
   cJSON *out = cJSON_CreateObject(), *copy = cJSON_Duplicate(id, 1);
   if (!out || !copy || !cJSON_AddStringToObject(out, "jsonrpc", "2.0") ||
       !cJSON_AddItemToObject(out, "id", copy))
   {
      cJSON_Delete(copy);
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}
cJSON *aimee_acp_error_reply(const cJSON *id, int code, const char *message)
{
   cJSON *out = acp_rpc_envelope(id);
   cJSON *error = out ? cJSON_AddObjectToObject(out, "error") : NULL;
   if (!error || !cJSON_AddNumberToObject(error, "code", code) ||
       !cJSON_AddStringToObject(error, "message", message ? message : "request refused"))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}
cJSON *aimee_acp_client_tool_reply(const cJSON *frame, const aimee_block_t *result)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(frame, "id");
   aimee_block_t call = {0};
   int admitted = aimee_acp_client_tool(frame, &call);
   int valid = admitted == 1 && result && result->type == AIMEE_BLK_TOOL_RESULT &&
               result->tool_id && !strcmp(result->tool_id, call.tool_id) &&
               cJSON_IsString(result->tool_result) && aimee_ir_block_valid(result);
   aimee_block_free_contents(&call);
   if (!valid)
      return NULL;
   if (result->tool_is_error)
      return aimee_acp_error_reply(id, -32603, result->tool_result->valuestring);
   cJSON *out = acp_rpc_envelope(id);
   cJSON *body = out ? cJSON_AddObjectToObject(out, "result") : NULL;
   if (!body || (!strcmp(str(frame, "method"), "fs/read_text_file") &&
                 !cJSON_AddStringToObject(body, "content", result->tool_result->valuestring)))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}
cJSON *aimee_acp_control_reply(const cJSON *id, const cJSON *request, const cJSON *decision)
{
   if (!aimee_ir_control_reply_valid("permission", request, decision))
      return NULL;
   const char *choice = str(decision, "decision"), *selected = NULL;
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(request, "input");
   const cJSON *options = cJSON_GetObjectItemCaseSensitive(input, "options"), *option;
   cJSON_ArrayForEach(option, options)
   {
      const char *kind = str(option, "kind"), *key = str(option, "optionId");
      if (key[0] && ((!strcmp(choice, "allow") && !strcmp(kind, "allow_once")) ||
                     (!strcmp(choice, "deny") && !strcmp(kind, "reject_once"))))
      {
         selected = key;
         break;
      }
   }
   /* A one-operation approval never expands into a remembered grant. */
   if (!strcmp(choice, "allow") && !selected)
      return NULL;
   cJSON *out = acp_rpc_envelope(id);
   cJSON *body = out ? cJSON_AddObjectToObject(out, "result") : NULL;
   cJSON *outcome = body ? cJSON_AddObjectToObject(body, "outcome") : NULL;
   if (!outcome ||
       !cJSON_AddStringToObject(outcome, "outcome", selected ? "selected" : "cancelled") ||
       (selected && !cJSON_AddStringToObject(outcome, "optionId", selected)))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
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
   harness_output_clear(s->output);
   free(s->output);
   cJSON_Delete(s->started);
   cJSON_Delete(s->pending);
   cJSON_Delete(s->claude_message);
   cJSON_Delete(s->acp_tools);
   cJSON_Delete(s->acp_client_requests);
   aimee_block_free_contents(&s->acp_client_call);
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
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "is_api_error_message")))
      {
         const cJSON *message = cJSON_GetObjectItemCaseSensitive(f, "message");
         if (!cJSON_IsObject(message) || strcmp(str(message, "role"), "assistant"))
            return -1;
         /* This SDK envelope is a transport diagnostic, not model output.
          * Never copy its message/errors text into canonical observations. */
         if (!strcmp(str(f, "error"), "rate_limit"))
            s->failure = AIMEE_HARNESS_FAILURE_RATE_LIMIT;
         return 0;
      }
   }
   else if ((!strcmp(type, "result") &&
             cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(f, "is_error"))) ||
            (!strcmp(type, "stream_event") &&
             !strcmp(str(cJSON_GetObjectItemCaseSensitive(f, "event"), "type"), "message_start")))
      s->failure = AIMEE_HARNESS_FAILURE_NONE;
   if (s->host_model_capture && (!strcmp(type, "stream_event") || !strcmp(type, "assistant") ||
                                 !strcmp(type, "user") || !strcmp(type, "result")))
      return 0; /* Original host capture owns model/tool/history observations. */

   if (!strcmp(type, "system") && !strcmp(str(f, "subtype"), "compact_boundary"))
      return native_compaction("claude");
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
      int rc = claude_snapshot(m, id);
      if (rc)
         return rc;
      cJSON *copy = cJSON_Duplicate(m, 1);
      if (!copy)
         return -1;
      cJSON_Delete(s->claude_message);
      s->claude_message = copy;
      return 0;
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
         for (int j = 0; j < ir.messages[0].n_blocks; j++)
         {
            free(ir.messages[0].blocks[j].thinking_binding);
            ir.messages[0].blocks[j].thinking_binding = NULL;
         }
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
   if (!strcmp(method, "thread/compacted"))
      return native_compaction("codex");
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(f, "params");
   if (s->host_model_capture && !strcmp(method, "item/agentMessage/delta"))
      return 0;
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
   int complete = !strcmp(method, "item/completed");
   if (s->host_model_capture &&
       (!strcmp(type, "agentMessage") || !strcmp(type, "userMessage") ||
        !strcmp(type, "reasoning") || !strcmp(type, "plan") || !strcmp(type, "commandExecution") ||
        !strcmp(type, "mcpToolCall") || !strcmp(type, "dynamicToolCall") ||
        !strcmp(type, "fileChange") || !strcmp(type, "webSearch") || !strcmp(type, "imageView")))
      return 0;
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
   if (!strcmp(type, "contextCompaction"))
      return native_compaction("codex");
   if (!strcmp(type, "enteredReviewMode") || !strcmp(type, "exitedReviewMode"))
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
      output_key = !strcmp(type, "dynamicToolCall") ? "contentItems" : "result";
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
   const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(item, "exitCode");
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(item, "error");
   const cJSON *success = cJSON_GetObjectItemCaseSensitive(item, "success");
   b.tool_is_error =
       complete &&
       (!strcmp(str(item, "status"), "failed") || !strcmp(str(item, "status"), "declined") ||
        (!strcmp(type, "commandExecution") && cJSON_IsNumber(exit_code) &&
         exit_code->valuedouble != 0) ||
        (!strcmp(type, "dynamicToolCall") && cJSON_IsFalse(success)) ||
        (!strcmp(type, "mcpToolCall") &&
         ((error && !cJSON_IsNull(error)) ||
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(source, "isError")))));
   if (complete)
      b.tool_result = value;
   else
      b.tool_input = value;
   char *native_namespace = NULL;
   if (!strcmp(type, "mcpToolCall") || !strcmp(type, "dynamicToolCall"))
   {
      /* A function call's input is its actual arguments. Host SDK metadata
       * belongs outside that object; wrapping it changes a replayed call. */
      if (!complete)
      {
         cJSON_Delete(value);
         value = source ? cJSON_Duplicate(source, 1) : cJSON_CreateObject();
         if (!value)
         {
            free(mid);
            return -1;
         }
         b.tool_input = value;
      }
      if (!strcmp(type, "dynamicToolCall"))
         b.tool_namespace = (char *)str(item, "namespace");
      else if (str(item, "server")[0])
      {
         /* Match the callable namespace advertised by Codex's MCP owner. */
         size_t capacity = strlen(str(item, "server")) + 6;
         native_namespace = malloc(capacity);
         if (!native_namespace)
         {
            cJSON_Delete(value);
            free(mid);
            return -1;
         }
         snprintf(native_namespace, capacity, "mcp__%s", str(item, "server"));
         b.tool_namespace = native_namespace;
      }
   }
   int rc = message(mid, complete ? "tool" : "assistant", &b, 1);
   cJSON_Delete(value);
   free(native_namespace);
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
   if (rpcid && (!strcmp(method, "fs/read_text_file") || !strcmp(method, "fs/write_text_file")))
   {
      if (s->acp_client_call.tool_id || acp_close(s))
         return -1;
      const char *session = str(p, "sessionId");
      if (session[0] && (!s->acp_session_id || strcmp(session, s->acp_session_id)))
         return -1;
      aimee_block_t call = {0};
      if (aimee_acp_client_tool(f, &call) != 1)
         return -1;
      if (acp_request_once(s, call.tool_id + sizeof "acp-rpc:" - 1) ||
          message(call.tool_id, "assistant", &call, 1))
      {
         aimee_block_free_contents(&call);
         return -1;
      }
      s->acp_client_call = call;
      return 0;
   }
   if (!strcmp(method, "session/request_permission") && rpcid)
   {
      if (acp_close(s))
         return -1;
      char *id = acp_rpc_key(rpcid);
      if (!id || acp_request_once(s, id))
      {
         free(id);
         return -1;
      }
      if (!s->pending)
         s->pending = cJSON_CreateObject();
      if (!s->pending)
      {
         free(id);
         return -1;
      }
      const char *session = str(p, "sessionId");
      cJSON *payload =
          control_payload(str(cJSON_GetObjectItemCaseSensitive(p, "toolCall"), "title"), p, NULL);
      if (!payload || (session[0] && (!s->acp_session_id || strcmp(session, s->acp_session_id))) ||
          cJSON_GetObjectItemCaseSensitive(s->pending, id) ||
          !cJSON_AddStringToObject(s->pending, id, "permission"))
      {
         cJSON_Delete(payload);
         free(id);
         return -1;
      }
      aimee_session_event_t e = {.kind = AIMEE_SESSION_CONTROL_REQUEST,
                                 .request_id = id,
                                 .control_kind = "permission",
                                 .control = payload};
      int rc = aimee_ir_session_emit(&e);
      cJSON_Delete(payload);
      free(id);
      return rc;
   }
   if (!method[0] && rpcid)
   {
      const cJSON *result = cJSON_GetObjectItemCaseSensitive(f, "result");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
      /* The supported legacy terminal text is decoded here as native input,
       * never rebuilt from the executor's accumulated display buffer. */
      int open_text =
          s->acp_open && s->acp_kind == AIMEE_BLK_TEXT && !strcmp(s->acp_role, "assistant");
      if (acp_close(s))
         return -1;
      if (content)
      {
         if (!cJSON_IsString(content))
            return -1;
         char id[256];
         if (open_text)
            snprintf(id, sizeof id, "%s", s->message_id);
         else
            snprintf(id, sizeof id, "acp-result-%lu", ++s->counter);
         aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = content->valuestring};
         if (message(id, "assistant", &block, 1))
            return -1;
      }
      const cJSON *stop = cJSON_GetObjectItemCaseSensitive(result, "stopReason");
      if (stop)
      {
         if (!cJSON_IsString(stop) || !stop->valuestring[0] || !s->output)
            return -1;
         char *copy = strdup(stop->valuestring);
         if (!copy)
            return -1;
         free(s->output->raw_stop_reason);
         s->output->raw_stop_reason = copy;
         s->output->stop_reason = !strcmp(copy, "end_turn")     ? AIMEE_STOP_END_TURN
                                  : !strcmp(copy, "max_tokens") ? AIMEE_STOP_MAX_TOKENS
                                  : !strcmp(copy, "refusal")    ? AIMEE_STOP_CONTENT_FILTER
                                  : !strcmp(copy, "cancelled")  ? AIMEE_STOP_ERROR
                                                                : AIMEE_STOP_UNKNOWN;
      }
      return 0;
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
   int rc = harness_output_decode(frame, state, aimee_codex_frame_ir_decode);
   if (rc < 0)
      aimee_ir_session_fail_capture();
   return rc;
}

int aimee_codex_reply_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = harness_output_decode(frame, state, aimee_codex_reply_ir_decode);
   if (rc < 0)
      aimee_ir_session_fail_capture();
   return rc;
}

int aimee_acp_frame_ir(const cJSON *frame, aimee_harness_ir_t *state)
{
   int rc = harness_output_decode(frame, state, aimee_acp_frame_ir_decode);
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
