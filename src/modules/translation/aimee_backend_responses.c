/* aimee_backend_responses.c -- IR <-> OpenAI Responses API (codex). See
 * aimee_backend.h. This is the backend for the user's key case: Claude Code
 * (Anthropic frontend) served by codex (Responses backend), all via the IR.
 *
 * NOTE (Slice 2): TOOL_RESULT -> function_call_output items (the split) applied per
 * the tool_result-grouping ruling in the follow-up; this covers instructions,
 * message text, function_call (tool_use), tools, and response parse. */
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_ir_stream.h>

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
   if (!wire_response_metadata_valid(resp) || wire_usage_validate(usage) ||
       aimee_ir_read_usage(usage, "input_tokens", &out->usage_in) ||
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
   if (cJSON_GetObjectItemCaseSensitive(resp, "status") && !status)
   {
      aimee_response_free(out);
      return -1;
   }
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
            const cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role"),
                        *parts = cJSON_GetObjectItemCaseSensitive(item, "content");
            if (!wire_response_metadata_valid(item) ||
                (role && (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant"))) ||
                (parts && !cJSON_IsArray(parts)))
            {
               aimee_response_free(out);
               return -1;
            }
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
   else if (status && strcmp(status, "completed"))
      out->stop_reason = AIMEE_STOP_UNKNOWN;
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

#define RESPONSES_STREAM_BYTES (16u * 1024u * 1024u)

typedef struct
{
   int output, part, summary;
   char *arguments;
} response_slot_t;

/* Aliased native text observations share one semantic value per actual item/part.
 * This is bounded edge assembly state, never a second model request/history. */
struct response_text
{
   char *item_id, *text;
   int output, part;
   unsigned closed;
};

struct aimee_responses_stream
{
   aimee_ir_response_builder_t builder;
   aimee_response_t final;
   response_slot_t *slots;
   cJSON **items;
   cJSON *items_done, *terminal_object;
   struct response_text *texts;
   int n_texts, refused;
   char *response_id;
   size_t bytes;
   int failed, terminal, taken, groups;
   int message_groups[AIMEE_IR_MAX_BLOCKS];
};

static const char *string(const cJSON *object, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
   return cJSON_IsString(value) ? value->valuestring : NULL;
}

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
   const char *text = string(value, "text");
   return text ? text : string(value, "value");
}

static int response_item_same(const cJSON *a, const cJSON *b, int same_index)
{
   if (!cJSON_IsObject(a) || !cJSON_IsObject(b))
      return 0;
   const char *at = string(a, "type"), *bt = string(b, "type");
   if (!at || !bt || strcmp(at, bt))
      return 0;
   const char *aid = string(a, "id"), *bid = string(b, "id");
   if (aid && bid)
      return !strcmp(aid, bid);
   if (!strcmp(at, "function_call"))
   {
      aid = string(a, "call_id");
      bid = string(b, "call_id");
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

static int same(const char *left, const char *right);
static int response_text_materialize(aimee_responses_stream_t *stream, cJSON *output);

static cJSON *native_object(aimee_responses_stream_t *stream)
{
   if (!stream->terminal_object)
      return NULL;
   if (!cJSON_GetObjectItemCaseSensitive(stream->terminal_object, "error"))
   {
      const cJSON *native_output =
          cJSON_GetObjectItemCaseSensitive(stream->terminal_object, "output");
      if (native_output && !cJSON_IsArray(native_output))
         return NULL;
      cJSON *output = response_merge_output(stream->items_done, native_output);
      if (!output)
         return NULL;
      if (response_text_materialize(stream, output))
      {
         cJSON_Delete(output);
         return NULL;
      }
      cJSON_DeleteItemFromObjectCaseSensitive(stream->terminal_object, "output");
      if (!cJSON_AddItemToObject(stream->terminal_object, "output", output))
      {
         cJSON_Delete(output);
         return NULL;
      }
   }
   return stream->terminal_object;
}

static int index_value(const cJSON *object, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
   return cJSON_IsNumber(value) && value->valuedouble == value->valueint && value->valueint >= 0 &&
                  value->valueint < AIMEE_IR_MAX_BLOCKS
              ? value->valueint
              : -1;
}

static int response_identity(aimee_responses_stream_t *stream, const cJSON *frame)
{
   const cJSON *nested = cJSON_GetObjectItemCaseSensitive(frame, "response");
   if (nested && !cJSON_IsObject(nested))
      return -1;
   const cJSON *ids[] = {cJSON_GetObjectItemCaseSensitive(frame, "response_id"),
                         cJSON_GetObjectItemCaseSensitive(nested, "id")};
   for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
   {
      if (!ids[i] || cJSON_IsNull(ids[i]))
         continue;
      const char *id = cJSON_GetStringValue(ids[i]);
      if (!id || !*id || (stream->response_id && strcmp(stream->response_id, id)))
         return -1;
      if (!stream->response_id && !(stream->response_id = strdup(id)))
         return -1;
   }
   return 0;
}

static int response_text_observe(aimee_responses_stream_t *stream, const char *event,
                                 const cJSON *frame)
{
   int delta = !strcmp(event, "response.output_text.delta");
   int part = !strcmp(event, "response.content_part.done");
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(frame, part    ? "part"
                                                                : delta ? "delta"
                                                                        : "text");
   if (part && (!string(value, "type") || strcmp(string(value, "type"), "output_text")))
      return 0;
   const char *text = part ? string(value, "text") : response_text_value(value);
   const cJSON *oid = cJSON_GetObjectItemCaseSensitive(frame, "output_index"),
               *pid = cJSON_GetObjectItemCaseSensitive(frame, "content_index"),
               *iid = cJSON_GetObjectItemCaseSensitive(frame, "item_id");
   const char *id = iid ? cJSON_GetStringValue(iid) : NULL;
   int output = oid ? index_value(frame, "output_index") : -1,
       content = pid ? index_value(frame, "content_index") : 0;
   if (!text || (oid && output < 0) || content < 0 || (iid && (!id || !*id)))
      return -1;
   struct response_text *entry = NULL;
   for (int i = 0; i < stream->n_texts; i++)
   {
      struct response_text *prior = &stream->texts[i];
      if ((id && prior->item_id && !strcmp(id, prior->item_id)) ||
          (output >= 0 && output == prior->output) ||
          (!id && output < 0 && !prior->item_id && prior->output < 0))
      {
         if ((id && prior->item_id && strcmp(id, prior->item_id)) ||
             (output >= 0 && prior->output >= 0 && output != prior->output))
            return -1;
         if (prior->part == content)
            entry = prior;
      }
   }
   if (!entry)
   {
      if (stream->n_texts == AIMEE_IR_MAX_BLOCKS)
         return -1;
      entry = &stream->texts[stream->n_texts++];
      entry->output = output;
      entry->part = content;
   }
   if (id && !entry->item_id && !(entry->item_id = strdup(id)))
      return -1;
   if (output >= 0)
      entry->output = output;
   if (delta)
      return entry->closed ? -1 : response_append(&entry->text, text, strlen(text), 0);
   unsigned alias = part ? 2u : 1u;
   if ((entry->closed & alias) ||
       (entry->text && (entry->closed ? strcmp(entry->text, text)
                                      : strncmp(text, entry->text, strlen(entry->text)))))
      return -1;
   char *copy = strdup(text);
   if (!copy)
      return -1;
   free(entry->text);
   entry->text = copy;
   entry->closed |= alias;
   return 0;
}

static int response_text_materialize(aimee_responses_stream_t *stream, cJSON *output)
{
   int authored = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, output)
   {
      if (!same(string(item, "type"), "message"))
         continue;
      const cJSON *part;
      cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(item, "content")) authored |=
          same(string(part, "type"), "output_text") && string(part, "text") &&
          *string(part, "text");
   }
   for (int n = 0; n < stream->n_texts; n++)
   {
      const struct response_text *text = &stream->texts[n];
      int bound = text->item_id || text->output >= 0;
      if (!bound && authored)
         continue; /* Anonymous legacy hints cannot override a completed authored snapshot. */
      cJSON *target = NULL;
      for (int i = 0; i < cJSON_GetArraySize(output); i++)
      {
         cJSON *candidate = cJSON_GetArrayItem(output, i);
         if ((text->item_id && same(text->item_id, string(candidate, "id"))) ||
             (!text->item_id && text->output == i))
            target = candidate;
      }
      if (target && text->output >= 0 && text->item_id)
      {
         const cJSON *at = cJSON_GetArrayItem(stream->items_done, text->output);
         if (!same(text->item_id, string(at, "id")) &&
             target != cJSON_GetArrayItem(output, text->output))
            return -1;
      }
      if (!target)
      {
         if (bound && authored)
            return -1; /* A foreign closed alias is not another admitted message. */
         target = cJSON_CreateObject();
         if (!target || !cJSON_AddStringToObject(target, "type", "message") ||
             (text->item_id && !cJSON_AddStringToObject(target, "id", text->item_id)) ||
             !cJSON_AddArrayToObject(target, "content") || !cJSON_AddItemToArray(output, target))
         {
            cJSON_Delete(target);
            return -1;
         }
      }
      if (!same(string(target, "type"), "message"))
         return -1;
      cJSON *content = cJSON_GetObjectItemCaseSensitive(target, "content");
      if (!content || cJSON_IsNull(content))
      {
         cJSON *array = cJSON_CreateArray();
         int ok =
             array && (content ? cJSON_ReplaceItemInObjectCaseSensitive(target, "content", array)
                               : cJSON_AddItemToObject(target, "content", array));
         if (!ok)
         {
            cJSON_Delete(array);
            return -1;
         }
         content = array;
      }
      if (!cJSON_IsArray(content) || text->part > cJSON_GetArraySize(content))
         return -1;
      cJSON *part = cJSON_GetArrayItem(content, text->part);
      if (!part)
      {
         part = cJSON_CreateObject();
         if (!part || !cJSON_AddStringToObject(part, "type", "output_text") ||
             !cJSON_AddItemToArray(content, part))
         {
            cJSON_Delete(part);
            return -1;
         }
      }
      const cJSON *previous_value = cJSON_GetObjectItemCaseSensitive(part, "text");
      const char *previous = cJSON_GetStringValue(previous_value);
      if ((previous_value && !cJSON_IsNull(previous_value) && !previous) ||
          !same(string(part, "type"), "output_text") ||
          (previous && *previous &&
           (text->closed ? strcmp(previous, text->text ? text->text : "")
                         : strncmp(previous, text->text ? text->text : "",
                                   text->text ? strlen(text->text) : 0))))
         return -1;
      if (!previous || !*previous)
      {
         cJSON *value = cJSON_CreateString(text->text ? text->text : "");
         int ok =
             value && (previous_value ? cJSON_ReplaceItemInObjectCaseSensitive(part, "text", value)
                                      : cJSON_AddItemToObject(part, "text", value));
         if (!ok)
         {
            cJSON_Delete(value);
            return -1;
         }
      }
   }
   return 0;
}

aimee_responses_stream_t *aimee_responses_stream_new(void)
{
   aimee_responses_stream_t *stream = calloc(1, sizeof *stream);
   if (!stream)
      return NULL;
   stream->slots = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *stream->slots);
   stream->items = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *stream->items);
   stream->items_done = cJSON_CreateArray();
   stream->texts = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *stream->texts);
   if (!stream->slots || !stream->items || !stream->items_done || !stream->texts)
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
   }
   free(stream->slots);
   free(stream->items);
   cJSON_Delete(stream->items_done);
   cJSON_Delete(stream->terminal_object);
   for (int i = 0; i < stream->n_texts; i++)
   {
      free(stream->texts[i].item_id);
      free(stream->texts[i].text);
   }
   free(stream->texts);
   free(stream->response_id);
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
         cJSON *parsed =
             prefix && arguments ? aimee_ir_json_parse_frame(prefix, strlen(prefix)) : NULL;
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
   if (block.type == AIMEE_BLK_TOOL_USE &&
       response_append(&stream->slots[id].arguments, text, strlen(text), 0))
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
   const char *keys[] = {"id",      "type",      "role",      "call_id",
                         "name",    "namespace", "arguments", "encrypted_content",
                         "summary", "content"};
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
         cJSON *aa = aimee_ir_json_parse_frame(a->valuestring, strlen(a->valuestring));
         cJSON *bb = aimee_ir_json_parse_frame(b->valuestring, strlen(b->valuestring));
         int equal = cJSON_IsObject(aa) && cJSON_IsObject(bb) && cJSON_Compare(aa, bb, 1);
         cJSON_Delete(aa);
         cJSON_Delete(bb);
         if (!equal)
            return 0;
      }
   }
   return 1;
}

static int native_done(aimee_responses_stream_t *stream, const cJSON *payload)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, "item");
   const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, "output_index");
   int position =
       index ? index_value(payload, "output_index") : cJSON_GetArraySize(stream->items_done);
   if (!cJSON_IsObject(item) || position < 0 || position >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   while (cJSON_GetArraySize(stream->items_done) <= position)
   {
      cJSON *empty = cJSON_CreateNull();
      if (!empty || !cJSON_AddItemToArray(stream->items_done, empty))
      {
         cJSON_Delete(empty);
         return -1;
      }
   }
   const char *identity = string(item, "id");
   for (int i = 0; i < cJSON_GetArraySize(stream->items_done); i++)
      if (identity && same(identity, string(cJSON_GetArrayItem(stream->items_done, i), "id")))
         return -1;
   const cJSON *previous = cJSON_GetArrayItem(stream->items_done, position);
   if (!cJSON_IsNull(previous))
      return -1;
   cJSON *copy = cJSON_Duplicate(item, 1);
   if (copy && cJSON_ReplaceItemInArray(stream->items_done, position, copy))
      return 0;
   cJSON_Delete(copy);
   return -1;
}

/* One native event owner. Buffered ingress can omit live item identities because
 * no prefix has been published; live sinks retain all identity/prefix proofs. */
static int feed(aimee_responses_stream_t *stream, const char *event, const cJSON *payload,
                aimee_ir_delta_sink_fn sink, void *context)
{
   const char *type = string(payload, "type");
   if (!event || !*event)
      event = type;
   if (!event)
      return sink ? -1 : 0;
   if ((type && strcmp(type, event)) || !aimee_ir_json_unique(payload))
      return -1;
   const cJSON *part = cJSON_GetObjectItemCaseSensitive(payload, "part");
   int refusal = !strcmp(event, "response.refusal.delta") ||
                 !strcmp(event, "response.refusal.done") ||
                 (part && same(string(part, "type"), "refusal"));
   if (refusal)
   {
      stream->refused = 1;
      if (!sink)
         return 0; /* Retain refusal diagnostics; never grant completion from contradictory text. */
   }
   if (stream->terminal || response_identity(stream, payload))
      return -1;
   if (sink)
   {
      /* Account the same bounded native JSON representation without retaining or
       * reparsing a synthetic SSE copy of the entire turn. */
      char *json = cJSON_PrintUnformatted(payload);
      size_t size = json ? strlen(json) : RESPONSES_STREAM_BYTES + 1u, name = strlen(event);
      free(json);
      if (size > RESPONSES_STREAM_BYTES || name > 127 ||
          stream->bytes > RESPONSES_STREAM_BYTES - size ||
          RESPONSES_STREAM_BYTES - stream->bytes - size < name + 32)
         return -1;
      stream->bytes += size + name + 16;
   }
   if (sink && !stream->builder.started)
   {
      const cJSON *response = cJSON_GetObjectItemCaseSensitive(payload, "response");
      const cJSON *usage = cJSON_GetObjectItemCaseSensitive(response, "usage");
      aimee_delta_t opening = {.type = AIMEE_DELTA_TURN_START};
      if (!wire_response_metadata_valid(response) || wire_usage_validate(usage) ||
          aimee_ir_read_usage(usage, "input_tokens", &opening.usage_in) ||
          aimee_ir_read_usage(usage, "output_tokens", &opening.usage_out) ||
          aimee_ir_read_usage(cJSON_GetObjectItemCaseSensitive(usage, "input_tokens_details"),
                              "cached_tokens", &opening.usage_cache_read) ||
          aimee_ir_read_usage(cJSON_GetObjectItemCaseSensitive(usage, "output_tokens_details"),
                              "reasoning_tokens", &opening.usage_reasoning))
         return -1;
      wire_unknown_delta_usage(&opening,
                               wire_usage_fields(usage, "input_tokens", "output_tokens") == 3);
      if (publish(stream, opening, sink, context))
         return -1;
   }
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
   if ((!strcmp(event, "response.output_text.delta") ||
        !strcmp(event, "response.output_text.done") ||
        !strcmp(event, "response.content_part.done")) &&
       response_text_observe(stream, event, payload))
      return -1;
   if (!strcmp(event, "response.output_text.delta") || !strcmp(event, "response.refusal.delta") ||
       !strcmp(event, "response.reasoning_summary_text.delta") ||
       !strcmp(event, "response.function_call_arguments.delta"))
      return sink ? live_delta(stream, event, payload, sink, context) : 0;
   if (!strcmp(event, "response.output_item.done"))
   {
      int index = index_value(payload, "output_index");
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, "item");
      if (sink && (index < 0 || (stream->items[index] && !item_agrees(stream->items[index], item))))
         return -1;
      return native_done(stream, payload);
   }
   int completed = !strcmp(event, "response.completed"),
       incomplete = !strcmp(event, "response.incomplete");
   int failed = !strcmp(event, "response.failed"), error_event = !strcmp(event, "error");
   if (completed || incomplete || (!sink && (failed || error_event)))
   {
      const cJSON *response = cJSON_GetObjectItemCaseSensitive(payload, "response");
      const char *status = string(response, "status");
      const cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
      {
         const char *expected = completed ? "completed" : incomplete ? "incomplete" : "failed";
         if (!wire_response_metadata_valid(response) ||
             wire_usage_validate(cJSON_GetObjectItemCaseSensitive(response, "usage")) ||
             (status && strcmp(status, expected)) ||
             cJSON_GetArraySize(output) > AIMEE_IR_MAX_BLOCKS)
            return -1;
         for (int i = 0; i < cJSON_GetArraySize(output); i++)
         {
            const cJSON *done = cJSON_GetArrayItem(stream->items_done, i);
            if (cJSON_IsObject(done) && !item_agrees(done, cJSON_GetArrayItem(output, i)))
               return -1;
         }
      }
      cJSON *copy = error_event                ? cJSON_CreateObject()
                    : cJSON_IsObject(response) ? cJSON_Duplicate(response, 1)
                                               : NULL;
      if (!copy)
         return -1;
      if (error_event)
      {
         cJSON *failure = cJSON_Duplicate(payload, 1);
         if (!failure || !cJSON_AddItemToObject(copy, "error", failure))
         {
            cJSON_Delete(failure);
            cJSON_Delete(copy);
            return -1;
         }
      }
      else if (!cJSON_GetObjectItemCaseSensitive(copy, "status") &&
               !cJSON_AddStringToObject(copy, "status",
                                        incomplete ? "incomplete"
                                        : failed   ? "failed"
                                                   : "completed"))
      {
         cJSON_Delete(copy);
         return -1;
      }
      cJSON_Delete(stream->terminal_object);
      stream->terminal_object = copy;
      stream->terminal = 1;
      if (sink)
      {
         cJSON *object = native_object(stream);
         char error[256];
         if (!object || responses_backend_parse(object, &stream->final, error, sizeof error) ||
             stream->final.error_message || aimee_ir_response_admit(&stream->final) ||
             reconcile(stream, sink, context))
            return -1;
         stream->terminal = 1;
      }
   }
   else if (sink && strcmp(event, "response.created") && strcmp(event, "response.in_progress") &&
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

int responses_backend_decode_body(const char *body, aimee_response_t *out, char *error,
                                  size_t capacity)
{
   if (!body || !out || strlen(body) > RESPONSES_STREAM_BYTES)
      return -1;
   memset(out, 0, sizeof *out);
   const char *first = body;
   while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n')
      first++;
   if (*first == '{')
   {
      cJSON *object = aimee_ir_json_parse_frame(body, strlen(body));
      int rc = responses_backend_parse(object, out, error, capacity);
      cJSON_Delete(object);
      return rc;
   }
   aimee_responses_stream_t *stream = aimee_responses_stream_new();
   char event[128] = "", *data = NULL;
   int rc = -1;
   if (!stream)
      return -1;
   const char *cursor = body;
   for (;;)
   {
      const char *line = cursor, *end = strpbrk(cursor, "\r\n");
      size_t size = end ? (size_t)(end - line) : strlen(line);
      cursor = end ? end + ((*end == '\r' && end[1] == '\n') ? 2 : 1) : line + size;
      if (!size)
      {
         if (data && data[0] && strcmp(data, "[DONE]"))
         {
            cJSON *frame = aimee_ir_json_parse_frame(data, strlen(data));
            int failed = !cJSON_IsObject(frame) || feed(stream, event, frame, NULL, NULL);
            cJSON_Delete(frame);
            if (failed)
               goto done;
         }
         event[0] = 0;
         free(data);
         data = NULL;
         if (!*cursor)
            break;
      }
      else if (size >= 6 && !strncmp(line, "event:", 6))
      {
         size_t offset = size > 6 && line[6] == ' ' ? 7 : 6;
         if (size - offset >= sizeof event)
            goto done;
         memcpy(event, line + offset, size - offset);
         event[size - offset] = 0;
      }
      else if (size >= 5 && !strncmp(line, "data:", 5))
      {
         size_t offset = size > 5 && line[5] == ' ' ? 6 : 5;
         if (response_append(&data, line + offset, size - offset, 1))
            goto done;
      }
      if (!*cursor && size)
         continue; /* Flush the last frame even without a trailing blank line. */
   }
   cJSON *object = native_object(stream);
   if (object)
      rc = responses_backend_parse(object, out, error, capacity);
done:
   if (stream->refused)
   {
      if (rc || aimee_ir_response_valid(out))
      {
         aimee_response_free(out);
         out->usage_unobserved = 1;
      }
      /* A refusal observation cannot carry an executable tool or authored
       * successful-answer authority, even if a contradictory terminal did. */
      for (int i = 0; i < out->n_content; i++)
         aimee_block_free_contents(&out->content[i]);
      free(out->content);
      out->content = NULL;
      out->n_content = 0;
      free(out->error_message);
      out->error_message = NULL;
      out->stop_reason = AIMEE_STOP_CONTENT_FILTER;
      free(out->raw_stop_reason);
      out->raw_stop_reason = strdup("refusal");
      rc = out->raw_stop_reason && !aimee_ir_response_admit(out) ? 0 : -1;
   }
   free(data);
   aimee_responses_stream_free(stream);
   return rc;
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
