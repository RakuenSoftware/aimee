/* aimee_backend_responses.c -- IR <-> OpenAI Responses API (codex). See
 * aimee_backend.h. This is the backend for the user's key case: Claude Code
 * (Anthropic frontend) served by codex (Responses backend), all via the IR.
 *
 * NOTE (Slice 2): TOOL_RESULT -> function_call_output items (the split) applied per
 * the tool_result-grouping ruling in the follow-up; this covers instructions,
 * message text, function_call (tool_use), tools, and response parse. */
#include <aimee/translation/aimee_backend.h>

#include "cJSON.h"

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

cJSON *responses_backend_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (ir->model)
      cJSON_AddStringToObject(out, "model", ir->model);
   /* NOTE: no max_output_tokens -- the legacy build omits it and codex/gpt-5.5
    * 400s on "Unsupported parameter: max_output_tokens" (verified live). */
   /* Responses/codex REQUIREMENTS (verified live: codex 400s with "Store must be
    * set to false" otherwise): store=false, and the API is SSE-based so stream is
    * always on (the caller accumulates + parses the SSE). */
   cJSON_AddBoolToObject(out, "store", 0);
   cJSON_AddBoolToObject(out, "stream", 1);
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
      /* text blocks -> a `message` item; each tool_use -> a `function_call` item */
      char *text = blocks_text(im->blocks, im->n_blocks);
      if (text && text[0])
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "message");
         cJSON_AddStringToObject(item, "role", im->role ? im->role : "user");
         cJSON *content = cJSON_AddArrayToObject(item, "content");
         cJSON *part = cJSON_CreateObject();
         cJSON_AddStringToObject(part, "type", is_assistant ? "output_text" : "input_text");
         cJSON_AddStringToObject(part, "text", text);
         cJSON_AddItemToArray(content, part);
         cJSON_AddItemToArray(input, item);
      }
      free(text);
      for (int j = 0; j < im->n_blocks; j++)
      {
         const aimee_block_t *b = &im->blocks[j];
         if (b->type == AIMEE_BLK_TOOL_USE)
         {
            cJSON *fc = cJSON_CreateObject();
            cJSON_AddStringToObject(fc, "type", "function_call");
            cJSON_AddStringToObject(fc, "call_id", b->tool_id ? b->tool_id : "");
            cJSON_AddStringToObject(fc, "name", b->tool_name ? b->tool_name : "");
            /* Only when set: a plain tool has no group, and an empty `namespace`
             * would claim one that does not exist. */
            if (b->tool_namespace && b->tool_namespace[0])
               cJSON_AddStringToObject(fc, "namespace", b->tool_namespace);
            char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
            cJSON_AddStringToObject(fc, "arguments", args ? args : "{}");
            free(args);
            cJSON_AddItemToArray(input, fc);
         }
         else if (b->type == AIMEE_BLK_TOOL_RESULT)
         {
            /* SPLIT (grouping ruling, Option A): tool_result block -> a
             * function_call_output item, call_id = tool_id verbatim; rich content
             * coerced to a string (documented lossy). */
            cJSON *fo = cJSON_CreateObject();
            cJSON_AddStringToObject(fo, "type", "function_call_output");
            cJSON_AddStringToObject(fo, "call_id", b->tool_id ? b->tool_id : "");
            char *o = NULL;
            if (b->tool_result && cJSON_IsString(b->tool_result))
               o = strdup(b->tool_result->valuestring);
            else if (b->tool_result)
               o = cJSON_PrintUnformatted(b->tool_result);
            cJSON_AddStringToObject(fo, "output", o ? o : "");
            free(o);
            cJSON_AddItemToArray(input, fo);
         }
      }
   }

   if (ir->n_tools > 0)
   {
      cJSON *tools = cJSON_AddArrayToObject(out, "tools");
      for (int i = 0; i < ir->n_tools; i++)
      {
         /* Re-emit anything the IR does not model as a plain named function exactly
          * as it arrived. A Codex client's `namespace` groups carry their nested tool
          * list under `tools`, and flattening one into a single function named
          * mcp__aimee lost all nineteen aimee tools and made the model call a name
          * Codex then rejected ("unsupported call: mcp__aimee"). `web_search` and
          * friends carry no name at all, and synthesising `"name": ""` for them made
          * the provider reject the entire request.
          *
          * Only a sidecar whose own type is `function` is safe to re-render from the
          * modelled fields; everything else goes back verbatim. */
         const cJSON *raw = ir->tools[i].raw;
         const cJSON *rtype = raw ? cJSON_GetObjectItemCaseSensitive((cJSON *)raw, "type") : NULL;
         int raw_is_function = cJSON_IsString(rtype) && strcmp(rtype->valuestring, "function") == 0;
         if (raw && (!raw_is_function || !ir->tools[i].name || !ir->tools[i].name[0]))
         {
            cJSON *verbatim = cJSON_Duplicate((cJSON *)raw, 1);
            if (verbatim)
               cJSON_AddItemToArray(tools, verbatim);
            continue;
         }
         if (!ir->tools[i].name || !ir->tools[i].name[0])
            continue; /* unnamed and unmodelled: never emit name:"" */
         cJSON *t = cJSON_CreateObject();
         cJSON_AddStringToObject(t, "type", "function");
         cJSON_AddStringToObject(t, "name", ir->tools[i].name);
         if (ir->tools[i].description)
            cJSON_AddStringToObject(t, "description", ir->tools[i].description);
         cJSON_AddItemToObject(t, "parameters",
                               ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                   : cJSON_CreateObject());
         cJSON_AddItemToArray(tools, t);
      }
   }
   if (ir->tool_choice)
      cJSON_AddItemToObject(out, "tool_choice", cJSON_Duplicate(ir->tool_choice, 1));
   return out;
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
   out->id = dupstr(ostr(resp, "id"));
   out->model = dupstr(ostr(resp, "model"));
   out->role = dupstr("assistant");
   const char *status = ostr(resp, "status");
   out->raw_stop_reason = dupstr(status);

   /* output items: message (output_text parts) / function_call / reasoning */
   const cJSON *output = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "output");
   int saw_tool = 0;
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
            b->tool_input = args ? cJSON_Parse(args) : NULL;
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
         }
         else if (type && strcmp(type, "message") == 0)
         {
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
                     b->type = AIMEE_BLK_TEXT;
                     b->text = dupstr(ostr(part, "text"));
                  }
               }
         }
      }
   }
   /* status "completed" -> tool_use if a function_call was emitted, else end_turn;
    * "incomplete" -> max_tokens (Responses' truncation status). */
   if (status && strcmp(status, "incomplete") == 0)
      out->stop_reason = AIMEE_STOP_MAX_TOKENS;
   else
      out->stop_reason = saw_tool ? AIMEE_STOP_TOOL_USE : AIMEE_STOP_END_TURN;

   const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "usage");
   if (usage && cJSON_IsObject(usage))
   {
      const cJSON *in = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "input_tokens");
      const cJSON *ou = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "output_tokens");
      if (in && cJSON_IsNumber(in))
         out->usage_in = (long)in->valuedouble;
      if (ou && cJSON_IsNumber(ou))
         out->usage_out = (long)ou->valuedouble;
      /* CACHED PROMPT TOKENS. Responses reports them in a sibling object,
       * usage.input_tokens_details.cached_tokens -- the same shape as Chat's
       * prompt_tokens_details, under a different name. Reading only the two flat
       * counters loses them, which is why the Anthropic arm was the only one ever
       * reporting cache numbers. See the note in aimee_backend_openai.c. */
      const cJSON *itd = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "input_tokens_details");
      if (itd && cJSON_IsObject(itd))
      {
         const cJSON *cr = cJSON_GetObjectItemCaseSensitive((cJSON *)itd, "cached_tokens");
         if (cr && cJSON_IsNumber(cr))
            out->usage_cache_read = (long)cr->valuedouble;
      }
   }
   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "responses_backend_parse: out of memory");
   aimee_response_free(out);
   return -1;
}
