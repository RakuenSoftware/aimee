#include <aimee/ir/session_event.h>
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local aimee_ir_session_sink_fn session_sink;
static _Thread_local void *session_context;
static _Thread_local int session_capture_failed;

static const char *block_kind(aimee_block_type_t type)
{
   static const char *const names[] = {"text",     "tool_use", "tool_result", "image",
                                       "document", "thinking", "unknown"};
   return type >= AIMEE_BLK_TEXT && type <= AIMEE_BLK_UNKNOWN ? names[type] : NULL;
}

static int add_string(cJSON *object, const char *key, const char *value)
{
   return !value || cJSON_AddStringToObject(object, key, value) != NULL;
}

static int add_copy(cJSON *object, const char *key, const cJSON *value)
{
   if (!value)
      return 1;
   cJSON *copy = cJSON_Duplicate(value, 1);
   if (!copy)
      return 0;
   if (cJSON_AddItemToObject(object, key, copy))
      return 1;
   cJSON_Delete(copy);
   return 0;
}

cJSON *aimee_ir_block_json(const aimee_block_t *block)
{
   if (!aimee_ir_block_valid(block))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   int ok = add_string(out, "type", block_kind(block->type)) &&
            add_string(out, "cache_control", block->cache_control) &&
            add_string(out, "cache_ttl", block->cache_ttl);
   if (block->message_group)
      ok = ok && cJSON_AddNumberToObject(out, "message_group", block->message_group) != NULL;
   cJSON *context = cJSON_AddObjectToObject(out, "context");
   if (!context || !cJSON_AddNumberToObject(context, "origin", block->context.origin) ||
       !cJSON_AddNumberToObject(context, "authority", block->context.authority) ||
       !cJSON_AddNumberToObject(context, "trust", block->context.trust) ||
       !cJSON_AddNumberToObject(context, "sensitivity", block->context.sensitivity) ||
       !cJSON_AddBoolToObject(context, "model_visible", block->context.model_visible) ||
       !add_string(context, "revision_domain", block->context.revision_domain) ||
       !add_string(context, "revision_scope", block->context.revision_scope))
      ok = 0;
   char epoch[32];
   snprintf(epoch, sizeof(epoch), "%llu", block->context.revision_epoch);
   ok = ok && add_string(context, "revision_epoch", epoch);
   switch (block->type)
   {
   case AIMEE_BLK_TEXT:
   case AIMEE_BLK_THINKING:
      ok = ok && add_string(out, "text", block->text ? block->text : "");
      if (block->type == AIMEE_BLK_THINKING)
         ok = ok && add_string(out, "signature", block->thinking_signature) &&
              add_string(out, "thinking_id", block->thinking_id) &&
              add_string(out, "encrypted_content", block->thinking_encrypted_content) &&
              add_string(out, "redacted_content", block->thinking_redacted);
      break;
   case AIMEE_BLK_TOOL_USE:
      ok = ok && add_string(out, "tool_id", block->tool_id) &&
           add_string(out, "name", block->tool_name) &&
           add_string(out, "namespace", block->tool_namespace) &&
           add_copy(out, "input", block->tool_input);
      break;
   case AIMEE_BLK_TOOL_RESULT:
      ok = ok && add_string(out, "tool_id", block->tool_id) &&
           add_copy(out, "content", block->tool_result) &&
           cJSON_AddBoolToObject(out, "is_error", block->tool_is_error) != NULL;
      break;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
      ok = ok && add_string(out, "media_type", block->media_type) &&
           add_string(out, "media_ref", block->media_ref);
      break;
   default:
      ok = 0;
   }
   if (!ok)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

cJSON *aimee_ir_message_json(const aimee_message_t *message)
{
   if (!message || !message->role || !message->role[0] || message->n_blocks < 0 ||
       message->n_blocks > 4096 || (message->n_blocks && !message->blocks))
      return NULL;
   cJSON *out = cJSON_CreateObject(), *blocks = cJSON_CreateArray();
   if (!out || !blocks || !add_string(out, "role", message->role))
      goto fail;
   for (int i = 0; i < message->n_blocks; i++)
   {
      cJSON *block = aimee_ir_block_json(&message->blocks[i]);
      if (!block)
         goto fail;
      if (!cJSON_AddItemToArray(blocks, block))
      {
         cJSON_Delete(block);
         goto fail;
      }
   }
   if (!cJSON_AddItemToObject(out, "blocks", blocks))
      goto fail;
   return out;
fail:
   cJSON_Delete(blocks);
   cJSON_Delete(out);
   return NULL;
}

static int read_string(const cJSON *json, const char *key, char **out, int required)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
   if (!value)
      return required ? -1 : 0;
   if (!cJSON_IsString(value) || !value->valuestring)
      return -1;
   *out = strdup(value->valuestring);
   return *out ? 0 : -1;
}

static int read_enum(const cJSON *json, const char *key, int max, int *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
   if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > max ||
       value->valuedouble != value->valueint)
      return -1;
   *out = value->valueint;
   return 0;
}

static int read_context(const cJSON *json, aimee_context_meta_t *out)
{
   if (!json)
      return 0;
   int origin, authority, trust, sensitivity;
   if (!cJSON_IsObject(json) || read_enum(json, "origin", AIMEE_CTX_ORIGIN_MODEL, &origin) ||
       read_enum(json, "authority", AIMEE_CTX_AUTH_POLICY, &authority) ||
       read_enum(json, "trust", AIMEE_CTX_TRUST_VERIFIED, &trust) ||
       read_enum(json, "sensitivity", AIMEE_CTX_SENS_RESTRICTED, &sensitivity))
      return -1;
   const cJSON *visible = cJSON_GetObjectItemCaseSensitive(json, "model_visible");
   const char *domain =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_domain"));
   const char *scope =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_scope"));
   const char *epoch =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_epoch"));
   if (!cJSON_IsBool(visible) || !domain || !scope || !epoch || !*epoch ||
       strlen(domain) >= sizeof out->revision_domain || strlen(scope) >= sizeof out->revision_scope)
      return -1;
   for (const char *p = epoch; *p; p++)
      if (*p < '0' || *p > '9')
         return -1;
   errno = 0;
   unsigned long long revision = strtoull(epoch, NULL, 10);
   if (errno == ERANGE)
      return -1;
   out->origin = origin;
   out->authority = authority;
   out->trust = trust;
   out->sensitivity = sensitivity;
   out->model_visible = cJSON_IsTrue(visible);
   strcpy(out->revision_domain, domain);
   strcpy(out->revision_scope, scope);
   out->revision_epoch = revision;
   return 0;
}

static int parse_block(const cJSON *json, aimee_block_t *out)
{
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "type"));
   if (!cJSON_IsObject(json) || !type)
      return -1;
   int kind;
   for (kind = AIMEE_BLK_TEXT; kind < AIMEE_BLK_UNKNOWN; kind++)
      if (!strcmp(type, block_kind(kind)))
         break;
   if (kind == AIMEE_BLK_UNKNOWN)
      return -1;
   out->type = kind;
   const cJSON *group = cJSON_GetObjectItemCaseSensitive(json, "message_group");
   if (group)
   {
      if (!cJSON_IsNumber(group) || group->valuedouble < 1 || group->valuedouble > 4096 ||
          group->valuedouble != group->valueint)
         return -1;
      out->message_group = group->valueint;
   }
   if (read_context(cJSON_GetObjectItemCaseSensitive(json, "context"), &out->context) ||
       read_string(json, "cache_control", &out->cache_control, 0) ||
       read_string(json, "cache_ttl", &out->cache_ttl, 0))
      return -1;
   const cJSON *value;
   switch (out->type)
   {
   case AIMEE_BLK_TEXT:
   case AIMEE_BLK_THINKING:
      return read_string(json, "text", &out->text, 1) ||
                     read_string(json, "signature", &out->thinking_signature, 0) ||
                     read_string(json, "thinking_id", &out->thinking_id, 0) ||
                     read_string(json, "encrypted_content", &out->thinking_encrypted_content, 0) ||
                     read_string(json, "redacted_content", &out->thinking_redacted, 0)
                 ? -1
                 : 0;
   case AIMEE_BLK_TOOL_USE:
      if (read_string(json, "tool_id", &out->tool_id, 1) || !out->tool_id[0] ||
          read_string(json, "name", &out->tool_name, 1) || !out->tool_name[0] ||
          read_string(json, "namespace", &out->tool_namespace, 0))
         return -1;
      value = cJSON_GetObjectItemCaseSensitive(json, "input");
      out->tool_input = value ? cJSON_Duplicate(value, 1) : NULL;
      return value && !out->tool_input ? -1 : 0;
   case AIMEE_BLK_TOOL_RESULT:
      if (read_string(json, "tool_id", &out->tool_id, 1) || !out->tool_id[0])
         return -1;
      value = cJSON_GetObjectItemCaseSensitive(json, "is_error");
      if (!cJSON_IsBool(value))
         return -1;
      out->tool_is_error = cJSON_IsTrue(value);
      value = cJSON_GetObjectItemCaseSensitive(json, "content");
      out->tool_result = value ? cJSON_Duplicate(value, 1) : NULL;
      return value && !out->tool_result ? -1 : 0;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
      return read_string(json, "media_type", &out->media_type, 0) ||
                     (out->media_type && !out->media_type[0]) ||
                     read_string(json, "media_ref", &out->media_ref, 1) || !out->media_ref[0]
                 ? -1
                 : 0;
   default:
      return -1;
   }
}

int aimee_ir_message_parse(const cJSON *json, aimee_message_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(json, "blocks");
   if (!cJSON_IsObject(json) || !cJSON_IsArray(blocks) || cJSON_GetArraySize(blocks) > 4096 ||
       read_string(json, "role", &out->role, 1) || !out->role[0])
      goto fail;
   int count = cJSON_GetArraySize(blocks);
   if (count)
   {
      out->blocks = calloc((size_t)count, sizeof *out->blocks);
      if (!out->blocks)
         goto fail;
   }
   for (int i = 0; i < count; i++)
   {
      out->n_blocks++;
      if (parse_block(cJSON_GetArrayItem(blocks, i), &out->blocks[i]))
         goto fail;
   }
   return 0;
fail:
   aimee_message_free(out);
   return -1;
}

static int flush_pending(cJSON *messages, const char **ids, int count)
{
   int repaired = 0;
   for (int i = 0; i < count; i++)
   {
      if (!ids[i])
         continue;
      aimee_block_t block = {.type = AIMEE_BLK_TOOL_RESULT,
                             .tool_id = (char *)ids[i],
                             .tool_result =
                                 cJSON_CreateString("[Tool call was cancelled or timed out]"),
                             .tool_is_error = 1};
      aimee_message_t message = {.role = "tool", .blocks = &block, .n_blocks = 1};
      aimee_request_t request = {.messages = &message, .n_messages = 1};
      aimee_ir_classify_context(&request);
      cJSON *item = block.tool_result ? aimee_ir_message_json(&message) : NULL;
      cJSON_Delete(block.tool_result);
      if (!item || !cJSON_AddItemToArray(messages, item))
      {
         cJSON_Delete(item);
         return -1;
      }
      repaired++;
   }
   return repaired;
}

int aimee_ir_history_repair(cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return -1;
   cJSON *rebuilt = cJSON_CreateArray();
   const char **pending = calloc(4096, sizeof *pending);
   if (!rebuilt || !pending)
      goto fail;
   int count = 0, repairs = 0;
   const cJSON *message;
   cJSON_ArrayForEach(message, messages)
   {
      const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(message, "blocks");
      if (!cJSON_IsArray(blocks) || cJSON_GetArraySize(blocks) > 4096)
         goto fail;
      int has_results = 0;
      const cJSON *block;
      cJSON_ArrayForEach(block, blocks)
      {
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
         has_results |= kind && !strcmp(kind, "tool_result");
      }
      if (!has_results)
      {
         int n = flush_pending(rebuilt, pending, count);
         if (n < 0)
            goto fail;
         repairs += n;
         count = 0;
      }
      cJSON *copy = cJSON_Duplicate(message, 1);
      if (!copy)
         goto fail;
      cJSON *copied_blocks = cJSON_GetObjectItemCaseSensitive(copy, "blocks");
      cJSON *current = copied_blocks->child;
      while (current)
      {
         cJSON *next = current->next;
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "type"));
         const char *id =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "tool_id"));
         if (kind && !strcmp(kind, "tool_result"))
         {
            int matched = 0;
            for (int i = 0; i < count; i++)
               if (id && pending[i] && !strcmp(id, pending[i]))
               {
                  pending[i] = NULL;
                  matched = 1;
                  break;
               }
            if (!matched)
            {
               cJSON_Delete(cJSON_DetachItemViaPointer(copied_blocks, current));
               repairs++;
            }
         }
         current = next;
      }
      /* Keep opaque IDs borrowed from the original, which remains unchanged
       * until every allocation and validation has succeeded. */
      cJSON_ArrayForEach(block, blocks)
      {
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
         if (kind && !strcmp(kind, "tool_use"))
         {
            const char *id =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "tool_id"));
            if (!id || !id[0] || count == 4096)
            {
               cJSON_Delete(copy);
               goto fail;
            }
            pending[count++] = id;
         }
      }
      if (cJSON_GetArraySize(copied_blocks) || !cJSON_GetArraySize(blocks))
         cJSON_AddItemToArray(rebuilt, copy);
      else
         cJSON_Delete(copy);
   }
   int n = flush_pending(rebuilt, pending, count);
   if (n < 0)
      goto fail;
   repairs += n;
   if (repairs)
   {
      while (messages->child)
         cJSON_Delete(cJSON_DetachItemViaPointer(messages, messages->child));
      while (rebuilt->child)
         cJSON_AddItemToArray(messages, cJSON_DetachItemViaPointer(rebuilt, rebuilt->child));
   }
   free(pending);
   cJSON_Delete(rebuilt);
   return repairs;
fail:
   free(pending);
   cJSON_Delete(rebuilt);
   return -1;
}

cJSON *aimee_ir_delta_json(const aimee_delta_t *delta)
{
   static const char *const types[] = {"TURN_START", "BLOCK_START", "BLOCK_DELTA",
                                       "BLOCK_STOP", "TURN_STOP",   "ERROR"};
   if (!delta || delta->type < AIMEE_DELTA_TURN_START || delta->type > AIMEE_DELTA_ERROR ||
       (delta->type == AIMEE_DELTA_TURN_STOP &&
        (delta->usage_in < 0 || delta->usage_out < 0 || delta->usage_cache_read < 0 ||
         delta->usage_cache_write < 0 || delta->usage_reasoning < 0)))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   int ok = add_string(out, "type", types[delta->type]);
   if (delta->type >= AIMEE_DELTA_BLOCK_START && delta->type <= AIMEE_DELTA_BLOCK_STOP)
   {
      ok = ok && delta->block_id >= 0 && delta->block_id <= 4095 &&
           (delta->kind == AIMEE_BLK_TEXT || delta->kind == AIMEE_BLK_THINKING ||
            delta->kind == AIMEE_BLK_TOOL_USE) &&
           block_kind(delta->kind) &&
           cJSON_AddNumberToObject(out, "block_id", delta->block_id) != NULL &&
           add_string(out, "kind", block_kind(delta->kind));
      if (delta->type == AIMEE_DELTA_BLOCK_START && delta->kind == AIMEE_BLK_TOOL_USE &&
          (!delta->tool_id || !delta->tool_id[0] || !delta->tool_name || !delta->tool_name[0]))
         ok = 0;
      if (delta->type == AIMEE_DELTA_BLOCK_DELTA &&
          ((delta->text_delta && delta->kind != AIMEE_BLK_TEXT &&
            delta->kind != AIMEE_BLK_THINKING) ||
           (delta->tool_args_delta && delta->kind != AIMEE_BLK_TOOL_USE)))
         ok = 0;
      if (delta->type == AIMEE_DELTA_BLOCK_START)
      {
         ok = ok && add_string(out, "tool_id", delta->tool_id) &&
              add_string(out, "name", delta->tool_name) &&
              add_string(out, "namespace", delta->tool_namespace);
         if (delta->tool_input)
            ok = ok && delta->kind == AIMEE_BLK_TOOL_USE && cJSON_IsObject(delta->tool_input) &&
                 add_copy(out, "input", delta->tool_input);
         if (delta->redacted_content)
            ok = ok && delta->kind == AIMEE_BLK_THINKING &&
                 add_string(out, "redacted_content", delta->redacted_content);
      }
      if (delta->type == AIMEE_DELTA_BLOCK_START || delta->type == AIMEE_DELTA_BLOCK_DELTA)
      {
         if (delta->signature_delta)
            ok = ok && delta->kind == AIMEE_BLK_THINKING &&
                 add_string(out, "signature_delta", delta->signature_delta);
         if (delta->type == AIMEE_DELTA_BLOCK_START && delta->text_delta)
            ok = ok && (delta->kind == AIMEE_BLK_TEXT || delta->kind == AIMEE_BLK_THINKING) &&
                 add_string(out, "text_delta", delta->text_delta);
      }
      if (delta->type == AIMEE_DELTA_BLOCK_DELTA)
         ok = ok && add_string(out, "text_delta", delta->text_delta) &&
              add_string(out, "tool_args_delta", delta->tool_args_delta);
   }
   if (delta->type == AIMEE_DELTA_TURN_STOP)
      ok = ok && add_string(out, "stop_reason", aimee_stop_reason_name(delta->stop_reason)) &&
           cJSON_AddNumberToObject(out, "usage_in", delta->usage_in) != NULL &&
           cJSON_AddNumberToObject(out, "usage_out", delta->usage_out) != NULL;
   if (delta->type == AIMEE_DELTA_TURN_STOP)
   {
      if (delta->usage_cache_read)
         ok = ok &&
              cJSON_AddNumberToObject(out, "usage_cache_read", delta->usage_cache_read) != NULL;
      if (delta->usage_cache_write)
         ok = ok &&
              cJSON_AddNumberToObject(out, "usage_cache_write", delta->usage_cache_write) != NULL;
      if (delta->usage_reasoning)
         ok = ok && cJSON_AddNumberToObject(out, "usage_reasoning", delta->usage_reasoning) != NULL;
   }
   if (delta->type == AIMEE_DELTA_ERROR)
      ok = ok && add_string(out, "error_message", delta->error_message);
   if (!ok)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

cJSON *aimee_ir_session_event_json(const aimee_session_event_t *event)
{
   if (!event)
      return NULL;
   cJSON *out = cJSON_CreateObject(), *payload = NULL;
   const char *kind = NULL;
   switch (event->kind)
   {
   case AIMEE_SESSION_DELTA:
      if (!event->role || !event->role[0] || !event->message_id || !event->message_id[0])
         goto fail;
      kind = "delta";
      payload = aimee_ir_delta_json(event->delta);
      break;
   case AIMEE_SESSION_MESSAGE:
      if (!event->message_id || !event->message_id[0])
         goto fail;
      kind = "message";
      payload = aimee_ir_message_json(event->message);
      break;
   case AIMEE_SESSION_CONTROL_REQUEST:
   case AIMEE_SESSION_CONTROL_RESPONSE:
      if (!event->request_id || !event->request_id[0] || !event->control_kind ||
          !event->control_kind[0] || !cJSON_IsObject(event->control))
         goto fail;
      kind = event->kind == AIMEE_SESSION_CONTROL_REQUEST ? "control_request" : "control_response";
      payload = cJSON_Duplicate(event->control, 1);
      break;
   case AIMEE_SESSION_EXECUTION:
      if (!event->execution_status || (strcmp(event->execution_status, "running") &&
                                       strcmp(event->execution_status, "completed") &&
                                       strcmp(event->execution_status, "failed") &&
                                       strcmp(event->execution_status, "cancelled")))
         goto fail;
      kind = "execution";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "status", event->execution_status) ||
          !add_string(payload, "error", event->execution_error))
         goto fail;
      break;
   default:
      goto fail;
   }
   if (!out || !payload || !add_string(out, "kind", kind) ||
       !add_string(out, "message_id", event->message_id) || !add_string(out, "role", event->role) ||
       !add_string(out, "request_id", event->request_id) ||
       !add_string(out, "control_kind", event->control_kind))
      goto fail;
   if (!cJSON_AddItemToObject(out, "payload", payload))
      goto fail;
   return out;
fail:
   cJSON_Delete(payload);
   cJSON_Delete(out);
   return NULL;
}

void aimee_ir_session_set_sink(aimee_ir_session_sink_fn sink, void *context)
{
   session_sink = sink;
   session_context = sink ? context : NULL;
   if (!sink)
      session_capture_failed = 0;
}

void aimee_ir_session_begin_task(void)
{
   if (!session_sink)
      session_capture_failed = 0;
}

void aimee_ir_session_restore_sink(aimee_ir_session_sink_fn sink, void *context, int failed)
{
   session_sink = sink;
   session_context = sink ? context : NULL;
   session_capture_failed = session_capture_failed || failed;
}

aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context)
{
   if (context)
      *context = session_context;
   return session_sink;
}

int aimee_ir_session_emit(const aimee_session_event_t *event)
{
   int rc = session_sink ? session_sink(event, session_context) : 0;
   if (rc)
      session_capture_failed = 1;
   return rc;
}

void aimee_ir_session_fail_capture(void)
{
   if (session_sink)
      session_capture_failed = 1;
}

int aimee_ir_session_capture_failed(void)
{
   return session_capture_failed;
}

int aimee_ir_session_emit_response(const aimee_response_t *response, const char *message_id)
{
   if (!response)
      return -1;
   static _Thread_local unsigned long long counter;
   char generated[64];
   if (!message_id)
   {
      snprintf(generated, sizeof(generated), "response-%llu", ++counter);
      message_id = generated;
   }
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message = &message, .message_id = message_id};
   return aimee_ir_session_emit(&event);
}

int aimee_ir_session_emit_buffered_response(const aimee_response_t *response,
                                            const char *message_id)
{
   if (!response)
      return -1;
   static _Thread_local unsigned long long counter;
   char generated[64];
   if (!message_id)
   {
      snprintf(generated, sizeof generated, "buffered-%llu", ++counter);
      message_id = generated;
   }
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                  .role = response->role ? response->role : "assistant",
                                  .message_id = message_id,
                                  .delta = &delta};
   int rc = aimee_ir_session_emit(&event);
   if (rc)
      return rc;
   if (response->error_message)
   {
      delta = (aimee_delta_t){.type = AIMEE_DELTA_ERROR, .error_message = response->error_message};
      rc = aimee_ir_session_emit(&event);
   }
   else
      rc = aimee_ir_session_emit_response(response, message_id);
   if (rc)
      return rc;
   delta = (aimee_delta_t){.type = AIMEE_DELTA_TURN_STOP,
                           .stop_reason = response->stop_reason,
                           .usage_in = response->usage_in,
                           .usage_out = response->usage_out,
                           .usage_cache_read = response->usage_cache_read,
                           .usage_cache_write = response->usage_cache_write,
                           .usage_reasoning = response->usage_reasoning};
   return aimee_ir_session_emit(&event);
}

int aimee_ir_session_emit_tool_result(const char *tool_id, const char *result, int is_error)
{
   static _Thread_local unsigned long long counter;
   char id[64];
   snprintf(id, sizeof(id), "tool-result-%llu", ++counter);
   cJSON *content = cJSON_CreateString(result ? result : "");
   if (!content)
      return -1;
   aimee_block_t block = {.type = AIMEE_BLK_TOOL_RESULT,
                          .tool_id = (char *)tool_id,
                          .tool_result = content,
                          .tool_is_error = is_error};
   aimee_message_t message = {.role = "tool", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &message};
   int rc = aimee_ir_session_emit(&event);
   cJSON_Delete(content);
   return rc;
}
