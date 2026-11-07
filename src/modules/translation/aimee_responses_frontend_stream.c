/* Canonical incremental Responses projection. No upstream event or JSON enters
 * this renderer. Full snapshot completion is explicitly separate from deltas. */
#include <aimee/translation/aimee_ir_stream.h>
#include <aimee/translation/aimee_frontend.h>
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct aimee_responses_frontend
{
   aimee_ir_response_builder_t builder;
   char *id, *model;
   long created;
   int failed, complete, items, blocks_open;
   int output[AIMEE_IR_MAX_BLOCKS], part[AIMEE_IR_MAX_BLOCKS];
   int summary_started[AIMEE_IR_MAX_BLOCKS];
   cJSON *headers[AIMEE_IR_MAX_BLOCKS];
};

aimee_responses_frontend_t *aimee_responses_frontend_new(const char *id, const char *model,
                                                         long created)
{
   if (!id || !model)
      return NULL;
   aimee_responses_frontend_t *state = calloc(1, sizeof *state);
   if (!state)
      return NULL;
   state->id = strdup(id);
   state->model = strdup(model);
   state->created = created;
   if (!state->id || !state->model)
   {
      aimee_responses_frontend_free(state);
      return NULL;
   }
   return state;
}

void aimee_responses_frontend_free(aimee_responses_frontend_t *state)
{
   if (!state)
      return;
   for (int i = 0; i < AIMEE_IR_MAX_BLOCKS; i++)
      cJSON_Delete(state->headers[i]);
   aimee_ir_response_builder_free(&state->builder);
   free(state->id);
   free(state->model);
   free(state);
}

static int send(aimee_sse_emit_fn emit, void *context, const char *name, cJSON *event)
{
   if (!event || !cJSON_AddStringToObject(event, "type", name))
   {
      cJSON_Delete(event);
      return -1;
   }
   char *text = cJSON_PrintUnformatted(event);
   cJSON_Delete(event);
   if (!text)
      return -1;
   emit(context, name, text);
   free(text);
   return 0;
}

static int attach(cJSON *object, const char *name, cJSON *owned)
{
   if (!owned || !cJSON_AddItemToObject(object, name, owned))
   {
      cJSON_Delete(owned);
      return -1;
   }
   return 0;
}

static int replace(cJSON *object, const char *name, cJSON *owned)
{
   if (!owned || !cJSON_ReplaceItemInObjectCaseSensitive(object, name, owned))
   {
      cJSON_Delete(owned);
      return -1;
   }
   return 0;
}

static cJSON *envelope(const cJSON *item, int index, int part, int reasoning)
{
   cJSON *event = cJSON_CreateObject();
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
   if (!event || !id || !cJSON_AddStringToObject(event, "item_id", id) ||
       !cJSON_AddNumberToObject(event, "output_index", index) ||
       (part >= 0 &&
        !cJSON_AddNumberToObject(event, reasoning ? "summary_index" : "content_index", part)))
   {
      cJSON_Delete(event);
      return NULL;
   }
   return event;
}

static cJSON *render(aimee_responses_frontend_t *state, const aimee_response_t *response)
{
   return aimee_openai_render_response(response, state->id, state->model, state->created, 1);
}

static int delta_emit(aimee_responses_frontend_t *state, const aimee_delta_t *delta,
                      aimee_sse_emit_fn emit, void *context)
{
   if (aimee_ir_response_builder_apply(&state->builder, delta))
      return -1;
   if (delta->type == AIMEE_DELTA_BLOCK_START)
      state->blocks_open++;
   if (delta->type == AIMEE_DELTA_BLOCK_STOP)
      state->blocks_open--;
   if (delta->type == AIMEE_DELTA_TURN_START || delta->type == AIMEE_DELTA_BLOCK_STOP)
      return 0;
   if (delta->type != AIMEE_DELTA_BLOCK_START && delta->type != AIMEE_DELTA_BLOCK_DELTA)
      return -1;
   int index = state->output[delta->block_id], part = state->part[delta->block_id];
   cJSON *root = NULL;
   const cJSON *item = NULL;
   if (delta->type == AIMEE_DELTA_BLOCK_START)
   {
      int block = delta->block_id;
      if (block)
      {
         const aimee_block_t *previous = &state->builder.response.content[block - 1];
         index = state->output[block - 1];
         if (delta->kind == AIMEE_BLK_TEXT && previous->type == AIMEE_BLK_TEXT &&
             previous->message_group == delta->message_group)
            part = state->part[block - 1] + 1;
         else
            index++;
      }
      state->output[block] = index;
      state->part[block] = part;
      /* Render only the newly opened block. Re-rendering the whole accumulated
       * response on each text delta would make streaming quadratic in bytes. */
      aimee_block_t view = state->builder.response.content[block];
      view.text = "";
      aimee_response_t single = {.content = &view, .n_content = 1};
      root = render(state, &single);
      cJSON *opened = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "output"), 0);
      if (opened && !(view.type == AIMEE_BLK_THINKING && view.thinking_id))
      {
         size_t capacity = strlen(state->id) + 40;
         char *id = malloc(capacity);
         if (!id)
         {
            cJSON_Delete(root);
            return -1;
         }
         snprintf(id, capacity, "%s-item-%d", state->id, index);
         cJSON *value = cJSON_CreateString(id);
         free(id);
         if (!value || !cJSON_ReplaceItemInObjectCaseSensitive(opened, "id", value))
         {
            cJSON_Delete(value);
            cJSON_Delete(root);
            return -1;
         }
      }
      item = opened;
      if (item && !state->headers[index])
         state->headers[index] = cJSON_Duplicate(item, 1);
   }
   else
      item = state->headers[index];
   int rc = -1, reasoning = delta->kind == AIMEE_BLK_THINKING;
   if (!item || !state->headers[index])
      goto done;
   if (delta->type == AIMEE_DELTA_BLOCK_START)
   {
      if (index >= state->items)
      {
         cJSON *start = cJSON_Duplicate(item, 1), *event = envelope(item, index, -1, 0);
         if (!start || !event)
         {
            cJSON_Delete(start);
            cJSON_Delete(event);
            goto done;
         }
         if (replace(start, "status", cJSON_CreateString("in_progress")) ||
             (delta->kind == AIMEE_BLK_TOOL_USE
                  ? replace(start, "arguments", cJSON_CreateString(""))
                  : replace(start, reasoning ? "summary" : "content", cJSON_CreateArray())))
         {
            cJSON_Delete(start);
            cJSON_Delete(event);
            goto done;
         }
         cJSON_DeleteItemFromObjectCaseSensitive(start, "encrypted_content");
         if (attach(event, "item", start))
         {
            cJSON_Delete(event);
            goto done;
         }
         if (send(emit, context, "response.output_item.added", event))
            goto done;
         state->items++;
      }
   }
   const char *text =
       delta->kind == AIMEE_BLK_TOOL_USE ? delta->tool_args_delta : delta->text_delta;
   if ((delta->kind == AIMEE_BLK_TEXT && delta->type == AIMEE_DELTA_BLOCK_START) ||
       (reasoning && text && *text && !state->summary_started[delta->block_id]))
   {
      cJSON *event = envelope(item, index, part, reasoning), *empty = cJSON_CreateObject();
      if (!event || !empty)
      {
         cJSON_Delete(event);
         cJSON_Delete(empty);
         goto done;
      }
      if (!cJSON_AddStringToObject(empty, "type", reasoning ? "summary_text" : "output_text") ||
          !cJSON_AddStringToObject(empty, "text", ""))
      {
         cJSON_Delete(empty);
         cJSON_Delete(event);
         goto done;
      }
      if (attach(event, "part", empty))
      {
         cJSON_Delete(event);
         goto done;
      }
      if (send(emit, context,
               reasoning ? "response.reasoning_summary_part.added" : "response.content_part.added",
               event))
         goto done;
      state->summary_started[delta->block_id] = 1;
   }
   rc = 0;
   if (text && *text)
   {
      cJSON *event =
          envelope(item, index, delta->kind == AIMEE_BLK_TOOL_USE ? -1 : part, reasoning);
      if (!event || !cJSON_AddStringToObject(event, "delta", text))
      {
         cJSON_Delete(event);
         rc = -1;
         goto done;
      }
      rc = send(emit, context,
                delta->kind == AIMEE_BLK_TOOL_USE ? "response.function_call_arguments.delta"
                : reasoning                       ? "response.reasoning_summary_text.delta"
                                                  : "response.output_text.delta",
                event);
   }
done:
   cJSON_Delete(root);
   return rc;
}

int aimee_responses_frontend_delta(aimee_responses_frontend_t *state, const aimee_delta_t *delta,
                                   aimee_sse_emit_fn emit, void *context)
{
   if (!state || state->failed || state->complete || !delta || !emit)
      return -1;
   /* Refuse before changing projected state or emitting a false completion. */
   if (delta->refusal)
   {
      state->failed = 1;
      return -1;
   }
   if (delta_emit(state, delta, emit, context))
   {
      state->failed = 1;
      return -1;
   }
   return 0;
}

int aimee_responses_frontend_matches(aimee_responses_frontend_t *state,
                                     const aimee_response_t *response)
{
   if (!state || state->failed || state->complete || state->blocks_open ||
       !state->builder.started || !response)
      return -1;
   cJSON *root = render(state, response), *projected = render(state, &state->builder.response);
   int rc = !root || !projected ||
            !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(root, "output"),
                           cJSON_GetObjectItemCaseSensitive(projected, "output"), 1);
   cJSON_Delete(root);
   cJSON_Delete(projected);
   return rc ? -1 : 0;
}

/* The existing complete renderer supplies final items/usage/opaque state. Compare
 * its entire output to the streamed projection before publishing any done event. */
int aimee_responses_frontend_complete(aimee_responses_frontend_t *state,
                                      const aimee_response_t *response, aimee_sse_emit_fn emit,
                                      void *context)
{
   if (!state || state->failed || state->complete || state->blocks_open ||
       !state->builder.started || !response || !emit)
      return -1;
   cJSON *root = render(state, response), *projected = render(state, &state->builder.response);
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
   int rc = -1;
   if (!root || !projected ||
       !cJSON_Compare(output, cJSON_GetObjectItemCaseSensitive(projected, "output"), 1))
      goto done;
   int index = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, output)
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      int reasoning = type && !strcmp(type, "reasoning");
      const cJSON *parts =
          cJSON_GetObjectItemCaseSensitive(item, reasoning ? "summary" : "content");
      for (int part = 0; part < cJSON_GetArraySize(parts); part++)
      {
         const cJSON *content = cJSON_GetArrayItem(parts, part);
         cJSON *event = envelope(item, index, part, reasoning);
         if (!event || !cJSON_AddStringToObject(
                           event, "text",
                           cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "text"))))
         {
            cJSON_Delete(event);
            goto done;
         }
         if (send(emit, context,
                  reasoning ? "response.reasoning_summary_text.done" : "response.output_text.done",
                  event))
            goto done;
         event = envelope(item, index, part, reasoning);
         if (attach(event, "part", cJSON_Duplicate(content, 1)))
         {
            cJSON_Delete(event);
            goto done;
         }
         if (send(emit, context,
                  reasoning ? "response.reasoning_summary_part.done" : "response.content_part.done",
                  event))
            goto done;
      }
      if (type && !strcmp(type, "function_call"))
      {
         cJSON *event = envelope(item, index, -1, 0);
         if (!event ||
             !cJSON_AddStringToObject(
                 event, "arguments",
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "arguments"))))
         {
            cJSON_Delete(event);
            goto done;
         }
         if (send(emit, context, "response.function_call_arguments.done", event))
            goto done;
      }
      cJSON *event = envelope(item, index++, -1, 0);
      if (attach(event, "item", cJSON_Duplicate(item, 1)))
      {
         cJSON_Delete(event);
         goto done;
      }
      if (send(emit, context, "response.output_item.done", event))
         goto done;
   }
   cJSON *event = cJSON_CreateObject();
   if (attach(event, "response", cJSON_Duplicate(root, 1)))
   {
      cJSON_Delete(event);
      goto done;
   }
   rc = send(emit, context,
             response->stop_reason == AIMEE_STOP_MAX_TOKENS ? "response.incomplete"
                                                            : "response.completed",
             event);
   state->complete = !rc;
done:
   if (rc)
      state->failed = 1;
   cJSON_Delete(root);
   cJSON_Delete(projected);
   return rc;
}
