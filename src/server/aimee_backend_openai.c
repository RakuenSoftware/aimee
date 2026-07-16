/* aimee_backend_openai.c -- IR <-> OpenAI Chat Completions (upstream provider).
 * See aimee_backend.h.
 *
 * NOTE (Slice 2): TOOL_RESULT blocks -> OpenAI role:"tool" messages (the split) are
 * applied per the tool_result-grouping ruling in the follow-up; this covers
 * system-lowering, text/tool_use, tool definitions, params, and response parse. */
#include "aimee_backend.h"

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

cJSON *openai_backend_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (ir->model)
      cJSON_AddStringToObject(out, "model", ir->model);
   if (ir->has_max_tokens)
      cJSON_AddNumberToObject(out, "max_tokens", ir->max_tokens);
   if (ir->has_temperature)
      cJSON_AddNumberToObject(out, "temperature", ir->temperature);
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
         cJSON *fn = cJSON_AddObjectToObject(call, "function");
         cJSON_AddStringToObject(fn, "name", b->tool_name ? b->tool_name : "");
         char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
         cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
         free(args);
         cJSON_AddItemToArray(tool_calls, call);
      }
      if (text && text[0])
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
         cJSON *t = cJSON_CreateObject();
         cJSON_AddStringToObject(t, "type", "function");
         cJSON *fn = cJSON_AddObjectToObject(t, "function");
         cJSON_AddStringToObject(fn, "name", ir->tools[i].name ? ir->tools[i].name : "");
         if (ir->tools[i].description)
            cJSON_AddStringToObject(fn, "description", ir->tools[i].description);
         cJSON_AddItemToObject(fn, "parameters",
                               ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                   : cJSON_CreateObject());
         cJSON_AddItemToArray(tools, t);
      }
   }
   if (ir->tool_choice)
      cJSON_AddItemToObject(out, "tool_choice", cJSON_Duplicate(ir->tool_choice, 1));
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

      /* Text response: split answer vs reasoning. Reasoning models on this wire embed
       * their chain-of-thought in the content (content-parts thinking items, an inline
       * <think>...</think> prefix, or a separate reasoning_content field). We STORE it
       * as a THINKING block (excluded from the content accessor) rather than discard
       * it, and keep the answer as a TEXT block. */
      char *answer = NULL, *reasoning = NULL;
      openai_content_split(cJSON_GetObjectItemCaseSensitive((cJSON *)msg, "content"), &answer,
                           &reasoning);

      const cJSON *rc = cJSON_GetObjectItemCaseSensitive((cJSON *)msg, "reasoning_content");
      if (rc)
      {
         /* qwen3 etc.: when content is empty the actual output is in reasoning_content;
          * otherwise it is separate reasoning. */
         if (!answer || !answer[0])
         {
            free(answer);
            answer = NULL;
            openai_content_split(rc, &answer, &answer);
         }
         else
            openai_content_split(rc, &reasoning, &reasoning);
      }

      if (answer)
      {
         const char *rp = NULL;
         size_t rlen = 0;
         const char *ans = text_split_reasoning_prefix(answer, &rp, &rlen);
         if (rlen > 0)
            sappend(&reasoning, rp, rlen); /* store the <think> prefix */
         /* Drop leaked private scaffold from the answer (malformed prose, not a clean
          * reasoning block; the helper returns only the cleaned text). */
         char *cleaned = strip_llm_private_scaffold(ans);
         free(answer);
         answer = cleaned;
      }

      if (reasoning && reasoning[0])
      {
         aimee_block_t *b = ob_grow(out);
         if (!b)
         {
            free(answer);
            free(reasoning);
            aimee_response_free(out);
            return -1;
         }
         b->type = AIMEE_BLK_THINKING;
         b->text = reasoning;
         reasoning = NULL;
      }
      free(reasoning);

      if (answer && answer[0])
      {
         aimee_block_t *b = ob_grow(out);
         if (!b)
         {
            free(answer);
            aimee_response_free(out);
            return -1;
         }
         b->type = AIMEE_BLK_TEXT;
         b->text = answer;
         answer = NULL;
      }
      free(answer);

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
            const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "function");
            b->tool_name = dupstr(fn ? ostr(fn, "name") : NULL);
            const char *args = fn ? ostr(fn, "arguments") : NULL;
            b->tool_input = args ? cJSON_Parse(args) : NULL;
         }
   }

   const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "usage");
   if (usage && cJSON_IsObject(usage))
   {
      const cJSON *pt = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "prompt_tokens");
      const cJSON *ct = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "completion_tokens");
      if (pt && cJSON_IsNumber(pt))
         out->usage_in = (long)pt->valuedouble;
      if (ct && cJSON_IsNumber(ct))
         out->usage_out = (long)ct->valuedouble;
   }
   return 0;
}
