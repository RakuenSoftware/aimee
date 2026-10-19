/* Responses is decoded only at this entry boundary. Live deltas are reconciled
 * against the existing full native decoder before the host can commit history. */
#include <aimee/translation/aimee_ir_stream.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RESPONSES_STREAM_BYTES (16u * 1024u * 1024u)

typedef struct
{
   int output, part, summary;
   char *arguments;
} response_slot_t;

struct aimee_responses_stream
{
   aimee_ir_response_builder_t builder;
   aimee_response_t final;
   response_slot_t *slots;
   cJSON **items;
   cJSON *completed[AIMEE_IR_MAX_BLOCKS];
   char *frames;
   size_t bytes;
   int failed, terminal, taken, groups;
   int message_groups[AIMEE_IR_MAX_BLOCKS];
};

static const char *string(const cJSON *object, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
   return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int index_value(const cJSON *object, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
   return cJSON_IsNumber(value) && value->valuedouble == value->valueint && value->valueint >= 0 &&
                  value->valueint < AIMEE_IR_MAX_BLOCKS
              ? value->valueint
              : -1;
}

static int append(char **target, const char *text)
{
   size_t old = *target ? strlen(*target) : 0, size = strlen(text);
   if (size > RESPONSES_STREAM_BYTES || old > RESPONSES_STREAM_BYTES - size)
      return -1;
   char *grown = realloc(*target, old + size + 1);
   if (!grown)
      return -1;
   memcpy(grown + old, text, size + 1);
   *target = grown;
   return 0;
}

aimee_responses_stream_t *aimee_responses_stream_new(void)
{
   aimee_responses_stream_t *stream = calloc(1, sizeof *stream);
   if (!stream)
      return NULL;
   stream->slots = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *stream->slots);
   stream->items = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *stream->items);
   if (!stream->slots || !stream->items)
   {
      aimee_responses_stream_free(stream);
      return NULL;
   }
   return stream;
}

void aimee_responses_stream_free(aimee_responses_stream_t *stream)
{
   if (!stream)
      return;
   for (int i = 0; i < AIMEE_IR_MAX_BLOCKS; i++)
   {
      if (stream->slots)
         free(stream->slots[i].arguments);
      if (stream->items)
         cJSON_Delete(stream->items[i]);
      cJSON_Delete(stream->completed[i]);
   }
   free(stream->slots);
   free(stream->items);
   free(stream->frames);
   aimee_ir_response_builder_free(&stream->builder);
   aimee_response_free(&stream->final);
   free(stream);
}

static int publish(aimee_responses_stream_t *stream, aimee_delta_t delta,
                   aimee_ir_delta_sink_fn sink, void *context)
{
   if (aimee_ir_response_builder_apply(&stream->builder, &delta))
      return -1;
   return sink(context, &delta);
}

static int slot(aimee_responses_stream_t *stream, int output, int part, const aimee_block_t *block,
                aimee_ir_delta_sink_fn sink, void *context)
{
   int count = stream->builder.response.n_content;
   for (int i = 0; i < count; i++)
      if (stream->slots[i].output == output && stream->slots[i].part == part)
         return stream->builder.response.content[i].type == block->type ? i : -1;
   if (count >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   stream->slots[count].output = output;
   stream->slots[count].part = part;
   stream->slots[count].summary = -1;
   aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_START,
                          .block_id = count,
                          .kind = block->type,
                          .message_group = block->message_group,
                          .tool_id = block->tool_id,
                          .tool_name = block->tool_name,
                          .tool_namespace = block->tool_namespace,
                          .thinking_id = block->thinking_id,
                          .thinking_encrypted_content = block->thinking_encrypted_content};
   return publish(stream, delta, sink, context) ? -1 : count;
}

static int same(const char *left, const char *right)
{
   return (!left && !right) || (left && right && !strcmp(left, right));
}

/* Completed data may extend an emitted prefix; it cannot rewrite that prefix. */
static int reconcile(aimee_responses_stream_t *stream, aimee_ir_delta_sink_fn sink, void *context)
{
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(stream->final.raw, "output");
   if (cJSON_GetArraySize(output) > AIMEE_IR_MAX_BLOCKS)
      return -1;
   int canonical = 0;
   for (int i = 0; i < cJSON_GetArraySize(output); i++)
   {
      const cJSON *item = cJSON_GetArrayItem(output, i);
      const char *type = string(item, "type");
      if (!type ||
          (stream->items[i] && (!same(string(stream->items[i], "id"), string(item, "id")) ||
                                !same(string(stream->items[i], "type"), type))))
         return -1;
      int message = !strcmp(type, "message");
      int parts =
          message ? cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(item, "content")) : 1;
      for (int part = 0; part < parts; part++, canonical++)
      {
         if (canonical >= stream->final.n_content)
            return -1;
         const aimee_block_t *full = &stream->final.content[canonical];
         int id = slot(stream, i, message ? part : -1, full, sink, context);
         if (id != canonical)
            return -1;
         const aimee_block_t *live = &stream->builder.response.content[id];
         if (full->type == AIMEE_BLK_TEXT && full->message_group != live->message_group)
            return -1;
         const char *prefix =
             full->type == AIMEE_BLK_TOOL_USE ? stream->slots[id].arguments : live->text;
         const char *native_arguments =
             full->type == AIMEE_BLK_TOOL_USE ? string(item, "arguments") : NULL;
         char *arguments = native_arguments ? strdup(native_arguments) : NULL;
         const char *complete = arguments ? arguments : full->text ? full->text : "";
         if (full->type == AIMEE_BLK_TOOL_USE &&
             (!arguments || !same(full->tool_id, live->tool_id) ||
              !same(full->tool_name, live->tool_name) ||
              !same(full->tool_namespace, live->tool_namespace)))
         {
            free(arguments);
            return -1;
         }
         /* Argument whitespace/key order may differ in the final object. Compare
          * complete streamed JSON semantically; an unfinished prefix must match. */
         cJSON *parsed = prefix && arguments ? cJSON_ParseWithOpts(prefix, NULL, 1) : NULL;
         if (parsed && !aimee_ir_json_unique(parsed))
         {
            cJSON_Delete(parsed);
            free(arguments);
            return -1;
         }
         int equal = parsed && cJSON_Compare(parsed, full->tool_input, 1);
         cJSON_Delete(parsed);
         size_t length = prefix ? strlen(prefix) : 0;
         if (!equal && strncmp(complete, prefix ? prefix : "", length))
         {
            free(arguments);
            return -1;
         }
         aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_DELTA,
                                .block_id = id,
                                .kind = full->type,
                                .text_delta = arguments ? NULL : complete + length,
                                .tool_args_delta = arguments && !equal ? complete + length : NULL,
                                .thinking_id = full->thinking_id,
                                .thinking_encrypted_content = full->thinking_encrypted_content};
         int failed = publish(stream, delta, sink, context);
         free(arguments);
         if (failed ||
             publish(stream,
                     (aimee_delta_t){
                         .type = AIMEE_DELTA_BLOCK_STOP, .block_id = id, .kind = full->type},
                     sink, context))
            return -1;
      }
   }
   return canonical == stream->final.n_content && canonical == stream->builder.response.n_content
              ? aimee_ir_response_admit(&stream->builder.response)
              : -1;
}

static int live_delta(aimee_responses_stream_t *stream, const char *event, const cJSON *frame,
                      aimee_ir_delta_sink_fn sink, void *context)
{
   int output = index_value(frame, "output_index");
   if (output < 0 || !stream->items[output] ||
       !same(string(frame, "item_id"), string(stream->items[output], "id")))
      return -1;
   const cJSON *item = stream->items[output];
   const char *type = string(item, "type"), *text = string(frame, "delta");
   int part = -1;
   aimee_block_t block = {0};
   if ((!strcmp(event, "response.output_text.delta") || !strcmp(event, "response.refusal.delta")) &&
       type && !strcmp(type, "message"))
   {
      part = index_value(frame, "content_index");
      if (part < 0)
         return -1;
      block.type = AIMEE_BLK_TEXT;
      block.message_group = stream->message_groups[output];
   }
   else if (!strcmp(event, "response.reasoning_summary_text.delta") && type &&
            !strcmp(type, "reasoning"))
   {
      block.type = AIMEE_BLK_THINKING;
      block.thinking_id = (char *)string(item, "id");
      block.thinking_encrypted_content = (char *)string(item, "encrypted_content");
   }
   else if (!strcmp(event, "response.function_call_arguments.delta") && type &&
            !strcmp(type, "function_call"))
   {
      block.type = AIMEE_BLK_TOOL_USE;
      block.tool_id = (char *)string(item, "call_id");
      block.tool_name = (char *)string(item, "name");
      block.tool_namespace = (char *)string(item, "namespace");
   }
   else
      return -1;
   if (!text)
      return -1;
   int id = slot(stream, output, part, &block, sink, context);
   if (id < 0)
      return -1;
   if (block.type == AIMEE_BLK_THINKING)
   {
      int summary = index_value(frame, "summary_index");
      int previous = stream->slots[id].summary;
      if (summary < 0 ||
          (previous < 0 ? summary != 0 : summary != previous && summary != previous + 1))
         return -1;
      if (previous >= 0 && summary != previous && *text &&
          stream->builder.response.content[id].text[0] &&
          publish(stream,
                  (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_DELTA,
                                  .block_id = id,
                                  .kind = block.type,
                                  .text_delta = "\n\n"},
                  sink, context))
         return -1;
      stream->slots[id].summary = summary;
   }
   if (block.type == AIMEE_BLK_TOOL_USE && append(&stream->slots[id].arguments, text))
      return -1;
   return publish(
       stream,
       (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_DELTA,
                       .block_id = id,
                       .kind = block.type,
                       .text_delta = block.type == AIMEE_BLK_TOOL_USE ? NULL : text,
                       .tool_args_delta = block.type == AIMEE_BLK_TOOL_USE ? text : NULL},
       sink, context);
}

/* Completed item snapshots and the terminal snapshot can omit repeated values,
 * but two supplied semantic values cannot disagree. The shared buffered decoder
 * fills omissions only after this streaming consistency check. */
static int item_agrees(const cJSON *left, const cJSON *right)
{
   const char *keys[] = {"id",        "type",      "call_id",           "name",
                         "namespace", "arguments", "encrypted_content", "summary",
                         "content"};
   for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
   {
      const cJSON *a = cJSON_GetObjectItemCaseSensitive(left, keys[i]);
      const cJSON *b = cJSON_GetObjectItemCaseSensitive(right, keys[i]);
      if (!a || !b || cJSON_IsNull(a) || cJSON_IsNull(b) || (cJSON_IsArray(a) && !a->child) ||
          (cJSON_IsArray(b) && !b->child) || (cJSON_IsString(a) && !*a->valuestring) ||
          (cJSON_IsString(b) && !*b->valuestring))
         continue;
      if (!cJSON_Compare(a, b, 1))
      {
         if (strcmp(keys[i], "arguments") || !cJSON_IsString(a) || !cJSON_IsString(b))
            return 0;
         cJSON *aa = cJSON_ParseWithOpts(a->valuestring, NULL, 1);
         cJSON *bb = cJSON_ParseWithOpts(b->valuestring, NULL, 1);
         int equal = cJSON_IsObject(aa) && cJSON_IsObject(bb) && cJSON_Compare(aa, bb, 1);
         cJSON_Delete(aa);
         cJSON_Delete(bb);
         if (!equal)
            return 0;
      }
   }
   return 1;
}

static int feed(aimee_responses_stream_t *stream, const char *event, const cJSON *payload,
                aimee_ir_delta_sink_fn sink, void *context)
{
   const char *type = string(payload, "type");
   if (!event || !*event)
      event = type;
   if (!event || (type && strcmp(type, event)) || !aimee_ir_json_unique(payload))
      return -1;
   char *json = cJSON_PrintUnformatted(payload);
   if (!json)
      return -1;
   size_t size = strlen(json), name = strlen(event);
   if (size > RESPONSES_STREAM_BYTES || name > 127 ||
       stream->bytes > RESPONSES_STREAM_BYTES - size ||
       RESPONSES_STREAM_BYTES - stream->bytes - size < name + 32)
   {
      free(json);
      return -1;
   }
   char *grown = realloc(stream->frames, stream->bytes + size + name + 32);
   if (!grown)
   {
      free(json);
      return -1;
   }
   stream->frames = grown;
   int count =
       snprintf(grown + stream->bytes, size + name + 32, "event: %s\ndata: %s\n\n", event, json);
   free(json);
   stream->bytes += (size_t)count;
   if (!stream->builder.started &&
       publish(stream, (aimee_delta_t){.type = AIMEE_DELTA_TURN_START}, sink, context))
      return -1;
   if (!strcmp(event, "response.output_item.added"))
   {
      int index = index_value(payload, "output_index");
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, "item");
      const char *kind = string(item, "type");
      if (index < 0 || stream->items[index] || (!string(item, "id") || !*string(item, "id")) ||
          !kind ||
          (strcmp(kind, "message") && strcmp(kind, "reasoning") && strcmp(kind, "function_call")))
         return -1;
      for (int i = 0; i < AIMEE_IR_MAX_BLOCKS; i++)
         if (stream->items[i] && same(string(stream->items[i], "id"), string(item, "id")))
            return -1;
      stream->items[index] = cJSON_Duplicate(item, 1);
      if (!stream->items[index])
         return -1;
      if (!strcmp(kind, "message"))
         stream->message_groups[index] = ++stream->groups;
      return 0;
   }
   if (!strcmp(event, "response.output_text.delta") || !strcmp(event, "response.refusal.delta") ||
       !strcmp(event, "response.reasoning_summary_text.delta") ||
       !strcmp(event, "response.function_call_arguments.delta"))
      return live_delta(stream, event, payload, sink, context);
   if (!strcmp(event, "response.output_item.done"))
   {
      int index = index_value(payload, "output_index");
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, "item");
      if (index < 0 || !cJSON_IsObject(item) ||
          (stream->items[index] && !item_agrees(stream->items[index], item)) ||
          (stream->completed[index] && !cJSON_Compare(stream->completed[index], item, 1)))
         return -1;
      if (!stream->completed[index])
         stream->completed[index] = cJSON_Duplicate(item, 1);
      return stream->completed[index] ? 0 : -1;
   }
   if (!strcmp(event, "response.completed") || !strcmp(event, "response.incomplete"))
   {
      const cJSON *response = cJSON_GetObjectItemCaseSensitive(payload, "response");
      const char *status = string(response, "status");
      if (status &&
          strcmp(status, !strcmp(event, "response.completed") ? "completed" : "incomplete"))
         return -1;
      const cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
      if (cJSON_GetArraySize(output) > AIMEE_IR_MAX_BLOCKS)
         return -1;
      for (int i = 0; i < cJSON_GetArraySize(output); i++)
         if (stream->completed[i] &&
             !item_agrees(stream->completed[i], cJSON_GetArrayItem(output, i)))
            return -1;
      cJSON *object = responses_backend_sse_object(stream->frames);
      char error[256];
      int failed = !object || responses_backend_parse(object, &stream->final, error, sizeof error);
      cJSON_Delete(object);
      if (failed || stream->final.error_message || aimee_ir_response_admit(&stream->final) ||
          reconcile(stream, sink, context))
         return -1;
      stream->terminal = 1;
   }
   else if (strcmp(event, "response.created") && strcmp(event, "response.in_progress") &&
            strcmp(event, "response.output_item.done") &&
            strcmp(event, "response.content_part.added") &&
            strcmp(event, "response.content_part.done") &&
            strcmp(event, "response.output_text.done") && strcmp(event, "response.refusal.done") &&
            strcmp(event, "response.function_call_arguments.done") &&
            strcmp(event, "response.reasoning_summary_part.added") &&
            strcmp(event, "response.reasoning_summary_part.done") &&
            strcmp(event, "response.reasoning_summary_text.done"))
      return -1;
   return 0;
}

int aimee_responses_stream_feed(aimee_responses_stream_t *stream, const char *event,
                                const cJSON *payload, aimee_ir_delta_sink_fn sink, void *context)
{
   if (!stream)
      return -1;
   if (stream->failed || stream->terminal || !sink || !cJSON_IsObject(payload))
   {
      stream->failed = 1;
      return -1;
   }
   if (feed(stream, event, payload, sink, context))
   {
      stream->failed = 1;
      return -1;
   }
   return 0;
}

int aimee_responses_stream_take(aimee_responses_stream_t *stream, aimee_response_t *out)
{
   if (!stream || !out || stream->failed || !stream->terminal || stream->taken)
      return -1;
   *out = stream->final;
   memset(&stream->final, 0, sizeof stream->final);
   stream->taken = 1;
   return 0;
}
