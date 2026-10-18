/* aimee_backend_openai.c -- IR <-> OpenAI Chat Completions (upstream provider).
 * See aimee_backend.h.
 *
 * NOTE (Slice 2): TOOL_RESULT blocks -> OpenAI role:"tool" messages (the split) are
 * applied per the tool_result-grouping ruling in the follow-up; this covers
 * system-lowering, text/tool_use, tool definitions, params, and response parse. */
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>

#include "cJSON.h"
#include "util.h" /* text_split_reasoning_prefix, strip_llm_private_scaffold */

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

/* concat the text of a block array into a malloc'd string ("" if none). */
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

cJSON *openai_family_output_format_build(const aimee_output_format_t *format, int responses_shape)
{
   if (!format || format->kind == AIMEE_OUTPUT_UNSET)
      return NULL;
   const char *type = format->kind == AIMEE_OUTPUT_TEXT          ? "text"
                      : format->kind == AIMEE_OUTPUT_JSON_OBJECT ? "json_object"
                      : format->kind == AIMEE_OUTPUT_JSON_SCHEMA ? "json_schema"
                                                                 : NULL;
   if (!type || format->strict < AIMEE_STRICT_UNSET || format->strict > AIMEE_STRICT_TRUE)
      return NULL;
   if (format->kind == AIMEE_OUTPUT_JSON_SCHEMA && format->name)
   {
      size_t length = strlen(format->name);
      if (!length || length > 64)
         return NULL;
      for (size_t i = 0; i < length; i++)
      {
         unsigned char c = (unsigned char)format->name[i];
         if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-'))
            return NULL;
      }
   }
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddStringToObject(out, "type", type))
      goto fail;
   if (format->kind == AIMEE_OUTPUT_JSON_SCHEMA)
   {
      if (!cJSON_IsObject(format->schema))
         goto fail;
      cJSON *definition = responses_shape ? out : cJSON_AddObjectToObject(out, "json_schema");
      if (!definition ||
          !cJSON_AddStringToObject(definition, "name", format->name ? format->name : "response") ||
          !cJSON_AddItemToObject(definition, "schema", cJSON_Duplicate(format->schema, 1)) ||
          (format->description &&
           !cJSON_AddStringToObject(definition, "description", format->description)))
         goto fail;
      if (format->strict == AIMEE_STRICT_NULL)
      {
         if (!cJSON_AddNullToObject(definition, "strict"))
            goto fail;
      }
      else if (format->strict != AIMEE_STRICT_UNSET &&
               !cJSON_AddBoolToObject(definition, "strict", format->strict == AIMEE_STRICT_TRUE))
         goto fail;
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

void openai_family_add_tool_choice(cJSON *out, const aimee_request_t *ir, int responses_shape)
{
   switch (ir->tool_choice)
   {
   case AIMEE_TOOL_CHOICE_AUTO:
      cJSON_AddStringToObject(out, "tool_choice", "auto");
      break;
   case AIMEE_TOOL_CHOICE_NONE:
      cJSON_AddStringToObject(out, "tool_choice", "none");
      break;
   case AIMEE_TOOL_CHOICE_ANY:
      cJSON_AddStringToObject(out, "tool_choice", "required");
      break;
   case AIMEE_TOOL_CHOICE_TOOL:
   {
      cJSON *tc = cJSON_AddObjectToObject(out, "tool_choice");
      if (!tc)
         break;
      cJSON_AddStringToObject(tc, "type", "function");
      const char *name = ir->tool_choice_name ? ir->tool_choice_name : "";
      if (responses_shape)
         cJSON_AddStringToObject(tc, "name", name);
      else
         cJSON_AddStringToObject(cJSON_AddObjectToObject(tc, "function"), "name", name);
      break;
   }
   case AIMEE_TOOL_CHOICE_UNSET:
      break;
   }
   if (ir->has_parallel_tool_calls)
      cJSON_AddBoolToObject(out, "parallel_tool_calls", ir->parallel_tool_calls ? 1 : 0);
}

cJSON *openai_backend_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (ir->output_format.kind != AIMEE_OUTPUT_UNSET)
   {
      cJSON *format = openai_family_output_format_build(&ir->output_format, 0);
      if (!format || !cJSON_AddItemToObject(out, "response_format", format))
      {
         cJSON_Delete(format);
         cJSON_Delete(out);
         return NULL;
      }
   }
   if (ir->model)
      cJSON_AddStringToObject(out, "model", ir->model);
   if (ir->has_max_tokens)
      cJSON_AddNumberToObject(out, "max_tokens", ir->max_tokens);
   if (ir->has_temperature)
      cJSON_AddNumberToObject(out, "temperature", ir->temperature);
   if (ir->has_top_p)
      cJSON_AddNumberToObject(out, "top_p", ir->top_p);
   if (ir->has_top_k) /* OpenAI-compatible local providers (ollama/llama.cpp) accept top_k */
      cJSON_AddNumberToObject(out, "top_k", ir->top_k);
   if (ir->metadata)
      cJSON_AddItemToObject(out, "metadata", cJSON_Duplicate(ir->metadata, 1));
   if (ir->stream)
      cJSON_AddBoolToObject(out, "stream", 1);

   cJSON *msgs = cJSON_AddArrayToObject(out, "messages");
   /* system blocks -> leading system messages, one per block (round-trip stable
    * with the frontend's leading-system lift). */
   for (int i = 0; i < ir->n_system; i++)
   {
      if (ir->system[i].type != AIMEE_BLK_TEXT)
         continue;
      cJSON *sm = cJSON_CreateObject();
      cJSON_AddStringToObject(sm, "role", "system");
      cJSON_AddStringToObject(sm, "content", ir->system[i].text ? ir->system[i].text : "");
      cJSON_AddItemToArray(msgs, sm);
   }
   for (int i = 0; i < ir->n_messages; i++)
   {
      const aimee_message_t *im = &ir->messages[i];
      /* SPLIT (grouping ruling, Option A): a message's tool_result blocks become
       * role:"tool" messages FIRST (one per block, tool_call_id = tool_id verbatim),
       * then the remaining text/tool_use content follows as the message. Rich/image
       * tool_result content is coerced to a string here (documented lossy). */
      int n_tr = 0, n_other = 0;
      for (int j = 0; j < im->n_blocks; j++)
      {
         if (im->blocks[j].type == AIMEE_BLK_TOOL_RESULT)
            n_tr++;
         else
            n_other++;
      }
      for (int j = 0; j < im->n_blocks && n_tr; j++)
      {
         const aimee_block_t *b = &im->blocks[j];
         if (b->type != AIMEE_BLK_TOOL_RESULT)
            continue;
         cJSON *tm = cJSON_CreateObject();
         cJSON_AddStringToObject(tm, "role", "tool");
         cJSON_AddStringToObject(tm, "tool_call_id", b->tool_id ? b->tool_id : "");
         char *content = NULL;
         if (b->tool_result && cJSON_IsString(b->tool_result))
            content = strdup(b->tool_result->valuestring);
         else if (b->tool_result)
            content = cJSON_PrintUnformatted(b->tool_result);
         cJSON_AddStringToObject(tm, "content", content ? content : "");
         free(content);
         cJSON_AddItemToArray(msgs, tm);
      }
      if (n_other == 0 && im->n_blocks > 0)
         continue; /* tool_result-only message: nothing else to emit */

      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "role", im->role ? im->role : "user");
      char *text = blocks_text(im->blocks, im->n_blocks);
      /* assistant tool_use blocks -> tool_calls[]; content may be empty */
      cJSON *tool_calls = NULL;
      for (int j = 0; j < im->n_blocks; j++)
      {
         const aimee_block_t *b = &im->blocks[j];
         if (b->type != AIMEE_BLK_TOOL_USE)
            continue;
         if (!tool_calls)
            tool_calls = cJSON_CreateArray();
         cJSON *call = cJSON_CreateObject();
         cJSON_AddStringToObject(call, "id", b->tool_id ? b->tool_id : "");
         cJSON_AddStringToObject(call, "type", "function");
         /* Chat has no namespace concept, but a Responses request crosses this shape
          * on its way to the provider (responses -> IR -> chat -> IR -> responses),
          * so dropping the group here loses it for good -- the same trap that made
          * the tools fix at the Responses ends insufficient on its own. Carried
          * beside `function` rather than inside it, so a strict `function` schema is
          * untouched, and only when the client actually grouped the tool: a request
          * that never used namespace grouping is byte-identical to before. */
         if (b->tool_namespace && b->tool_namespace[0])
            cJSON_AddStringToObject(call, "namespace", b->tool_namespace);
         cJSON *fn = cJSON_AddObjectToObject(call, "function");
         cJSON_AddStringToObject(fn, "name", b->tool_name ? b->tool_name : "");
         char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
         cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
         free(args);
         cJSON_AddItemToArray(tool_calls, call);
      }
      int has_media = 0;
      for (int j = 0; j < im->n_blocks; j++)
         has_media |=
             im->blocks[j].type == AIMEE_BLK_IMAGE || im->blocks[j].type == AIMEE_BLK_DOCUMENT;
      if (has_media)
      {
         cJSON *parts = openai_ir_render_content(im);
         if (!parts)
         {
            cJSON_Delete(m);
            cJSON_Delete(tool_calls);
            free(text);
            cJSON_Delete(out);
            return NULL;
         }
         cJSON_AddItemToObject(m, "content", parts);
      }
      else if (text && text[0])
         cJSON_AddStringToObject(m, "content", text);
      else if (tool_calls)
         cJSON_AddNullToObject(m, "content");
      else
         cJSON_AddStringToObject(m, "content", "");
      free(text);
      if (tool_calls)
         cJSON_AddItemToObject(m, "tool_calls", tool_calls);
      cJSON_AddItemToArray(msgs, m);
   }

   if (ir->n_tools > 0)
   {
      cJSON *tools = cJSON_AddArrayToObject(out, "tools");
      for (int i = 0; i < ir->n_tools; i++)
      {
         /* Carry an entry this shape cannot express, verbatim -- the same rule the
          * Responses backend follows, and needed here because a Responses request
          * reaches the provider as responses -> IR -> CHAT -> IR -> responses
          * (aimee_ir_responses_to_chat renders chat, then openai_build_body rebuilds).
          * Flattening at this hop undid the sidecar the ends preserve: a Codex
          * `namespace` group became a chat function named mcp__aimee with no schema,
          * its nineteen nested tools gone by the time the request was rebuilt.
          *
          * A tool whose sidecar is a plain named `function` renders normally; anything
          * else (namespace / custom / web_search / local_shell) passes through. */
         const cJSON *raw = ir->tools[i].raw;
         const cJSON *rtype = raw ? cJSON_GetObjectItemCaseSensitive((cJSON *)raw, "type") : NULL;
         int raw_is_function =
             !rtype ||
             (cJSON_IsString(rtype) &&
              (!strcmp(rtype->valuestring, "function") ||
               (ir->frontend == AIMEE_WIRE_ANTHROPIC && !strcmp(rtype->valuestring, "custom"))));
         if (ir->frontend == AIMEE_WIRE_ANTHROPIC && !raw_is_function)
         {
            cJSON_Delete(out);
            return NULL;
         }
         if (raw && !raw_is_function)
         {
            cJSON *verbatim = cJSON_Duplicate((cJSON *)raw, 1);
            if (verbatim)
               cJSON_AddItemToArray(tools, verbatim);
            continue;
         }
         if (!ir->tools[i].name || !ir->tools[i].name[0])
            continue; /* never emit name:"" -- the provider rejects the whole request */
         cJSON *t = cJSON_CreateObject();
         cJSON_AddStringToObject(t, "type", "function");
         cJSON *fn = cJSON_AddObjectToObject(t, "function");
         cJSON_AddStringToObject(fn, "name", ir->tools[i].name);
         if (ir->tools[i].description)
            cJSON_AddStringToObject(fn, "description", ir->tools[i].description);
         cJSON_AddItemToObject(fn, "parameters",
                               ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                   : cJSON_CreateObject());
         cJSON_AddItemToArray(tools, t);
      }
   }
   openai_family_add_tool_choice(out, ir, 0);
   if (ir->n_stop == 1)
      cJSON_AddStringToObject(out, "stop", ir->stop_sequences[0] ? ir->stop_sequences[0] : "");
   else if (ir->n_stop > 1)
   {
      cJSON *stop = cJSON_AddArrayToObject(out, "stop");
      for (int i = 0; i < ir->n_stop; i++)
         cJSON_AddItemToArray(
             stop, cJSON_CreateString(ir->stop_sequences[i] ? ir->stop_sequences[i] : ""));
   }
   return out;
}

static aimee_stop_reason_t finish_to_stop(const char *f)
{
   if (!f)
      return AIMEE_STOP_UNKNOWN;
   if (strcmp(f, "stop") == 0)
      return AIMEE_STOP_END_TURN;
   if (strcmp(f, "tool_calls") == 0)
      return AIMEE_STOP_TOOL_USE;
   if (strcmp(f, "length") == 0)
      return AIMEE_STOP_MAX_TOKENS;
   if (strcmp(f, "content_filter") == 0)
      return AIMEE_STOP_CONTENT_FILTER;
   return AIMEE_STOP_UNKNOWN;
}

/* Append `len` bytes of `src` to the malloc'd string *dst. Returns 0 on success. */
static int sappend(char **dst, const char *src, size_t len)
{
   if (!src || len == 0)
      return 0;
   size_t old = *dst ? strlen(*dst) : 0;
   char *g = realloc(*dst, old + len + 1);
   if (!g)
      return -1;
   memcpy(g + old, src, len);
   g[old + len] = '\0';
   *dst = g;
   return 0;
}

/* Grow out->content by one block; return the new zeroed block or NULL on OOM. */
static aimee_block_t *ob_grow(aimee_response_t *out)
{
   if (out->n_content >= AIMEE_IR_MAX_BLOCKS)
      return NULL;
   void *p = realloc(out->content, (size_t)(out->n_content + 1) * sizeof(aimee_block_t));
   if (!p)
      return NULL;
   out->content = p;
   aimee_block_t *b = &out->content[out->n_content++];
   memset(b, 0, sizeof(*b));
   return b;
}

/* Split an OpenAI message `content` value into answer text and reasoning text.
 * A plain string is all answer. In a content-parts array, text / output_text parts
 * are answer; thinking / reasoning parts (their nested text) are reasoning. Both
 * outputs are accumulated (caller frees). */
static void openai_content_split(const cJSON *content, char **answer, char **reasoning)
{
   if (!content)
      return;
   if (cJSON_IsString(content))
   {
      sappend(answer, content->valuestring, strlen(content->valuestring));
      return;
   }
   if (!cJSON_IsArray(content))
      return;
   const cJSON *part = NULL;
   cJSON_ArrayForEach(part, content)
   {
      if (cJSON_IsString(part))
      {
         sappend(answer, part->valuestring, strlen(part->valuestring));
         continue;
      }
      const char *t = ostr(part, "type");
      if (!t)
         continue;
      if (strcmp(t, "text") == 0 || strcmp(t, "output_text") == 0)
      {
         const char *tx = ostr(part, "text");
         if (tx)
            sappend(answer, tx, strlen(tx));
      }
      else if (strcmp(t, "thinking") == 0 || strcmp(t, "reasoning") == 0)
      {
         const cJSON *th = cJSON_GetObjectItemCaseSensitive((cJSON *)part, "thinking");
         if (!th)
            th = cJSON_GetObjectItemCaseSensitive((cJSON *)part, "reasoning");
         /* nested value's text goes to reasoning */
         openai_content_split(th, reasoning, reasoning);
      }
   }
}

static int openai_response_text(aimee_response_t *out, const char *text, int group)
{
   if (!text)
      return -1;
   const char *reasoning = NULL;
   size_t length = 0;
   const char *answer = text_split_reasoning_prefix(text, &reasoning, &length);
   if (length)
   {
      aimee_block_t *block = ob_grow(out);
      if (!block)
         return -1;
      block->type = AIMEE_BLK_THINKING;
      block->text = strndup(reasoning, length);
      if (!block->text)
         return -1;
   }
   char *cleaned = strip_llm_private_scaffold(answer);
   if (!cleaned)
      return -1;
   if (strcmp(cleaned, answer))
   {
      /* Keep the original private-scaffold spelling in the canonical record.
       * Only the visible text projection uses the cleaned completion. */
      aimee_block_t *block = ob_grow(out);
      if (!block)
      {
         free(cleaned);
         return -1;
      }
      block->type = AIMEE_BLK_THINKING;
      block->text = strdup(answer);
      if (!block->text)
      {
         free(cleaned);
         return -1;
      }
   }
   if (!cleaned[0])
   {
      free(cleaned);
      return 0;
   }
   aimee_block_t *block = ob_grow(out);
   if (!block)
   {
      free(cleaned);
      return -1;
   }
   block->type = AIMEE_BLK_TEXT;
   block->text = cleaned;
   block->message_group = group;
   return 0;
}
static int openai_response_content(aimee_response_t *out, const cJSON *content)
{
   if (!content || cJSON_IsNull(content))
      return 0;
   if (cJSON_IsString(content))
      return openai_response_text(out, content->valuestring, 0);
   if (!cJSON_IsArray(content))
      return -1;
   const cJSON *part;
   cJSON_ArrayForEach(part, content)
   {
      const char *type = ostr(part, "type");
      if (cJSON_IsString(part) || (type && (!strcmp(type, "text") || !strcmp(type, "output_text"))))
      {
         if (openai_response_text(out,
                                  cJSON_IsString(part) ? part->valuestring : ostr(part, "text"), 1))
            return -1;
      }
      else if (type && (!strcmp(type, "thinking") || !strcmp(type, "reasoning")))
      {
         const cJSON *thought = cJSON_GetObjectItemCaseSensitive(
             part, !strcmp(type, "thinking") ? "thinking" : "reasoning");
         char *text = NULL;
         openai_content_split(thought, &text, &text);
         aimee_block_t *block = ob_grow(out);
         if (!block)
         {
            free(text);
            return -1;
         }
         block->type = AIMEE_BLK_THINKING;
         block->text = text;
         block->thinking_signature = dupstr(ostr(part, "signature"));
         if (!text)
            return -1;
      }
      else
      {
         /* Borrow one native part without borrowing the following siblings. */
         cJSON item = *part;
         item.next = item.prev = NULL;
         cJSON array = {.type = cJSON_Array, .child = &item};
         if (out->n_content >= AIMEE_IR_MAX_BLOCKS ||
             openai_ir_parse_content(&array, &out->content, &out->n_content))
            return -1;
      }
   }
   return 0;
}

int openai_backend_parse(const cJSON *resp, aimee_response_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!resp || !cJSON_IsObject(resp) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "openai_backend_parse: null/non-object response");
      return -1;
   }
   out->raw = cJSON_Duplicate(resp, 1);
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

   const cJSON *choices = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "choices");
   const cJSON *choice =
       (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem((cJSON *)choices, 0) : NULL;
   const cJSON *msg = choice ? cJSON_GetObjectItemCaseSensitive((cJSON *)choice, "message") : NULL;
   const char *fr = choice ? ostr(choice, "finish_reason") : NULL;
   out->raw_stop_reason = dupstr(fr);
   out->stop_reason = finish_to_stop(fr);

   if (msg)
   {
      out->role = dupstr(ostr(msg, "role"));

      const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(msg, "reasoning_content");
      if (!reasoning)
         reasoning = cJSON_GetObjectItemCaseSensitive(msg, "reasoning");
      if (reasoning && !cJSON_IsNull(reasoning))
      {
         char *text = NULL;
         openai_content_split(reasoning, &text, &text);
         aimee_block_t *block = ob_grow(out);
         if (!block)
         {
            free(text);
            aimee_response_free(out);
            return -1;
         }
         block->type = AIMEE_BLK_THINKING;
         block->text = text;
         if (!text)
         {
            aimee_response_free(out);
            return -1;
         }
      }
      if (openai_response_content(out, cJSON_GetObjectItemCaseSensitive(msg, "content")))
      {
         aimee_response_free(out);
         return -1;
      }
      const cJSON *refusal = cJSON_GetObjectItemCaseSensitive(msg, "refusal");
      if (refusal && !cJSON_IsNull(refusal) &&
          (!cJSON_IsString(refusal) || openai_response_text(out, refusal->valuestring, 0)))
      {
         aimee_response_free(out);
         return -1;
      }
      const cJSON *calls = cJSON_GetObjectItemCaseSensitive((cJSON *)msg, "tool_calls");
      const cJSON *c = NULL;
      if (calls && cJSON_IsArray(calls))
         cJSON_ArrayForEach(c, calls)
         {
            aimee_block_t *b = ob_grow(out);
            if (!b)
            {
               aimee_response_free(out);
               return -1;
            }
            b->type = AIMEE_BLK_TOOL_USE;
            b->raw = cJSON_Duplicate(c, 1);
            b->tool_id = dupstr(ostr(c, "id"));
            b->tool_namespace = dupstr(ostr(c, "namespace"));
            const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "function");
            b->tool_name = dupstr(fn ? ostr(fn, "name") : NULL);
            const char *args = fn ? ostr(fn, "arguments") : NULL;
            const char *end = NULL;
            b->tool_input = args ? cJSON_ParseWithOpts(args, &end, 1) : NULL;
            if (!cJSON_IsObject(b->tool_input))
            {
               aimee_response_free(out);
               return -1;
            }
         }
   }

   const cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
   const cJSON *cached = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
   const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens_details");
   if (aimee_ir_read_usage(usage, "prompt_tokens", &out->usage_in) ||
       aimee_ir_read_usage(usage, "completion_tokens", &out->usage_out) ||
       aimee_ir_read_usage(cached, "cached_tokens", &out->usage_cache_read) ||
       aimee_ir_read_usage(reasoning, "reasoning_tokens", &out->usage_reasoning))
   {
      if (err && errn)
         snprintf(err, errn, "invalid provider usage counter");
      aimee_response_free(out);
      return -1;
   }

   return 0;
}
