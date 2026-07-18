/* aimee_frontend_openai.c -- OpenAI Chat Completions (/v1/chat/completions) <-> IR.
 * Parse only in this slice. See aimee_frontend.h.
 *
 * NOTE (Slice 1): in-message tool_calls (assistant) + role:"tool" results are
 * handled per the Q1 roundtable ruling on tool-argument representation and are
 * added in the follow-up; this slice covers system-lifting + text/image content +
 * tool DEFINITIONS + sampling params, which is what the basic golden pair needs. */
#include "aimee_frontend.h"

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

/* grow `*arr` (of element size `esz`) by one, return the new (zeroed) slot or NULL. */
static void *grow1(void **arr, int *n, size_t esz)
{
   void *p = realloc(*arr, (size_t)(*n + 1) * esz);
   if (!p)
      return NULL;
   *arr = p;
   void *slot = (char *)p + (size_t)(*n) * esz;
   memset(slot, 0, esz);
   (*n)++;
   return slot;
}

/* Parse an OpenAI content value (string OR array of parts) and APPEND blocks to
 * (*blocks,*n). type:"text" -> TEXT; type:"image_url" -> IMAGE (media_ref=url). */
static int append_openai_content(const cJSON *content, aimee_block_t **blocks, int *n)
{
   if (!content)
      return 0;
   if (cJSON_IsString(content))
   {
      aimee_block_t *b = grow1((void **)blocks, n, sizeof(aimee_block_t));
      if (!b)
         return -1;
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(content->valuestring);
      return 0;
   }
   if (!cJSON_IsArray(content))
      return 0;
   const cJSON *el = NULL;
   cJSON_ArrayForEach(el, content)
   {
      aimee_block_t *b = grow1((void **)blocks, n, sizeof(aimee_block_t));
      if (!b)
         return -1;
      b->raw = cJSON_Duplicate(el, 1);
      const char *type = ostr(el, "type");
      if (type && strcmp(type, "image_url") == 0)
      {
         b->type = AIMEE_BLK_IMAGE;
         const cJSON *iu = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "image_url");
         const char *url = iu ? ostr(iu, "url") : NULL;
/* Canonicalize an inline base64 data: URL into the structured media_type + base64
 * data form the Anthropic egress emits, so an OpenAI-sourced inline image serializes
 * identically to an Anthropic-sourced one. The RFC-2397 shape is
 * "data:<media_type>;base64,<data>": the FIRST comma delimits the header from the
 * data, and the header must end with ";base64". Anything else (plain http(s) URL, a
 * non-base64 data URL, or a ";base64," that appears only inside the payload after the
 * comma) is kept as a url reference verbatim -- Anthropic has no data:-url image form,
 * so there is no cross-protocol counterpart to canonicalize toward. */
#define DATA_URL_PREFIX_LEN 5 /* strlen("data:") */
#define B64_SUFFIX          ";base64"
#define B64_SUFFIX_LEN      7 /* strlen(";base64") */
         const char *comma = url ? strchr(url, ',') : NULL;
         int decomposed = 0;
         if (url && strncmp(url, "data:", DATA_URL_PREFIX_LEN) == 0 && comma)
         {
            const char *hdr = url + DATA_URL_PREFIX_LEN;
            size_t hdr_len = (size_t)(comma - hdr);
            if (hdr_len > B64_SUFFIX_LEN &&
                strncmp(comma - B64_SUFFIX_LEN, B64_SUFFIX, B64_SUFFIX_LEN) == 0)
            {
               size_t mt_len = hdr_len - B64_SUFFIX_LEN; /* media_type before ";base64" */
               b->media_type = strndup(hdr, mt_len);
               b->media_ref = strdup(comma + 1);
               decomposed = 1;
            }
         }
         if (!decomposed)
            b->media_ref = dupstr(url);
#undef DATA_URL_PREFIX_LEN
#undef B64_SUFFIX
#undef B64_SUFFIX_LEN
      }
      else /* text (default) */
      {
         b->type = AIMEE_BLK_TEXT;
         b->text = dupstr(ostr(el, "text"));
      }
   }
   return 0;
}

int openai_frontend_parse(const cJSON *req, aimee_request_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!req || !cJSON_IsObject(req) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "openai_frontend_parse: null/non-object request");
      return -1;
   }
   out->frontend = AIMEE_WIRE_OPENAI_CHAT;
   out->raw = cJSON_Duplicate(req, 1);
   out->model = dupstr(ostr(req, "model"));

   const cJSON *mt = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "max_tokens");
   if (mt && cJSON_IsNumber(mt))
   {
      out->max_tokens = mt->valueint;
      out->has_max_tokens = 1;
   }
   const cJSON *temp = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "temperature");
   if (temp && cJSON_IsNumber(temp))
   {
      out->temperature = temp->valuedouble;
      out->has_temperature = 1;
   }
   const cJSON *top_p = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "top_p");
   if (top_p && cJSON_IsNumber(top_p))
   {
      out->top_p = top_p->valuedouble;
      out->has_top_p = 1;
   }
   const cJSON *top_k = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "top_k");
   if (top_k && cJSON_IsNumber(top_k))
   {
      out->top_k = top_k->valueint;
      out->has_top_k = 1;
   }
   const cJSON *meta = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "metadata");
   if (meta && !cJSON_IsNull(meta))
      out->metadata = cJSON_Duplicate(meta, 1);
   const cJSON *stream = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stream");
   out->stream = (stream && cJSON_IsTrue(stream)) ? 1 : 0;

   /* messages. LIFT only the LEADING CONSECUTIVE run of role:"system" into
    * out->system (to converge with Anthropic's separate system). A system message
    * AFTER the first non-system turn is NOT lifted -- lifting mid-conversation
    * system would elevate later user-controlled content to system trust (a
    * privilege-escalation / injection vector, ruling Q2) -- it is kept as a normal
    * message with role preserved. Empty/whitespace-only system is dropped. */
   const cJSON *msgs = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "messages");
   if (msgs && cJSON_IsArray(msgs))
   {
      int leading = 1;
      int tool_run = -1; /* index into out->messages of the pending tool-result user msg */
      const cJSON *m = NULL;
      cJSON_ArrayForEach(m, msgs)
      {
         const char *role = ostr(m, "role");
         const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "content");
         if (leading && role && strcmp(role, "system") == 0)
         {
            const char *s = cJSON_IsString(content) ? content->valuestring : NULL;
            int empty = s && s[strspn(s, " \t\r\n")] == '\0'; /* whitespace-only */
            if (!empty && append_openai_content(content, &out->system, &out->n_system) != 0)
               goto oom;
            continue;
         }
         leading = 0; /* the leading system run has ended */
         /* MERGE (grouping ruling, Option A): a maximal run of consecutive
          * role:"tool" messages becomes tool_result BLOCKS in ONE canonical user
          * message. call_id -> tool_id verbatim; content string -> tool_result. */
         if (role && strcmp(role, "tool") == 0)
         {
            if (tool_run < 0)
            {
               aimee_message_t *tr =
                   grow1((void **)&out->messages, &out->n_messages, sizeof(aimee_message_t));
               if (!tr)
                  goto oom;
               tr->role = dupstr("user");
               tool_run = out->n_messages - 1;
            }
            aimee_message_t *tr = &out->messages[tool_run];
            aimee_block_t *b = grow1((void **)&tr->blocks, &tr->n_blocks, sizeof(aimee_block_t));
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_TOOL_RESULT;
            b->raw = cJSON_Duplicate(m, 1);
            b->tool_id = dupstr(ostr(m, "tool_call_id"));
            const char *tc = cJSON_IsString(content) ? content->valuestring : NULL;
            b->tool_result =
                tc ? cJSON_CreateString(tc) : (content ? cJSON_Duplicate(content, 1) : NULL);
            continue;
         }
         tool_run = -1; /* any non-tool message ends the run */
         aimee_message_t *msg =
             grow1((void **)&out->messages, &out->n_messages, sizeof(aimee_message_t));
         if (!msg)
            goto oom;
         msg->role = dupstr(role);
         msg->raw = cJSON_Duplicate(m, 1);
         if (append_openai_content(content, &msg->blocks, &msg->n_blocks) != 0)
            goto oom;
         /* assistant tool_calls -> TOOL_USE blocks. tool_input is the PARSED object
          * (canonical, cross-protocol comparable); the OpenAI `arguments` STRING is
          * preserved in the block raw sidecar for same-protocol replay (ruling Q1
          * option C: opaque bytes scoped to same-protocol). tool_id stays a separate
          * typed field, carried verbatim (never derived from the args). */
         const cJSON *calls = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "tool_calls");
         if (calls && cJSON_IsArray(calls))
         {
            const cJSON *call = NULL;
            cJSON_ArrayForEach(call, calls)
            {
               aimee_block_t *b =
                   grow1((void **)&msg->blocks, &msg->n_blocks, sizeof(aimee_block_t));
               if (!b)
                  goto oom;
               b->type = AIMEE_BLK_TOOL_USE;
               b->raw = cJSON_Duplicate(call, 1);
               b->tool_id = dupstr(ostr(call, "id"));
               const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)call, "function");
               b->tool_name = dupstr(fn ? ostr(fn, "name") : NULL);
               const char *args = fn ? ostr(fn, "arguments") : NULL;
               b->tool_input = args ? cJSON_Parse(args) : NULL; /* derived-once parse */
            }
         }
      }
   }

   /* tools: {type:function, function:{name,description,parameters}} */
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tools");
   if (tools && cJSON_IsArray(tools))
   {
      const cJSON *t = NULL;
      cJSON_ArrayForEach(t, tools)
      {
         const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "function");
         const cJSON *src = fn ? fn : t; /* tolerate a flat tool too */
         aimee_tool_t *tool = grow1((void **)&out->tools, &out->n_tools, sizeof(aimee_tool_t));
         if (!tool)
            goto oom;
         tool->name = dupstr(ostr(src, "name"));
         tool->description = dupstr(ostr(src, "description"));
         const cJSON *params = cJSON_GetObjectItemCaseSensitive((cJSON *)src, "parameters");
         tool->schema = params ? cJSON_Duplicate(params, 1) : NULL;
      }
   }

   const cJSON *tc = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tool_choice");
   out->tool_choice = tc ? cJSON_Duplicate(tc, 1) : NULL;

   /* stop: string OR array */
   const cJSON *stop = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stop");
   if (stop && cJSON_IsString(stop))
   {
      out->stop_sequences = calloc(1, sizeof(char *));
      if (!out->stop_sequences)
         goto oom;
      out->stop_sequences[0] = dupstr(stop->valuestring);
      out->n_stop = 1;
   }
   else if (stop && cJSON_IsArray(stop))
   {
      const cJSON *s = NULL;
      cJSON_ArrayForEach(s, stop)
      {
         if (!cJSON_IsString(s))
            continue;
         char **slot = grow1((void **)&out->stop_sequences, &out->n_stop, sizeof(char *));
         if (!slot)
            goto oom;
         *slot = dupstr(s->valuestring);
      }
   }
   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "openai_frontend_parse: out of memory");
   aimee_request_free(out);
   return -1;
}

/* canonical stop_reason -> OpenAI finish_reason */
static const char *openai_finish(const aimee_response_t *r)
{
   switch (r->stop_reason)
   {
   case AIMEE_STOP_TOOL_USE:
      return "tool_calls";
   case AIMEE_STOP_MAX_TOKENS:
      return "length";
   case AIMEE_STOP_CONTENT_FILTER:
      return "content_filter";
   case AIMEE_STOP_END_TURN:
   case AIMEE_STOP_STOP_SEQUENCE:
      return "stop";
   default:
      return "stop";
   }
}

cJSON *openai_frontend_render(const aimee_response_t *r)
{
   if (!r)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "id", r->id ? r->id : "");
   cJSON_AddStringToObject(out, "object", "chat.completion");
   if (r->model)
      cJSON_AddStringToObject(out, "model", r->model);

   cJSON *choices = cJSON_AddArrayToObject(out, "choices");
   cJSON *choice = cJSON_CreateObject();
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddNumberToObject(choice, "index", 0);
   cJSON *msg = cJSON_AddObjectToObject(choice, "message");
   cJSON_AddStringToObject(msg, "role", r->role ? r->role : "assistant");

   /* content = concatenation of text blocks; tool_use blocks -> tool_calls[] */
   size_t tlen = 0;
   for (int i = 0; i < r->n_content; i++)
      if (r->content[i].type == AIMEE_BLK_TEXT && r->content[i].text)
         tlen += strlen(r->content[i].text);
   char *text = calloc(tlen + 1, 1);
   cJSON *tool_calls = NULL;
   if (text)
   {
      for (int i = 0; i < r->n_content; i++)
      {
         const aimee_block_t *b = &r->content[i];
         if (b->type == AIMEE_BLK_TEXT && b->text)
            strcat(text, b->text);
         else if (b->type == AIMEE_BLK_TOOL_USE)
         {
            if (!tool_calls)
               tool_calls = cJSON_CreateArray();
            cJSON *call = cJSON_CreateObject();
            cJSON_AddStringToObject(call, "id", b->tool_id ? b->tool_id : "");
            cJSON_AddStringToObject(call, "type", "function");
            cJSON *fn = cJSON_AddObjectToObject(call, "function");
            cJSON_AddStringToObject(fn, "name", b->tool_name ? b->tool_name : "");
            /* arguments STRING: serialize the parsed tool_input (cross-protocol) */
            char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
            cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
            free(args);
            cJSON_AddItemToArray(tool_calls, call);
         }
      }
   }
   /* content is null when the assistant only emitted tool calls (OpenAI convention) */
   if (text && text[0])
      cJSON_AddStringToObject(msg, "content", text);
   else
      cJSON_AddNullToObject(msg, "content");
   free(text);
   if (tool_calls)
      cJSON_AddItemToObject(msg, "tool_calls", tool_calls);

   cJSON_AddStringToObject(choice, "finish_reason", openai_finish(r));

   cJSON *usage = cJSON_AddObjectToObject(out, "usage");
   cJSON_AddNumberToObject(usage, "prompt_tokens", (double)r->usage_in);
   cJSON_AddNumberToObject(usage, "completion_tokens", (double)r->usage_out);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)(r->usage_in + r->usage_out));
   return out;
}
