/* aimee_frontend_anthropic.c -- Anthropic Messages API (/v1/messages) <-> IR.
 * Parse only in this slice; render lands with the response path. See aimee_frontend.h. */
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

/* Anthropic cache_control is {"type":"ephemeral"}; keep the type string as the
 * opaque marker (full fidelity lives in the block's raw sidecar). */
static char *parse_cache_control(const cJSON *block)
{
   const cJSON *cc = cJSON_GetObjectItemCaseSensitive((cJSON *)block, "cache_control");
   if (!cc || !cJSON_IsObject(cc))
      return NULL;
   const char *t = ostr(cc, "type");
   return dupstr(t ? t : "");
}

/* Parse one Anthropic content block into `b`. */
static void parse_block(const cJSON *el, aimee_block_t *b)
{
   memset(b, 0, sizeof *b);
   b->raw = cJSON_Duplicate(el, 1);
   b->cache_control = parse_cache_control(el);
   const char *type = ostr(el, "type");
   if (!type)
   {
      b->type = AIMEE_BLK_UNKNOWN;
      return;
   }
   if (strcmp(type, "text") == 0)
   {
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(ostr(el, "text"));
   }
   else if (strcmp(type, "thinking") == 0)
   {
      b->type = AIMEE_BLK_THINKING;
      b->text = dupstr(ostr(el, "thinking"));
   }
   else if (strcmp(type, "tool_use") == 0)
   {
      b->type = AIMEE_BLK_TOOL_USE;
      b->tool_id = dupstr(ostr(el, "id"));
      b->tool_name = dupstr(ostr(el, "name"));
      const cJSON *input = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "input");
      b->tool_input = input ? cJSON_Duplicate(input, 1) : NULL; /* OPAQUE: verbatim */
   }
   else if (strcmp(type, "tool_result") == 0)
   {
      b->type = AIMEE_BLK_TOOL_RESULT;
      b->tool_id = dupstr(ostr(el, "tool_use_id"));
      const cJSON *iserr = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "is_error");
      b->tool_is_error = (iserr && cJSON_IsTrue(iserr)) ? 1 : 0;
      const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "content");
      b->tool_result = content ? cJSON_Duplicate(content, 1) : NULL;
   }
   else if (strcmp(type, "image") == 0 || strcmp(type, "document") == 0)
   {
      b->type = (strcmp(type, "image") == 0) ? AIMEE_BLK_IMAGE : AIMEE_BLK_DOCUMENT;
      const cJSON *src = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "source");
      if (src && cJSON_IsObject(src))
      {
         b->media_type = dupstr(ostr(src, "media_type"));
         /* base64 payload OR a url, whichever the source carries */
         const char *data = ostr(src, "data");
         b->media_ref = dupstr(data ? data : ostr(src, "url"));
      }
   }
   else
   {
      b->type = AIMEE_BLK_UNKNOWN;
   }
}

/* Parse a `content` value (string shorthand OR block array) into a block array. */
static int parse_content(const cJSON *content, aimee_block_t **out, int *n)
{
   *out = NULL;
   *n = 0;
   if (!content)
      return 0;
   if (cJSON_IsString(content))
   {
      aimee_block_t *b = calloc(1, sizeof(aimee_block_t));
      if (!b)
         return -1;
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(content->valuestring);
      *out = b;
      *n = 1;
      return 0;
   }
   if (!cJSON_IsArray(content))
      return 0;
   int cnt = cJSON_GetArraySize((cJSON *)content);
   if (cnt <= 0)
      return 0;
   aimee_block_t *arr = calloc((size_t)cnt, sizeof(aimee_block_t));
   if (!arr)
      return -1;
   int i = 0;
   const cJSON *el = NULL;
   cJSON_ArrayForEach(el, content)
   {
      parse_block(el, &arr[i++]);
   }
   *out = arr;
   *n = cnt;
   return 0;
}

int anthropic_frontend_parse(const cJSON *req, aimee_request_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!req || !cJSON_IsObject(req) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "anthropic_frontend_parse: null/non-object request");
      return -1;
   }
   out->frontend = AIMEE_WIRE_ANTHROPIC;
   out->raw = cJSON_Duplicate(req, 1); /* whole-request sidecar for same-protocol replay */
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
   const cJSON *stier = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "service_tier");
   if (stier && cJSON_IsString(stier) && stier->valuestring)
      out->service_tier = strdup(stier->valuestring);
   const cJSON *think = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "thinking");
   if (think && !cJSON_IsNull(think))
      out->thinking = cJSON_Duplicate(think, 1);
   const cJSON *stream = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stream");
   out->stream = (stream && cJSON_IsTrue(stream)) ? 1 : 0;

   /* system: string -> one text block; array -> blocks */
   const cJSON *sys = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "system");
   if (parse_content(sys, &out->system, &out->n_system) != 0)
      goto oom;

   /* messages */
   const cJSON *msgs = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "messages");
   if (msgs && cJSON_IsArray(msgs))
   {
      int mc = cJSON_GetArraySize((cJSON *)msgs);
      if (mc > 0)
      {
         out->messages = calloc((size_t)mc, sizeof(aimee_message_t));
         if (!out->messages)
            goto oom;
         out->n_messages = mc;
         int i = 0;
         const cJSON *m = NULL;
         cJSON_ArrayForEach(m, msgs)
         {
            out->messages[i].role = dupstr(ostr(m, "role"));
            out->messages[i].raw = cJSON_Duplicate(m, 1);
            const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "content");
            if (parse_content(content, &out->messages[i].blocks, &out->messages[i].n_blocks) != 0)
               goto oom;
            i++;
         }
      }
   }

   /* tools */
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tools");
   if (tools && cJSON_IsArray(tools))
   {
      int tc = cJSON_GetArraySize((cJSON *)tools);
      if (tc > 0)
      {
         out->tools = calloc((size_t)tc, sizeof(aimee_tool_t));
         if (!out->tools)
            goto oom;
         out->n_tools = tc;
         int i = 0;
         const cJSON *t = NULL;
         cJSON_ArrayForEach(t, tools)
         {
            out->tools[i].name = dupstr(ostr(t, "name"));
            out->tools[i].description = dupstr(ostr(t, "description"));
            out->tools[i].cache_control = parse_cache_control(t);
            const cJSON *schema = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "input_schema");
            out->tools[i].schema = schema ? cJSON_Duplicate(schema, 1) : NULL;
            i++;
         }
      }
   }

   const cJSON *tc = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tool_choice");
   out->tool_choice = tc ? cJSON_Duplicate(tc, 1) : NULL;

   /* stop_sequences */
   const cJSON *stop = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stop_sequences");
   if (stop && cJSON_IsArray(stop))
   {
      int sc = cJSON_GetArraySize((cJSON *)stop);
      if (sc > 0)
      {
         out->stop_sequences = calloc((size_t)sc, sizeof(char *));
         if (!out->stop_sequences)
            goto oom;
         int i = 0;
         const cJSON *s = NULL;
         cJSON_ArrayForEach(s, stop)
         {
            if (cJSON_IsString(s))
               out->stop_sequences[out->n_stop++] = dupstr(s->valuestring);
            (void)i;
         }
      }
   }
   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "anthropic_frontend_parse: out of memory");
   aimee_request_free(out);
   return -1;
}

/* canonical stop_reason -> Anthropic vocabulary */
static const char *anthropic_stop(const aimee_response_t *r)
{
   switch (r->stop_reason)
   {
   case AIMEE_STOP_END_TURN:
      return "end_turn";
   case AIMEE_STOP_TOOL_USE:
      return "tool_use";
   case AIMEE_STOP_MAX_TOKENS:
      return "max_tokens";
   case AIMEE_STOP_STOP_SEQUENCE:
      return "stop_sequence";
   default:
      /* preserve a provider-specific value if the canonical enum was UNKNOWN */
      return r->raw_stop_reason ? r->raw_stop_reason : "end_turn";
   }
}

cJSON *anthropic_frontend_render(const aimee_response_t *r)
{
   if (!r)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "id", r->id ? r->id : "");
   cJSON_AddStringToObject(out, "type", "message");
   cJSON_AddStringToObject(out, "role", r->role ? r->role : "assistant");
   if (r->model)
      cJSON_AddStringToObject(out, "model", r->model);
   cJSON *content = cJSON_AddArrayToObject(out, "content");
   for (int i = 0; i < r->n_content; i++)
   {
      const aimee_block_t *b = &r->content[i];
      cJSON *el = cJSON_CreateObject();
      if (b->type == AIMEE_BLK_TEXT)
      {
         cJSON_AddStringToObject(el, "type", "text");
         cJSON_AddStringToObject(el, "text", b->text ? b->text : "");
      }
      else if (b->type == AIMEE_BLK_THINKING)
      {
         cJSON_AddStringToObject(el, "type", "thinking");
         cJSON_AddStringToObject(el, "thinking", b->text ? b->text : "");
      }
      else if (b->type == AIMEE_BLK_TOOL_USE)
      {
         cJSON_AddStringToObject(el, "type", "tool_use");
         cJSON_AddStringToObject(el, "id", b->tool_id ? b->tool_id : "");
         cJSON_AddStringToObject(el, "name", b->tool_name ? b->tool_name : "");
         cJSON_AddItemToObject(
             el, "input", b->tool_input ? cJSON_Duplicate(b->tool_input, 1) : cJSON_CreateObject());
      }
      else
      {
         cJSON_Delete(el); /* drop non-renderable blocks (e.g. UNKNOWN) */
         continue;
      }
      cJSON_AddItemToArray(content, el);
   }
   cJSON_AddStringToObject(out, "stop_reason", anthropic_stop(r));
   cJSON_AddNullToObject(out, "stop_sequence");
   cJSON *usage = cJSON_AddObjectToObject(out, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", (double)r->usage_in);
   cJSON_AddNumberToObject(usage, "output_tokens", (double)r->usage_out);
   return out;
}
