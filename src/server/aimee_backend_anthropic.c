/* aimee_backend_anthropic.c -- IR <-> Anthropic Messages API (upstream provider).
 * The IR is Anthropic-shaped by design, so this is the most direct mapping; the
 * OpenAI/Responses backends convert the same IR to their wire. See aimee_backend.h. */
#include "aimee_backend.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
   return s ? strdup(s) : NULL;
}

static void add_cache_control(cJSON *el, const char *cc)
{
   if (!cc)
      return;
   cJSON *o = cJSON_AddObjectToObject(el, "cache_control");
   cJSON_AddStringToObject(o, "type", cc[0] ? cc : "ephemeral");
}

/* one IR block -> its Anthropic wire JSON (owned). NULL if not renderable. */
static cJSON *block_to_anthropic(const aimee_block_t *b)
{
   cJSON *el = cJSON_CreateObject();
   switch (b->type)
   {
   case AIMEE_BLK_TEXT:
      cJSON_AddStringToObject(el, "type", "text");
      cJSON_AddStringToObject(el, "text", b->text ? b->text : "");
      break;
   case AIMEE_BLK_THINKING:
      cJSON_AddStringToObject(el, "type", "thinking");
      cJSON_AddStringToObject(el, "thinking", b->text ? b->text : "");
      break;
   case AIMEE_BLK_TOOL_USE:
      cJSON_AddStringToObject(el, "type", "tool_use");
      cJSON_AddStringToObject(el, "id", b->tool_id ? b->tool_id : "");
      cJSON_AddStringToObject(el, "name", b->tool_name ? b->tool_name : "");
      cJSON_AddItemToObject(
          el, "input", b->tool_input ? cJSON_Duplicate(b->tool_input, 1) : cJSON_CreateObject());
      break;
   case AIMEE_BLK_TOOL_RESULT:
      cJSON_AddStringToObject(el, "type", "tool_result");
      cJSON_AddStringToObject(el, "tool_use_id", b->tool_id ? b->tool_id : "");
      if (b->tool_is_error)
         cJSON_AddBoolToObject(el, "is_error", 1);
      cJSON_AddItemToObject(el, "content",
                            b->tool_result ? cJSON_Duplicate(b->tool_result, 1)
                                           : cJSON_CreateString(""));
      break;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
   {
      cJSON_AddStringToObject(el, "type", b->type == AIMEE_BLK_IMAGE ? "image" : "document");
      cJSON *src = cJSON_AddObjectToObject(el, "source");
      /* a data: payload carries a media_type; a bare url does not */
      if (b->media_type)
      {
         cJSON_AddStringToObject(src, "type", "base64");
         cJSON_AddStringToObject(src, "media_type", b->media_type);
         cJSON_AddStringToObject(src, "data", b->media_ref ? b->media_ref : "");
      }
      else
      {
         cJSON_AddStringToObject(src, "type", "url");
         cJSON_AddStringToObject(src, "url", b->media_ref ? b->media_ref : "");
      }
      break;
   }
   case AIMEE_BLK_UNKNOWN:
   default:
      /* replay the original wire form if we have it, else drop */
      cJSON_Delete(el);
      return b->raw ? cJSON_Duplicate(b->raw, 1) : NULL;
   }
   add_cache_control(el, b->cache_control);
   return el;
}

static cJSON *blocks_to_anthropic(const aimee_block_t *blocks, int n)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *el = block_to_anthropic(&blocks[i]);
      if (el)
         cJSON_AddItemToArray(arr, el);
   }
   return arr;
}

cJSON *anthropic_backend_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   /* Same-protocol byte-faithful egress: an Anthropic request that no IR transform
    * touched is byte-identical to its raw sidecar, so ship those exact bytes. This
    * preserves Claude Code's prompt-cache prefix (which the typed re-serialization
    * below would perturb via key-order/formatting) and is the whole point of routing
    * the native path through the IR instead of a verbatim passthrough that skips it.
    * A transform that mutates the typed fields sets ir->mutated, forcing the rebuild. */
   if (ir->frontend == AIMEE_WIRE_ANTHROPIC && !ir->mutated && ir->raw)
      return cJSON_Duplicate(ir->raw, 1);

   cJSON *out = cJSON_CreateObject();
   if (ir->model)
      cJSON_AddStringToObject(out, "model", ir->model);
   if (ir->has_max_tokens)
      cJSON_AddNumberToObject(out, "max_tokens", ir->max_tokens);
   if (ir->has_temperature)
      cJSON_AddNumberToObject(out, "temperature", ir->temperature);
   if (ir->has_top_p)
      cJSON_AddNumberToObject(out, "top_p", ir->top_p);
   if (ir->has_top_k)
      cJSON_AddNumberToObject(out, "top_k", ir->top_k);
   if (ir->metadata)
      cJSON_AddItemToObject(out, "metadata", cJSON_Duplicate(ir->metadata, 1));
   if (ir->service_tier)
      cJSON_AddStringToObject(out, "service_tier", ir->service_tier);
   if (ir->thinking)
      cJSON_AddItemToObject(out, "thinking", cJSON_Duplicate(ir->thinking, 1));
   if (ir->stream)
      cJSON_AddBoolToObject(out, "stream", 1);
   if (ir->n_system > 0)
      cJSON_AddItemToObject(out, "system", blocks_to_anthropic(ir->system, ir->n_system));
   cJSON *msgs = cJSON_AddArrayToObject(out, "messages");
   for (int i = 0; i < ir->n_messages; i++)
   {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "role", ir->messages[i].role ? ir->messages[i].role : "user");
      cJSON_AddItemToObject(m, "content",
                            blocks_to_anthropic(ir->messages[i].blocks, ir->messages[i].n_blocks));
      cJSON_AddItemToArray(msgs, m);
   }
   if (ir->n_tools > 0)
   {
      cJSON *tools = cJSON_AddArrayToObject(out, "tools");
      for (int i = 0; i < ir->n_tools; i++)
      {
         cJSON *t = cJSON_CreateObject();
         cJSON_AddStringToObject(t, "name", ir->tools[i].name ? ir->tools[i].name : "");
         if (ir->tools[i].description)
            cJSON_AddStringToObject(t, "description", ir->tools[i].description);
         cJSON_AddItemToObject(t, "input_schema",
                               ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                   : cJSON_CreateObject());
         add_cache_control(t, ir->tools[i].cache_control);
         cJSON_AddItemToArray(tools, t);
      }
   }
   if (ir->tool_choice)
      cJSON_AddItemToObject(out, "tool_choice", cJSON_Duplicate(ir->tool_choice, 1));
   if (ir->n_stop > 0)
   {
      cJSON *stop = cJSON_AddArrayToObject(out, "stop_sequences");
      for (int i = 0; i < ir->n_stop; i++)
         cJSON_AddItemToArray(
             stop, cJSON_CreateString(ir->stop_sequences[i] ? ir->stop_sequences[i] : ""));
   }
   return out;
}

/* --- parse: Anthropic response -> IR --- */
static const char *ostr(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

int anthropic_backend_parse(const cJSON *resp, aimee_response_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!resp || !cJSON_IsObject(resp) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "anthropic_backend_parse: null/non-object response");
      return -1;
   }
   out->raw = cJSON_Duplicate(resp, 1);
   out->id = dupstr(ostr(resp, "id"));
   out->model = dupstr(ostr(resp, "model"));
   out->role = dupstr(ostr(resp, "role"));
   const char *sr = ostr(resp, "stop_reason");
   out->raw_stop_reason = dupstr(sr);
   out->stop_reason = aimee_stop_reason_parse(sr); /* end_turn/tool_use/max_tokens/stop_sequence */

   const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "content");
   if (content && cJSON_IsArray(content))
   {
      int n = cJSON_GetArraySize((cJSON *)content);
      if (n > 0)
      {
         out->content = calloc((size_t)n, sizeof(aimee_block_t));
         if (!out->content)
         {
            aimee_response_free(out);
            return -1;
         }
         int i = 0;
         const cJSON *el = NULL;
         cJSON_ArrayForEach(el, content)
         {
            aimee_block_t *b = &out->content[i++];
            b->raw = cJSON_Duplicate(el, 1);
            const char *type = ostr(el, "type");
            if (type && strcmp(type, "tool_use") == 0)
            {
               b->type = AIMEE_BLK_TOOL_USE;
               b->tool_id = dupstr(ostr(el, "id"));
               b->tool_name = dupstr(ostr(el, "name"));
               const cJSON *in = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "input");
               b->tool_input = in ? cJSON_Duplicate(in, 1) : NULL;
            }
            else if (type && strcmp(type, "thinking") == 0)
            {
               b->type = AIMEE_BLK_THINKING;
               b->text = dupstr(ostr(el, "thinking"));
            }
            else /* text (default) */
            {
               b->type = AIMEE_BLK_TEXT;
               b->text = dupstr(ostr(el, "text"));
            }
         }
         out->n_content = n;
      }
   }

   const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "usage");
   if (usage && cJSON_IsObject(usage))
   {
      const cJSON *in = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "input_tokens");
      const cJSON *ou = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "output_tokens");
      const cJSON *cr = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "cache_read_input_tokens");
      const cJSON *cw =
          cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "cache_creation_input_tokens");
      if (in && cJSON_IsNumber(in))
         out->usage_in = (long)in->valuedouble;
      if (ou && cJSON_IsNumber(ou))
         out->usage_out = (long)ou->valuedouble;
      if (cr && cJSON_IsNumber(cr))
         out->usage_cache_read = (long)cr->valuedouble;
      if (cw && cJSON_IsNumber(cw))
         out->usage_cache_write = (long)cw->valuedouble;
   }
   return 0;
}
