/* aimee_backend_responses.c -- IR <-> OpenAI Responses API (codex). See
 * aimee_backend.h. This is the backend for the user's key case: Claude Code
 * (Anthropic frontend) served by codex (Responses backend), all via the IR.
 *
 * NOTE (Slice 2): TOOL_RESULT -> function_call_output items (the split) applied per
 * the tool_result-grouping ruling in the follow-up; this covers instructions,
 * message text, function_call (tool_use), tools, and response parse. */
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>

#include "cJSON.h"
#include "usage_observation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
   return s ? strdup(s) : NULL;
}

static const char *ostr(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* See aimee_backend.h: accepts `summary` as a bare string OR as an array of typed
 * parts, so neither parser has to bet on which shape the wire uses. */
char *responses_reasoning_summary_text(const cJSON *item)
{
   const cJSON *summary = item ? cJSON_GetObjectItemCaseSensitive((cJSON *)item, "summary") : NULL;
   if (!summary)
      return NULL;
   if (cJSON_IsString(summary))
      return (summary->valuestring && summary->valuestring[0]) ? strdup(summary->valuestring)
                                                               : NULL;
   if (!cJSON_IsArray(summary))
      return NULL;

   /* Join the parts' `text` with blank lines, mirroring how a message's content
    * parts are read. Parts without text (a future part kind) are skipped rather
    * than rendered as gaps. */
   char *acc = NULL;
   size_t len = 0;
   const cJSON *part = NULL;
   cJSON_ArrayForEach(part, summary)
   {
      const char *t = ostr(part, "text");
      if (!t || !t[0])
         continue;
      size_t add = strlen(t);
      size_t sep = len ? 2 : 0; /* "\n\n" between parts */
      char *n = realloc(acc, len + sep + add + 1);
      if (!n)
      {
         free(acc);
         return NULL;
      }
      acc = n;
      if (sep)
         memcpy(acc + len, "\n\n", sep);
      memcpy(acc + len + sep, t, add);
      len += sep + add;
      acc[len] = '\0';
   }
   return acc;
}

/* grow out->content by one; return the new zeroed block or NULL on OOM. */
static aimee_block_t *grow_content(aimee_response_t *out)
{
   void *p = realloc(out->content, (size_t)(out->n_content + 1) * sizeof(aimee_block_t));
   if (!p)
      return NULL;
   out->content = p;
   aimee_block_t *b = &out->content[out->n_content];
   memset(b, 0, sizeof *b);
   out->n_content++;
   return b;
}

static char *blocks_text(const aimee_block_t *blocks, int n)
{
   size_t len = 0;
   for (int i = 0; i < n; i++)
      if (blocks[i].type == AIMEE_BLK_TEXT && blocks[i].text)
         len += strlen(blocks[i].text);
   char *s = calloc(len + 1, 1);
   if (!s)
      return NULL;
   for (int i = 0; i < n; i++)
      if (blocks[i].type == AIMEE_BLK_TEXT && blocks[i].text)
         strcat(s, blocks[i].text);
   return s;
}

cJSON *aimee_codex_history_items(const aimee_request_t *history)
{
   cJSON *request = responses_backend_build(history);
   cJSON *items = cJSON_DetachItemFromObjectCaseSensitive(request, "input");
   cJSON_Delete(request);
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(items);
      return NULL;
   }
   return items;
}

int responses_backend_control_losses(const aimee_request_t *ir, aimee_responses_profile_t profile)
{
   cJSON *body = responses_backend_build_profile(ir, profile);
   int losses = body ? aimee_backend_request_control_losses(ir, AIMEE_WIRE_RESPONSES, body) : -1;
   cJSON_Delete(body);
   return losses;
}

cJSON *responses_backend_build(const aimee_request_t *ir)
{
   return responses_backend_build_profile(ir, AIMEE_RESPONSES_CODEX);
}

cJSON *responses_backend_build_profile(const aimee_request_t *ir, aimee_responses_profile_t profile)
{
   if (aimee_ir_request_admit(ir) ||
       (profile != AIMEE_RESPONSES_API && profile != AIMEE_RESPONSES_CODEX))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   aimee_request_t cache = {.prompt_cache_key = ir->prompt_cache_key,
                            .prompt_cache_retention =
                                profile == AIMEE_RESPONSES_API ? ir->prompt_cache_retention : NULL};
   if (!out || openai_family_add_prompt_cache(out, &cache))
   {
      cJSON_Delete(out);
      return NULL;
   }
   if (ir->output_format.kind != AIMEE_OUTPUT_UNSET)
   {
      cJSON *format = openai_family_output_format_build(&ir->output_format, 1);
      cJSON *text = cJSON_AddObjectToObject(out, "text");
      if (!format || !text || !cJSON_AddItemToObject(text, "format", format))
      {
         cJSON_Delete(format);
         cJSON_Delete(out);
         return NULL;
      }
   }
   if (ir->reasoning_effort != AIMEE_EFFORT_UNSET || ir->reasoning_summary)
   {
      cJSON *reasoning = cJSON_AddObjectToObject(out, "reasoning");
      const char *effort = aimee_ir_effort_name(ir->reasoning_effort);
      if (!reasoning ||
          (ir->reasoning_effort != AIMEE_EFFORT_UNSET &&
           (!effort || !cJSON_AddStringToObject(reasoning, "effort", effort))) ||
          (ir->reasoning_summary &&
           !cJSON_AddStringToObject(reasoning, "summary", ir->reasoning_summary)))
         goto fail;
   }
   if (ir->model)
      cJSON_AddStringToObject(out, "model", ir->model);
   /* Generic Responses supports these controls. The authenticated Codex
    * request contract does not; callers must refuse or report their omission. */
   if (profile == AIMEE_RESPONSES_API)
   {
      if (ir->has_max_tokens)
         cJSON_AddNumberToObject(out, "max_output_tokens", ir->max_tokens);
      if (ir->has_temperature)
         cJSON_AddNumberToObject(out, "temperature", ir->temperature);
      if (ir->has_top_p)
         cJSON_AddNumberToObject(out, "top_p", ir->top_p);
      if (ir->metadata && !cJSON_AddItemToObject(out, "metadata", cJSON_Duplicate(ir->metadata, 1)))
         goto fail;
   }
   if (ir->service_tier && !cJSON_AddStringToObject(out, "service_tier", ir->service_tier))
      goto fail;
   /* Responses/codex REQUIREMENTS (verified live: codex 400s with "Store must be
    * set to false" otherwise): store=false, and the API is SSE-based so stream is
    * always on (the caller accumulates + parses the SSE). */
   cJSON_AddBoolToObject(out, "store", 0);
   cJSON_AddBoolToObject(out, "stream", profile == AIMEE_RESPONSES_CODEX || ir->stream);
   cJSON *include = cJSON_AddArrayToObject(out, "include");
   cJSON_AddItemToArray(include, cJSON_CreateString("reasoning.encrypted_content"));
   /* system blocks -> `instructions` (Responses' system field); a default keeps
    * codex happy when the client sent no system. */
   char *instr = ir->n_system > 0 ? blocks_text(ir->system, ir->n_system) : NULL;
   cJSON_AddStringToObject(out, "instructions",
                           (instr && instr[0]) ? instr : "You are an execution agent.");
   free(instr);

   cJSON *input = cJSON_AddArrayToObject(out, "input");
   for (int i = 0; i < ir->n_messages; i++)
   {
      const aimee_message_t *im = &ir->messages[i];
      int is_assistant = im->role && strcmp(im->role, "assistant") == 0;
      /* Preserve block ordering: reasoning may precede a call, and text can
       * appear on either side. Each native item is rendered at this boundary. */
      cJSON *last_message = NULL;
      int last_group = 0;
      for (int j = 0; j < im->n_blocks; j++)
      {
         const aimee_block_t *b = &im->blocks[j];
         cJSON *item = cJSON_CreateObject();
         if (!item)
            goto fail;
         if (b->type == AIMEE_BLK_TOOL_USE)
         {
            cJSON_AddStringToObject(item, "type", "function_call");
            cJSON_AddStringToObject(item, "call_id", b->tool_id ? b->tool_id : "");
            cJSON_AddStringToObject(item, "name", b->tool_name ? b->tool_name : "");
            if (b->tool_namespace && b->tool_namespace[0])
               cJSON_AddStringToObject(item, "namespace", b->tool_namespace);
            char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
            cJSON_AddStringToObject(item, "arguments", args ? args : "{}");
            free(args);
         }
         else if (b->type == AIMEE_BLK_TOOL_RESULT)
         {
            cJSON_AddStringToObject(item, "type", "function_call_output");
            cJSON_AddStringToObject(item, "call_id", b->tool_id ? b->tool_id : "");
            cJSON *result = NULL;
            if (!b->tool_result)
               result = cJSON_CreateString("");
            else if (cJSON_IsString(b->tool_result) || cJSON_IsArray(b->tool_result))
               result = cJSON_Duplicate(b->tool_result, 1);
            else
            {
               char *text = cJSON_PrintUnformatted(b->tool_result);
               result = text ? cJSON_CreateString(text) : NULL;
               free(text);
            }
            if (!result || !cJSON_AddItemToObject(item, "output", result))
            {
               cJSON_Delete(result);
               cJSON_Delete(item);
               goto fail;
            }
         }
         else if (b->type == AIMEE_BLK_THINKING)
         {
            /* A foreign provider's reasoning has no Responses item identity.
             * Keep it in canonical history; only native reasoning is replayable
             * as this provider's opaque state at egress. */
            if (!b->thinking_id)
            {
               cJSON_Delete(item);
               continue;
            }
            cJSON_AddStringToObject(item, "type", "reasoning");
            if (b->thinking_id)
               cJSON_AddStringToObject(item, "id", b->thinking_id);
            if (b->thinking_encrypted_content)
               cJSON_AddStringToObject(item, "encrypted_content", b->thinking_encrypted_content);
            cJSON *summary = cJSON_AddArrayToObject(item, "summary");
            if (b->text && b->text[0])
            {
               cJSON *part = cJSON_CreateObject();
               cJSON_AddStringToObject(part, "type", "summary_text");
               cJSON_AddStringToObject(part, "text", b->text);
               cJSON_AddItemToArray(summary, part);
            }
         }
         else if (b->type == AIMEE_BLK_TEXT || b->type == AIMEE_BLK_IMAGE ||
                  b->type == AIMEE_BLK_DOCUMENT)
         {
            cJSON_AddStringToObject(item, "type", "message");
            cJSON_AddStringToObject(item, "role", im->role ? im->role : "user");
            cJSON *content = cJSON_AddArrayToObject(item, "content");
            cJSON *part = cJSON_CreateObject();
            if (b->type == AIMEE_BLK_TEXT)
            {
               cJSON_AddStringToObject(part, "type", is_assistant ? "output_text" : "input_text");
               cJSON_AddStringToObject(part, "text", b->text ? b->text : "");
            }
            else if (b->type == AIMEE_BLK_IMAGE)
            {
               cJSON_AddStringToObject(part, "type", "input_image");
               char *url = aimee_ir_media_url(b);
               if (!url)
               {
                  cJSON_Delete(part);
                  cJSON_Delete(item);
                  goto fail;
               }
               cJSON_AddStringToObject(part, "image_url", url);
               free(url);
            }
            else
            {
               cJSON_AddStringToObject(part, "type", "input_file");
               char *ref = aimee_ir_media_url(b);
               if (!ref)
               {
                  cJSON_Delete(part);
                  cJSON_Delete(item);
                  goto fail;
               }
               const char *key = !strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)
                                     ? "file_url"
                                 : !strncmp(ref, "file-", 5) ? "file_id"
                                                             : "file_data";
               cJSON_AddStringToObject(part, key, ref);
               free(ref);
            }
            cJSON_AddItemToArray(content, part);
         }
         else
         {
            cJSON_Delete(item);
            goto fail;
         }
         int content_item = b->type == AIMEE_BLK_TEXT || b->type == AIMEE_BLK_IMAGE ||
                            b->type == AIMEE_BLK_DOCUMENT;
         if (content_item && last_message && b->message_group > 0 && last_group == b->message_group)
         {
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(item, "content");
            cJSON *part = cJSON_DetachItemFromArray(parts, 0);
            if (!part || !cJSON_AddItemToArray(
                             cJSON_GetObjectItemCaseSensitive(last_message, "content"), part))
            {
               cJSON_Delete(part);
               cJSON_Delete(item);
               goto fail;
            }
            cJSON_Delete(item);
            continue;
         }
         last_message = content_item ? item : NULL;
         last_group = content_item ? b->message_group : 0;
         if (!cJSON_AddItemToArray(input, item))
         {
            cJSON_Delete(item);
            goto fail;
         }
      }
   }

   if (ir->n_tools > 0)
   {
      cJSON *tools = cJSON_AddArrayToObject(out, "tools");
      for (int i = 0; i < ir->n_tools; i++)
      {
         cJSON *tool = openai_ir_render_tool(&ir->tools[i], 1);
         if (!tool || !cJSON_AddItemToArray(tools, tool))
         {
            cJSON_Delete(tool);
            cJSON_Delete(out);
            return NULL;
         }
      }
   }
   openai_family_add_tool_choice(out, ir, 1);
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

int responses_backend_parse(const cJSON *resp, aimee_response_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!resp || !cJSON_IsObject(resp) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "responses_backend_parse: null/non-object response");
      return -1;
   }
   out->raw = cJSON_Duplicate(resp, 1);
   const cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
   const cJSON *cached = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens_details");
   const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens_details");
   if (aimee_ir_read_usage(usage, "input_tokens", &out->usage_in) ||
       aimee_ir_read_usage(usage, "output_tokens", &out->usage_out) ||
       aimee_ir_read_usage(cached, "cached_tokens", &out->usage_cache_read) ||
       aimee_ir_read_usage(reasoning, "reasoning_tokens", &out->usage_reasoning))
   {
      if (err && errn)
         snprintf(err, errn, "invalid provider usage counter");
      aimee_response_free(out);
      return -1;
   }
   wire_response_usage(out, usage, "input_tokens", "output_tokens");

   const cJSON *failure = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (failure && !cJSON_IsNull(failure))
   {
      const char *message =
          cJSON_IsString(failure) ? failure->valuestring : ostr(failure, "message");
      out->error_message = dupstr(message ? message : "upstream API error");
      out->stop_reason = AIMEE_STOP_ERROR;
      if (!out->error_message)
      {
         aimee_response_free(out);
         return -1;
      }
      return 0;
   }
   out->id = dupstr(ostr(resp, "id"));
   out->model = dupstr(ostr(resp, "model"));
   out->role = dupstr("assistant");
   const char *status = ostr(resp, "status");
   out->raw_stop_reason = dupstr(status);
   if (status && (!strcmp(status, "failed") || !strcmp(status, "cancelled")))
   {
      out->stop_reason = AIMEE_STOP_ERROR;
      out->error_message = dupstr("upstream response failed");
      if (!out->error_message)
         goto oom;
      return 0;
   }

   /* output items: message (output_text parts) / function_call / reasoning */
   const cJSON *output = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "output");
   int saw_tool = 0, saw_refusal = 0, saw_message = 0;
   if (output && cJSON_IsArray(output))
   {
      const cJSON *item = NULL;
      cJSON_ArrayForEach(item, output)
      {
         const char *type = ostr(item, "type");
         if (type && strcmp(type, "function_call") == 0)
         {
            aimee_block_t *b = grow_content(out);
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_TOOL_USE;
            b->raw = cJSON_Duplicate(item, 1);
            b->tool_id = dupstr(ostr(item, "call_id"));
            b->tool_name = dupstr(ostr(item, "name"));
            b->tool_namespace = dupstr(ostr(item, "namespace"));
            const char *args = ostr(item, "arguments");
            b->tool_input = args ? aimee_ir_json_parse_frame(args, strlen(args)) : NULL;
            if (!cJSON_IsObject(b->tool_input))
            {
               if (err && errn)
                  snprintf(err, errn,
                           "completed function arguments require a complete JSON object");
               aimee_response_free(out);
               return -1;
            }
            saw_tool = 1;
         }
         else if (type && strcmp(type, "reasoning") == 0)
         {
            aimee_block_t *b = grow_content(out);
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_THINKING;
            b->raw = cJSON_Duplicate(item, 1);
            b->text = responses_reasoning_summary_text(item);
            b->thinking_id = dupstr(ostr(item, "id"));
            b->thinking_encrypted_content = dupstr(ostr(item, "encrypted_content"));
         }
         else if (type && strcmp(type, "message") == 0)
         {
            /* Keep authored-message grouping as metadata, never invented text. */
            saw_message++;
            const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "content");
            const cJSON *part = NULL;
            if (content && cJSON_IsArray(content))
               cJSON_ArrayForEach(part, content)
               {
                  const char *pt = ostr(part, "type");
                  if (pt && strcmp(pt, "output_text") == 0)
                  {
                     aimee_block_t *b = grow_content(out);
                     if (!b)
                        goto oom;
                     b->message_group = saw_message;
                     b->type = AIMEE_BLK_TEXT;
                     b->text = dupstr(ostr(part, "text"));
                  }
                  else if (pt && !strcmp(pt, "refusal"))
                  {
                     aimee_block_t *b = grow_content(out);
                     if (!b)
                        goto oom;
                     b->message_group = saw_message;
                     b->type = AIMEE_BLK_TEXT;
                     b->text = dupstr(ostr(part, "refusal"));
                     saw_refusal = 1;
                  }
                  else
                  {
                     aimee_block_t *b = grow_content(out);
                     if (!b)
                        goto oom;
                     b->message_group = saw_message;
                     b->type = AIMEE_BLK_UNKNOWN;
                     b->raw = cJSON_Duplicate(part, 1);
                  }
               }
         }
         else
         {
            aimee_block_t *b = grow_content(out);
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_UNKNOWN;
            b->raw = cJSON_Duplicate(item, 1);
         }
      }
   }
   /* status "completed" -> tool_use if a function_call was emitted, else end_turn;
    * "incomplete" -> max_tokens (Responses' truncation status). */
   if (status && strcmp(status, "incomplete") == 0)
      out->stop_reason = AIMEE_STOP_MAX_TOKENS;
   else
      out->stop_reason = saw_refusal ? AIMEE_STOP_CONTENT_FILTER
                         : saw_tool  ? AIMEE_STOP_TOOL_USE
                                     : AIMEE_STOP_END_TURN;

   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "responses_backend_parse: out of memory");
   aimee_response_free(out);
   return -1;
}

/* Buffered SSE framing belongs to this native entry adapter. No execution or
 * provider-neutral history type knows these event names or JSON keys. */
typedef struct
{
   cJSON *items;
   cJSON *terminal;
   char *delta_text;
   char *done_text;
   char *part_text;
} responses_body_t;

static int response_append(char **text, const char *part, size_t size, int newline)
{
   size_t length = *text ? strlen(*text) : 0;
   size_t separator = newline && length ? 1 : 0;
   if (length > SIZE_MAX - separator - 1 || size > SIZE_MAX - length - separator - 1)
      return -1;
   char *grown = realloc(*text, length + separator + size + 1);
   if (!grown)
      return -1;
   if (separator)
      grown[length++] = '\n';
   memcpy(grown + length, part, size);
   grown[length + size] = 0;
   *text = grown;
   return 0;
}

static const char *response_text_value(const cJSON *value)
{
   if (cJSON_IsString(value))
      return value->valuestring;
   const char *text = ostr(value, "text");
   return text ? text : ostr(value, "value");
}

static int response_body_event(responses_body_t *state, const char *event, const char *data)
{
   if (!data || !data[0] || !strcmp(data, "[DONE]"))
      return 0;
   cJSON *frame = aimee_ir_json_parse_frame(data, strlen(data));
   if (!cJSON_IsObject(frame))
   {
      cJSON_Delete(frame);
      return -1;
   }
   const char *kind = event && event[0] ? event : ostr(frame, "type");
   int rc = 0;
   if (!kind)
      goto done;
   if (!strcmp(kind, "response.output_item.done"))
   {
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(frame, "item");
      const cJSON *index = cJSON_GetObjectItemCaseSensitive(frame, "output_index");
      cJSON *copy = cJSON_IsObject(item) ? cJSON_Duplicate(item, 1) : NULL;
      if (!copy)
      {
         rc = -1;
         goto done;
      }
      if (index)
      {
         if (!cJSON_IsNumber(index) || index->valuedouble != index->valueint ||
             index->valueint < 0 || index->valueint > 4095)
         {
            cJSON_Delete(copy);
            rc = -1;
            goto done;
         }
         while (cJSON_GetArraySize(state->items) <= index->valueint)
         {
            cJSON *empty = cJSON_CreateNull();
            if (!empty || !cJSON_AddItemToArray(state->items, empty))
            {
               cJSON_Delete(empty);
               cJSON_Delete(copy);
               rc = -1;
               goto done;
            }
         }
         cJSON *previous = cJSON_GetArrayItem(state->items, index->valueint);
         if (!cJSON_IsNull(previous) && !cJSON_Compare(previous, copy, 1))
         {
            cJSON_Delete(copy);
            rc = -1;
            goto done;
         }
         if (!cJSON_ReplaceItemInArray(state->items, index->valueint, copy))
         {
            cJSON_Delete(copy);
            rc = -1;
         }
      }
      else if (cJSON_GetArraySize(state->items) >= 4096 ||
               !cJSON_AddItemToArray(state->items, copy))
      {
         cJSON_Delete(copy);
         rc = -1;
      }
   }
   else if (!strcmp(kind, "response.output_text.delta") ||
            !strcmp(kind, "response.output_text.done"))
   {
      int delta = !strcmp(kind, "response.output_text.delta");
      const char *text =
          response_text_value(cJSON_GetObjectItemCaseSensitive(frame, delta ? "delta" : "text"));
      if (text)
         rc =
             response_append(delta ? &state->delta_text : &state->done_text, text, strlen(text), 0);
   }
   else if (!strcmp(kind, "response.content_part.done"))
   {
      const cJSON *part = cJSON_GetObjectItemCaseSensitive(frame, "part");
      const char *type = ostr(part, "type"), *text = ostr(part, "text");
      if (type && !strcmp(type, "output_text") && text)
         rc = response_append(&state->part_text, text, strlen(text), 0);
   }
   else if (!strcmp(kind, "response.completed") || !strcmp(kind, "response.incomplete") ||
            !strcmp(kind, "response.failed"))
   {
      const cJSON *response = cJSON_GetObjectItemCaseSensitive(frame, "response");
      cJSON *copy = cJSON_IsObject(response) ? cJSON_Duplicate(response, 1) : NULL;
      if (!copy)
         rc = -1;
      else
      {
         cJSON_Delete(state->terminal);
         state->terminal = copy;
         if (!cJSON_GetObjectItemCaseSensitive(copy, "status"))
            cJSON_AddStringToObject(copy, "status",
                                    !strcmp(kind, "response.incomplete") ? "incomplete"
                                    : !strcmp(kind, "response.failed")   ? "failed"
                                                                         : "completed");
      }
   }
   else if (!strcmp(kind, "error"))
   {
      cJSON *failure = cJSON_CreateObject(), *copy = cJSON_Duplicate(frame, 1);
      if (!failure || !copy || !cJSON_AddItemToObject(failure, "error", copy))
      {
         cJSON_Delete(failure);
         cJSON_Delete(copy);
         rc = -1;
      }
      else
      {
         cJSON_Delete(state->terminal);
         state->terminal = failure;
      }
   }
done:
   cJSON_Delete(frame);
   return rc;
}

static int response_item_same(const cJSON *a, const cJSON *b, int same_index)
{
   if (!cJSON_IsObject(a) || !cJSON_IsObject(b))
      return 0;
   const char *at = ostr(a, "type"), *bt = ostr(b, "type");
   if (!at || !bt || strcmp(at, bt))
      return 0;
   const char *aid = ostr(a, "id"), *bid = ostr(b, "id");
   if (aid && bid)
      return !strcmp(aid, bid);
   if (!strcmp(at, "function_call"))
   {
      aid = ostr(a, "call_id");
      bid = ostr(b, "call_id");
      return aid && bid && !strcmp(aid, bid);
   }
   return same_index;
}

static int response_missing(const cJSON *value)
{
   return !value || cJSON_IsNull(value) || (cJSON_IsArray(value) && !value->child) ||
          (cJSON_IsString(value) && !value->valuestring[0]);
}

static int response_merge_fields(cJSON *out, const cJSON *other)
{
   const cJSON *field;
   cJSON_ArrayForEach(field, other)
   {
      const cJSON *previous = cJSON_GetObjectItemCaseSensitive(out, field->string);
      if (!response_missing(previous) || response_missing(field))
         continue;
      cJSON *copy = cJSON_Duplicate(field, 1);
      if (!copy)
         return -1;
      int ok = previous ? cJSON_ReplaceItemInObjectCaseSensitive(out, field->string, copy)
                        : cJSON_AddItemToObject(out, field->string, copy);
      if (!ok)
      {
         cJSON_Delete(copy);
         return -1;
      }
   }
   return 0;
}

static cJSON *response_merge_output(const cJSON *streamed, const cJSON *terminal)
{
   int streamed_count = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, streamed) streamed_count += cJSON_IsObject(item);
   int terminal_count = cJSON_GetArraySize(terminal);
   if (terminal_count > 4096)
      return NULL;
   const cJSON *primary = streamed_count >= terminal_count ? streamed : terminal;
   const cJSON *secondary = primary == streamed ? terminal : streamed;
   int n = cJSON_GetArraySize(primary), m = cJSON_GetArraySize(secondary);
   int *matches = malloc((size_t)(m ? m : 1) * sizeof *matches);
   cJSON *out = cJSON_CreateArray();
   if (!matches || !out)
      goto fail;
   for (int j = 0; j < m; j++)
   {
      matches[j] = -1;
      const cJSON *candidate = cJSON_GetArrayItem(secondary, j);
      for (int i = 0; i < n; i++)
         if (response_item_same(cJSON_GetArrayItem(primary, i), candidate, i == j))
         {
            matches[j] = i;
            break;
         }
   }
   for (int i = 0; i < n; i++)
   {
      const cJSON *source = cJSON_GetArrayItem(primary, i);
      if (!cJSON_IsObject(source))
      {
         if (i < m && matches[i] < 0 && cJSON_IsObject(cJSON_GetArrayItem(secondary, i)))
         {
            source = cJSON_GetArrayItem(secondary, i);
            matches[i] = i;
         }
         else
            continue;
      }
      cJSON *copy = cJSON_Duplicate(source, 1);
      if (!copy)
         goto fail;
      for (int j = 0; j < m; j++)
         if (matches[j] == i && response_merge_fields(copy, cJSON_GetArrayItem(secondary, j)))
         {
            cJSON_Delete(copy);
            goto fail;
         }
      if (!cJSON_AddItemToArray(out, copy))
      {
         cJSON_Delete(copy);
         goto fail;
      }
   }
   for (int j = 0; j < m; j++)
   {
      const cJSON *source = cJSON_GetArrayItem(secondary, j);
      if (matches[j] >= 0 || !cJSON_IsObject(source))
         continue;
      cJSON *copy = cJSON_Duplicate(source, 1);
      if (!copy || !cJSON_AddItemToArray(out, copy))
      {
         cJSON_Delete(copy);
         goto fail;
      }
   }
   free(matches);
   return out;
fail:
   free(matches);
   cJSON_Delete(out);
   return NULL;
}

cJSON *responses_backend_sse_object(const char *body)
{
   if (!body)
      return NULL;
   /* A buffered non-streamed response has no SSE framing to decode. */
   const char *first = body;
   while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n')
      first++;
   if (*first == '{')
      return aimee_ir_json_parse_frame(body, strlen(body));
   responses_body_t state = {.items = cJSON_CreateArray()};
   char event[128] = "", *data = NULL;
   if (!state.items)
      return NULL;
   const char *cursor = body;
   while (*cursor)
   {
      const char *line = cursor, *end = strpbrk(cursor, "\r\n");
      size_t size = end ? (size_t)(end - line) : strlen(line);
      cursor = end ? end + ((*end == '\r' && end[1] == '\n') ? 2 : 1) : line + size;
      if (!size)
      {
         if (response_body_event(&state, event, data))
            goto fail;
         event[0] = 0;
         free(data);
         data = NULL;
      }
      else if (size >= 6 && !strncmp(line, "event:", 6))
      {
         size_t offset = size > 6 && line[6] == ' ' ? 7 : 6;
         if (size - offset >= sizeof event)
            goto fail;
         memcpy(event, line + offset, size - offset);
         event[size - offset] = 0;
      }
      else if (size >= 5 && !strncmp(line, "data:", 5))
      {
         size_t offset = size > 5 && line[5] == ' ' ? 6 : 5;
         if (response_append(&data, line + offset, size - offset, 1))
            goto fail;
      }
   }
   if (response_body_event(&state, event, data) || !state.terminal)
      goto fail;
   if (!cJSON_GetObjectItemCaseSensitive(state.terminal, "error"))
   {
      const cJSON *native_output = cJSON_GetObjectItemCaseSensitive(state.terminal, "output");
      if (native_output && !cJSON_IsArray(native_output))
         goto fail;
      cJSON *output = response_merge_output(state.items, native_output);
      if (!output)
         goto fail;
      /* The completed item snapshots carry all native content. Text-only
       * providers may supply only text deltas and an empty terminal snapshot. */
      int text_found = 0;
      const cJSON *item;
      cJSON_ArrayForEach(item, output)
      {
         const char *type = ostr(item, "type");
         if (!type || strcmp(type, "message"))
            continue;
         const cJSON *part;
         cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(item, "content"))
         {
            const char *part_type = ostr(part, "type"), *text = ostr(part, "text");
            /* reasoning.content is opaque state, never visible answer text. */
            text_found |= part_type && !strcmp(part_type, "output_text") && text && text[0];
         }
      }
      const char *text = state.part_text;
      if (state.done_text && (!text || strlen(state.done_text) > strlen(text)))
         text = state.done_text;
      if (state.delta_text && (!text || strlen(state.delta_text) > strlen(text)))
         text = state.delta_text;
      if (!text_found && text && text[0])
      {
         cJSON *message = cJSON_CreateObject(), *part = cJSON_CreateObject();
         cJSON *content = message ? cJSON_AddArrayToObject(message, "content") : NULL;
         if (!message || !part || !content ||
             !cJSON_AddStringToObject(message, "type", "message") ||
             !cJSON_AddStringToObject(part, "type", "output_text") ||
             !cJSON_AddStringToObject(part, "text", text))
         {
            cJSON_Delete(message);
            cJSON_Delete(part);
            cJSON_Delete(output);
            goto fail;
         }
         cJSON_AddItemToArray(content, part);
         cJSON_AddItemToArray(output, message);
      }
      cJSON_DeleteItemFromObjectCaseSensitive(state.terminal, "output");
      if (!cJSON_AddItemToObject(state.terminal, "output", output))
      {
         cJSON_Delete(output);
         goto fail;
      }
   }
   free(data);
   free(state.delta_text);
   free(state.done_text);
   free(state.part_text);
   cJSON_Delete(state.items);
   return state.terminal;
fail:
   free(data);
   free(state.delta_text);
   free(state.done_text);
   free(state.part_text);
   cJSON_Delete(state.items);
   cJSON_Delete(state.terminal);
   return NULL;
}

static int omitted_number(const cJSON *body, const char *key, int present, double expected)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, key);
   return present && (!cJSON_IsNumber(value) || value->valuedouble != expected);
}

static int omitted_string(const cJSON *body, const char *key, const char *expected)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, key);
   return expected && (!cJSON_IsString(value) || strcmp(value->valuestring, expected));
}

static int omitted_object(const cJSON *body, const char *key, const cJSON *expected)
{
   return expected && !cJSON_Compare(expected, cJSON_GetObjectItemCaseSensitive(body, key), 1);
}

static int omitted_parallel(const aimee_request_t *source, aimee_wire_t wire, const cJSON *body)
{
   if (!source->has_parallel_tool_calls)
      return 0;
   if (wire == AIMEE_WIRE_ANTHROPIC)
   {
      const cJSON *choice = cJSON_GetObjectItemCaseSensitive(body, "tool_choice");
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(choice, "type"));
      if (type && !strcmp(type, "none"))
         return 0; /* No tool calls also satisfies the parallelism restriction. */
      const cJSON *disabled = cJSON_GetObjectItemCaseSensitive(choice, "disable_parallel_tool_use");
      /* Anthropic's absent flag allows parallel calls by default. */
      return cJSON_IsTrue(disabled) == !!source->parallel_tool_calls;
   }
   const cJSON *parallel = cJSON_GetObjectItemCaseSensitive(body, "parallel_tool_calls");
   return !cJSON_IsBool(parallel) || cJSON_IsTrue(parallel) != !!source->parallel_tool_calls;
}

/* Compare source controls with the completed provider representation. */
int aimee_backend_request_control_losses(const aimee_request_t *source, aimee_wire_t wire,
                                         const cJSON *body)
{
   if (!source || !cJSON_IsObject(body))
      return -1;
   int omitted =
       omitted_number(body, wire == AIMEE_WIRE_RESPONSES ? "max_output_tokens" : "max_tokens",
                      source->has_max_tokens, source->max_tokens) +
       omitted_number(body, "temperature", source->has_temperature, source->temperature) +
       omitted_number(body, "top_p", source->has_top_p, source->top_p) +
       omitted_number(body, "top_k", source->has_top_k, source->top_k);
   omitted += omitted_object(body, "thinking", source->thinking);
   omitted += omitted_object(body, "metadata", source->metadata);
   omitted += omitted_string(body, "service_tier", source->service_tier);
   omitted += omitted_string(body, "prompt_cache_key", source->prompt_cache_key);
   omitted += omitted_string(body, "prompt_cache_retention", source->prompt_cache_retention);
   omitted += omitted_object(body, "prompt_cache_options", source->prompt_cache_options);
   omitted += omitted_parallel(source, wire, body);
   omitted += source->n_stop && !cJSON_HasObjectItem(body, "stop") &&
              !cJSON_HasObjectItem(body, "stop_sequences");
   const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(body, "reasoning");
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(body, "output_config");
   omitted += source->reasoning_summary && !cJSON_HasObjectItem(reasoning, "summary");
   omitted += source->reasoning_effort && !cJSON_HasObjectItem(body, "reasoning_effort") &&
              !cJSON_HasObjectItem(reasoning, "effort") && !cJSON_HasObjectItem(output, "effort");
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "text");
   omitted += source->output_format.kind > AIMEE_OUTPUT_TEXT &&
              !cJSON_HasObjectItem(body, "response_format") &&
              !cJSON_HasObjectItem(text, "format") && !cJSON_HasObjectItem(output, "format");
   return omitted;
}
