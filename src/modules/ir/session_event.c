#include <aimee/ir/session_event.h>
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local aimee_ir_session_sink_fn session_sink;
static _Thread_local void *session_context;

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
   if (!block || !block_kind(block->type))
      return NULL;
   if ((block->type == AIMEE_BLK_TOOL_USE || block->type == AIMEE_BLK_TOOL_RESULT) &&
       (!block->tool_id || !block->tool_id[0]))
      return NULL;
   if (block->type == AIMEE_BLK_TOOL_USE && (!block->tool_name || !block->tool_name[0]))
      return NULL;
   if ((block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT) &&
       (!block->media_type || !block->media_type[0] || !block->media_ref || !block->media_ref[0]))
      return NULL;
   /* UNKNOWN needs a native entry adapter before it is admitted to a canonical
    * session. Passing raw sidecars through would make every consumer an adapter. */
   if (block->type == AIMEE_BLK_UNKNOWN)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   int ok = add_string(out, "type", block_kind(block->type));
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
         ok = ok && add_string(out, "signature", block->thinking_signature);
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

cJSON *aimee_ir_delta_json(const aimee_delta_t *delta)
{
   static const char *const types[] = {"TURN_START", "BLOCK_START", "BLOCK_DELTA",
                                       "BLOCK_STOP", "TURN_STOP",   "ERROR"};
   if (!delta || delta->type < AIMEE_DELTA_TURN_START || delta->type > AIMEE_DELTA_ERROR ||
       (delta->type == AIMEE_DELTA_TURN_STOP && (delta->usage_in < 0 || delta->usage_out < 0)))
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
         ok = ok && add_string(out, "tool_id", delta->tool_id) &&
              add_string(out, "name", delta->tool_name);
      if (delta->type == AIMEE_DELTA_BLOCK_DELTA)
         ok = ok && add_string(out, "text_delta", delta->text_delta) &&
              add_string(out, "tool_args_delta", delta->tool_args_delta);
   }
   if (delta->type == AIMEE_DELTA_TURN_STOP)
      ok = ok && add_string(out, "stop_reason", aimee_stop_reason_name(delta->stop_reason)) &&
           cJSON_AddNumberToObject(out, "usage_in", delta->usage_in) != NULL &&
           cJSON_AddNumberToObject(out, "usage_out", delta->usage_out) != NULL;
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
}

aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context)
{
   if (context)
      *context = session_context;
   return session_sink;
}

int aimee_ir_session_emit(const aimee_session_event_t *event)
{
   return session_sink ? session_sink(event, session_context) : 0;
}

int aimee_ir_session_emit_response(const aimee_response_t *response, const char *message_id)
{
   if (!response)
      return -1;
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message = &message, .message_id = message_id};
   return aimee_ir_session_emit(&event);
}
