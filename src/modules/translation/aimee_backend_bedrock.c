/* aimee_backend_bedrock.c -- IR <-> AWS Bedrock Converse API. Pure IR<->cJSON, like
 * the anthropic/openai/responses backends: deterministic, fixture-testable against
 * AWS's documented Converse schema, with NO AWS-substrate (SigV4/eventstream)
 * dependency. The Converse body is IDENTICAL for Converse and ConverseStream (they
 * differ only by endpoint), so build is stream-agnostic. modelId is a URI parameter,
 * NOT a body field, so it is never emitted here. See aimee_backend.h. */
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

static cJSON *json_new_object(int *failed)
{
   cJSON *v = cJSON_CreateObject();
   if (!v)
      *failed = 1;
   return v;
}

static cJSON *json_new_array(int *failed)
{
   cJSON *v = cJSON_CreateArray();
   if (!v)
      *failed = 1;
   return v;
}

static int json_add_item_object(cJSON *parent, const char *name, cJSON *item, int *failed)
{
   if (!parent || !item || !cJSON_AddItemToObject(parent, name, item))
   {
      cJSON_Delete(item);
      *failed = 1;
      return -1;
   }
   return 0;
}

static int json_add_item_array(cJSON *parent, cJSON *item, int *failed)
{
   if (!parent || !item || !cJSON_AddItemToArray(parent, item))
   {
      cJSON_Delete(item);
      *failed = 1;
      return -1;
   }
   return 0;
}

static cJSON *json_add_object(cJSON *parent, const char *name, int *failed)
{
   cJSON *item = json_new_object(failed);
   if (!item || json_add_item_object(parent, name, item, failed) != 0)
      return NULL;
   return item;
}

static cJSON *json_add_array(cJSON *parent, const char *name, int *failed)
{
   cJSON *item = json_new_array(failed);
   if (!item || json_add_item_object(parent, name, item, failed) != 0)
      return NULL;
   return item;
}

static int json_add_string(cJSON *parent, const char *name, const char *value, int *failed)
{
   cJSON *item = cJSON_CreateString(value ? value : "");
   if (!item)
      *failed = 1;
   return json_add_item_object(parent, name, item, failed);
}

static int json_add_number(cJSON *parent, const char *name, double value, int *failed)
{
   cJSON *item = cJSON_CreateNumber(value);
   if (!item)
      *failed = 1;
   return json_add_item_object(parent, name, item, failed);
}

/* Derive a Converse image `format` from an IR media_type ("image/png" -> "png").
 * Returns the substring after the '/', or the whole string if there is none. */
/* Converse image.format is a fixed lowercase enum {png,jpeg,gif,webp}. Derive it
 * from the media-type subtype; return NULL for anything not in the enum so the
 * caller OMITS the block rather than emitting a schema-invalid format. */
static const char *converse_image_format(const char *media_type)
{
   if (!media_type)
      return NULL;
   const char *slash = strchr(media_type, '/');
   const char *sub = (slash && slash[1]) ? slash + 1 : media_type;
   if (strcmp(sub, "png") == 0 || strcmp(sub, "jpeg") == 0 || strcmp(sub, "gif") == 0 ||
       strcmp(sub, "webp") == 0)
      return sub;
   if (strcmp(sub, "jpg") == 0)
      return "jpeg"; /* explicit MIME alias normalization to the Converse enum */
   return NULL;
}

/* An IR media_ref is renderable as Converse `source.bytes` (a base64 STRING) ONLY
 * when it is not a URL: Converse has no URL image input on the generic path (S3
 * source.s3Location is a P6c-egress concern), so a URL ref yields no image block. */
static int media_ref_is_base64(const char *ref)
{
   return ref && !strstr(ref, "://");
}

/* Map an IR tool_result (opaque cJSON) to a Converse toolResult content part:
 * a plain-string result -> {text:<str>}; a structured (object/array) result ->
 * {json:<dup>}. NULL/other -> {text:""} (the empty default). */
static cJSON *converse_tool_result_part(const cJSON *tr, int *failed)
{
   cJSON *part = json_new_object(failed);
   if (!part)
      return NULL;
   if (tr && cJSON_IsString(tr))
   {
      if (json_add_string(part, "text", tr->valuestring, failed) != 0)
         goto fail;
   }
   else if (tr && (cJSON_IsObject(tr) || cJSON_IsArray(tr)))
   {
      if (json_add_item_object(part, "json", cJSON_Duplicate((cJSON *)tr, 1), failed) != 0)
         goto fail;
   }
   else
   {
      if (json_add_string(part, "text", "", failed) != 0)
         goto fail;
   }
   return part;
fail:
   cJSON_Delete(part);
   return NULL;
}

/* one IR block -> its Converse content-part JSON (owned). NULL if not renderable. */
static cJSON *block_to_converse(const aimee_block_t *b, int *failed)
{
   switch (b->type)
   {
   case AIMEE_BLK_TEXT:
   {
      cJSON *el = json_new_object(failed);
      if (!el || json_add_string(el, "text", b->text, failed) != 0)
      {
         cJSON_Delete(el);
         return NULL;
      }
      return el;
   }
   case AIMEE_BLK_TOOL_USE:
   {
      cJSON *el = json_new_object(failed);
      cJSON *tu = el ? json_add_object(el, "toolUse", failed) : NULL;
      if (!tu || json_add_string(tu, "toolUseId", b->tool_id, failed) != 0)
         goto tool_use_fail;
      /* Converse toolUse.input MUST be a JSON object; the IR's opaque tool_input
       * is an object for a well-formed call, but guard a non-object (string/array/
       * scalar) into an empty object so the emitted body stays schema-valid. */
      if (json_add_string(tu, "name", b->tool_name, failed) != 0 ||
          json_add_item_object(tu, "input",
                               (b->tool_input && cJSON_IsObject(b->tool_input))
                                   ? cJSON_Duplicate(b->tool_input, 1)
                                   : cJSON_CreateObject(),
                               failed) != 0)
         goto tool_use_fail;
      return el;
   tool_use_fail:
      cJSON_Delete(el);
      return NULL;
   }
   case AIMEE_BLK_TOOL_RESULT:
   {
      cJSON *el = json_new_object(failed);
      cJSON *tr = el ? json_add_object(el, "toolResult", failed) : NULL;
      if (!tr || json_add_string(tr, "toolUseId", b->tool_id, failed) != 0)
         goto tool_result_fail;
      cJSON *content = json_add_array(tr, "content", failed);
      if (!content ||
          json_add_item_array(content, converse_tool_result_part(b->tool_result, failed), failed) !=
              0 ||
          json_add_string(tr, "status", b->tool_is_error ? "error" : "success", failed) != 0)
         goto tool_result_fail;
      return el;
   tool_result_fail:
      cJSON_Delete(el);
      return NULL;
   }
   case AIMEE_BLK_IMAGE:
   {
      /* Converse takes image bytes as a base64 string; a URL ref has no generic
       * Converse spelling -> omit the block (documented; S3 is P6c-egress). The
       * format must be a valid Converse enum, else the block is omitted (never a
       * schema-invalid format). */
      const char *fmt = converse_image_format(b->media_type);
      if (!fmt || !media_ref_is_base64(b->media_ref))
         return NULL;
      cJSON *el = json_new_object(failed);
      cJSON *img = el ? json_add_object(el, "image", failed) : NULL;
      if (!img || json_add_string(img, "format", fmt, failed) != 0)
         goto image_fail;
      cJSON *src = json_add_object(img, "source", failed);
      if (!src || json_add_string(src, "bytes", b->media_ref, failed) != 0)
         goto image_fail;
      return el;
   image_fail:
      cJSON_Delete(el);
      return NULL;
   }
   case AIMEE_BLK_THINKING:
   {
      if (!b->text || !b->text[0])
         return NULL; /* skip an empty reasoning block */
      cJSON *el = json_new_object(failed);
      cJSON *rc = el ? json_add_object(el, "reasoningContent", failed) : NULL;
      cJSON *rt = rc ? json_add_object(rc, "reasoningText", failed) : NULL;
      if (!rt || json_add_string(rt, "text", b->text, failed) != 0 ||
          (b->thinking_signature &&
           json_add_string(rt, "signature", b->thinking_signature, failed) != 0))
      {
         cJSON_Delete(el);
         return NULL;
      }
      return el;
   }
   case AIMEE_BLK_DOCUMENT:
      /* Converse `document` bytes are deferred (P6c-egress); no safe generic form. */
      return NULL;
   case AIMEE_BLK_UNKNOWN:
   default:
      /* Replay the raw sidecar ONLY if it is already a Converse-shaped part -- never
       * leak an openai/anthropic shape via a catch-all. A raw object counts as
       * Converse-shaped iff it carries a known Converse content key. */
      if (b->raw && cJSON_IsObject(b->raw) &&
          (cJSON_GetObjectItemCaseSensitive(b->raw, "text") ||
           cJSON_GetObjectItemCaseSensitive(b->raw, "toolUse") ||
           cJSON_GetObjectItemCaseSensitive(b->raw, "toolResult") ||
           cJSON_GetObjectItemCaseSensitive(b->raw, "image") ||
           cJSON_GetObjectItemCaseSensitive(b->raw, "reasoningContent")))
      {
         cJSON *copy = cJSON_Duplicate(b->raw, 1);
         if (!copy)
            *failed = 1;
         return copy;
      }
      return NULL;
   }
}

static cJSON *blocks_to_converse(const aimee_block_t *blocks, int n, int *failed)
{
   cJSON *arr = json_new_array(failed);
   if (!arr)
      return NULL;
   for (int i = 0; i < n; i++)
   {
      cJSON *el = block_to_converse(&blocks[i], failed);
      if (el)
      {
         if (json_add_item_array(arr, el, failed) != 0)
            goto fail;
      }
      else if (*failed)
         goto fail;
   }
   return arr;
fail:
   cJSON_Delete(arr);
   return NULL;
}

/* Converse `system[]` is text/guardContent only: emit a {text:...} part per TEXT
 * system block; skip non-text/empty. Returns a fresh array (may be empty). */
static cJSON *system_to_converse(const aimee_block_t *blocks, int n, int *failed)
{
   cJSON *arr = json_new_array(failed);
   if (!arr)
      return NULL;
   for (int i = 0; i < n; i++)
   {
      if (blocks[i].type != AIMEE_BLK_TEXT)
         continue;
      cJSON *el = json_new_object(failed);
      if (!el)
      {
         cJSON_Delete(arr);
         return NULL;
      }
      if (json_add_string(el, "text", blocks[i].text, failed) != 0)
      {
         cJSON_Delete(el);
         cJSON_Delete(arr);
         return NULL;
      }
      if (json_add_item_array(arr, el, failed) != 0)
      {
         cJSON_Delete(arr);
         return NULL;
      }
   }
   return arr;
}

/* Translate the opaque Anthropic-style ir->tool_choice ({type:auto|any|tool,name})
 * into Converse's object-wrapped toolChoice ({auto:{}}|{any:{}}|{tool:{name:X}}).
 * Returns NULL (=> OMIT toolChoice) for an absent/unrecognized shape -- never a bare
 * string, never a malformed object. */
static cJSON *converse_tool_choice(const cJSON *tc, int *failed)
{
   const char *type = ostr(tc, "type");
   if (!type)
      return NULL;
   if (strcmp(type, "auto") == 0)
   {
      cJSON *o = json_new_object(failed);
      if (!o || json_add_item_object(o, "auto", json_new_object(failed), failed) != 0)
      {
         cJSON_Delete(o);
         return NULL;
      }
      return o;
   }
   if (strcmp(type, "any") == 0)
   {
      cJSON *o = json_new_object(failed);
      if (!o || json_add_item_object(o, "any", json_new_object(failed), failed) != 0)
      {
         cJSON_Delete(o);
         return NULL;
      }
      return o;
   }
   if (strcmp(type, "tool") == 0)
   {
      const char *name = ostr(tc, "name");
      if (!name)
         return NULL;
      cJSON *o = json_new_object(failed);
      cJSON *t = o ? json_add_object(o, "tool", failed) : NULL;
      if (!t || json_add_string(t, "name", name, failed) != 0)
      {
         cJSON_Delete(o);
         return NULL;
      }
      return o;
   }
   return NULL;
}

cJSON *bedrock_converse_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   int failed = 0;
   cJSON *out = json_new_object(&failed);
   cJSON *choice = NULL;
   if (!out)
      return NULL;

   /* system[] -- omit entirely if no text system part is produced. */
   if (ir->n_system > 0)
   {
      cJSON *sys = system_to_converse(ir->system, ir->n_system, &failed);
      if (failed)
         goto fail;
      if (cJSON_GetArraySize(sys) > 0)
      {
         if (json_add_item_object(out, "system", sys, &failed) != 0)
            goto fail;
      }
      else
         cJSON_Delete(sys);
   }

   /* messages[] */
   cJSON *msgs = json_add_array(out, "messages", &failed);
   if (!msgs)
      goto fail;
   for (int i = 0; i < ir->n_messages; i++)
   {
      cJSON *m = json_new_object(&failed);
      if (!m || json_add_string(m, "role", ir->messages[i].role ? ir->messages[i].role : "user",
                                &failed) != 0)
      {
         cJSON_Delete(m);
         goto fail;
      }
      cJSON *content =
          blocks_to_converse(ir->messages[i].blocks, ir->messages[i].n_blocks, &failed);
      if (!content)
      {
         cJSON_Delete(m);
         goto fail;
      }
      if (json_add_item_object(m, "content", content, &failed) != 0)
      {
         cJSON_Delete(m);
         goto fail;
      }
      if (json_add_item_array(msgs, m, &failed) != 0)
         goto fail;
   }

   /* inferenceConfig -- only the present sub-fields; omit entirely if none set.
    * (top_k is NOT an inferenceConfig field -- it is additionalModelRequestFields,
    * deferred to P6c-egress -- so it is never emitted here.) */
   if (ir->has_max_tokens || ir->has_temperature || ir->has_top_p || ir->n_stop > 0)
   {
      cJSON *ic = json_add_object(out, "inferenceConfig", &failed);
      if (!ic)
         goto fail;
      if (ir->has_max_tokens)
         if (json_add_number(ic, "maxTokens", ir->max_tokens, &failed) != 0)
            goto fail;
      if (ir->has_temperature)
         if (json_add_number(ic, "temperature", ir->temperature, &failed) != 0)
            goto fail;
      if (ir->has_top_p)
         if (json_add_number(ic, "topP", ir->top_p, &failed) != 0)
            goto fail;
      if (ir->n_stop > 0)
      {
         cJSON *stop = json_add_array(ic, "stopSequences", &failed);
         if (!stop)
            goto fail;
         for (int i = 0; i < ir->n_stop; i++)
            if (json_add_item_array(
                    stop, cJSON_CreateString(ir->stop_sequences[i] ? ir->stop_sequences[i] : ""),
                    &failed) != 0)
               goto fail;
      }
   }

   /* toolConfig -- omit entirely if n_tools==0 AND no toolChoice is produced. */
   choice = converse_tool_choice(ir->tool_choice, &failed);
   if (failed)
      goto fail;
   if (ir->n_tools > 0 || choice)
   {
      cJSON *tcfg = json_add_object(out, "toolConfig", &failed);
      if (!tcfg)
         goto fail;
      if (choice)
      {
         cJSON *choice_item = choice;
         choice = NULL;
         if (json_add_item_object(tcfg, "toolChoice", choice_item, &failed) != 0)
            goto fail;
      }
      if (ir->n_tools > 0)
      {
         cJSON *tools = json_add_array(tcfg, "tools", &failed);
         if (!tools)
            goto fail;
         for (int i = 0; i < ir->n_tools; i++)
         {
            cJSON *t = json_new_object(&failed);
            cJSON *spec = t ? json_add_object(t, "toolSpec", &failed) : NULL;
            if (!spec || json_add_string(spec, "name", ir->tools[i].name, &failed) != 0)
            {
               cJSON_Delete(t);
               goto fail;
            }
            if (ir->tools[i].description)
               if (json_add_string(spec, "description", ir->tools[i].description, &failed) != 0)
               {
                  cJSON_Delete(t);
                  goto fail;
               }
            cJSON *is = json_add_object(spec, "inputSchema", &failed);
            if (!is)
            {
               cJSON_Delete(t);
               goto fail;
            }
            if (json_add_item_object(is, "json",
                                     ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                         : cJSON_CreateObject(),
                                     &failed) != 0)
            {
               cJSON_Delete(t);
               goto fail;
            }
            if (json_add_item_array(tools, t, &failed) != 0)
               goto fail;
         }
      }
   }
   /* choice is non-NULL only inside the branch above (it forces the condition), so
    * there is no path here that would leak it. */

   return out;
fail:
   cJSON_Delete(choice);
   cJSON_Delete(out);
   return NULL;
}

/* --- parse: Converse response -> IR --- */

/* Map a Converse stopReason string to the canonical enum. raw_stop_reason keeps the
 * provider string verbatim in EVERY case, so guardrail-vs-filter is recoverable.
 * Exposed via aimee_backend.h so the ConverseStream delta parser reuses this one
 * mapping (no second source of truth). */
aimee_stop_reason_t converse_stop_reason(const char *sr)
{
   if (!sr)
      return AIMEE_STOP_UNKNOWN;
   if (strcmp(sr, "end_turn") == 0)
      return AIMEE_STOP_END_TURN;
   if (strcmp(sr, "tool_use") == 0)
      return AIMEE_STOP_TOOL_USE;
   if (strcmp(sr, "max_tokens") == 0)
      return AIMEE_STOP_MAX_TOKENS;
   if (strcmp(sr, "stop_sequence") == 0)
      return AIMEE_STOP_STOP_SEQUENCE;
   if (strcmp(sr, "content_filtered") == 0)
      return AIMEE_STOP_CONTENT_FILTER;
   if (strcmp(sr, "guardrail_intervened") == 0)
      return AIMEE_STOP_CONTENT_FILTER;
   return AIMEE_STOP_UNKNOWN;
}

int bedrock_converse_parse(const cJSON *resp, aimee_response_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!resp || !cJSON_IsObject(resp) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "bedrock_converse_parse: null/non-object response");
      return -1;
   }
   const cJSON *output = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "output");
   const cJSON *msg = output ? cJSON_GetObjectItemCaseSensitive((cJSON *)output, "message") : NULL;
   if (!msg || !cJSON_IsObject(msg))
   {
      if (err && errn)
         snprintf(err, errn, "bedrock_converse_parse: missing output.message");
      return -1;
   }

   out->raw = cJSON_Duplicate((cJSON *)resp, 1);
   out->role = dupstr(ostr(msg, "role"));

   const char *sr = ostr(resp, "stopReason");
   out->raw_stop_reason = dupstr(sr);
   out->stop_reason = converse_stop_reason(sr);

   const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)msg, "content");
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
            b->raw = cJSON_Duplicate((cJSON *)el, 1);
            const cJSON *tu = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "toolUse");
            const cJSON *rc = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "reasoningContent");
            const cJSON *tx = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "text");
            if (tu && cJSON_IsObject(tu))
            {
               b->type = AIMEE_BLK_TOOL_USE;
               b->tool_id = dupstr(ostr(tu, "toolUseId"));
               b->tool_name = dupstr(ostr(tu, "name"));
               const cJSON *in = cJSON_GetObjectItemCaseSensitive((cJSON *)tu, "input");
               b->tool_input = in ? cJSON_Duplicate((cJSON *)in, 1) : NULL;
            }
            else if (rc && cJSON_IsObject(rc))
            {
               const cJSON *rt = cJSON_GetObjectItemCaseSensitive((cJSON *)rc, "reasoningText");
               b->type = AIMEE_BLK_THINKING;
               b->text = dupstr(ostr(rt, "text"));
               b->thinking_signature = dupstr(ostr(rt, "signature"));
            }
            else if (tx && cJSON_IsString(tx))
            {
               b->type = AIMEE_BLK_TEXT;
               b->text = dupstr(tx->valuestring);
            }
            else
            {
               b->type = AIMEE_BLK_UNKNOWN; /* raw already retained above */
            }
         }
         out->n_content = n;
      }
   }

   const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)resp, "usage");
   if (usage && cJSON_IsObject(usage))
   {
      const cJSON *in = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "inputTokens");
      const cJSON *ou = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "outputTokens");
      const cJSON *cr = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "cacheReadInputTokens");
      const cJSON *cw = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "cacheWriteInputTokens");
      if (in && cJSON_IsNumber(in))
         out->usage_in = (long)in->valuedouble;
      if (ou && cJSON_IsNumber(ou))
         out->usage_out = (long)ou->valuedouble;
      if (cr && cJSON_IsNumber(cr))
         out->usage_cache_read = (long)cr->valuedouble;
      if (cw && cJSON_IsNumber(cw))
         out->usage_cache_write = (long)cw->valuedouble;
   }
   if (!out->raw || !out->role || !out->raw_stop_reason)
      goto allocation_failure;
   for (int i = 0; i < out->n_content; i++)
   {
      aimee_block_t *b = &out->content[i];
      if (!b->raw || (b->type == AIMEE_BLK_TEXT && !b->text) ||
          (b->type == AIMEE_BLK_TOOL_USE && (!b->tool_id || !b->tool_name || !b->tool_input)) ||
          (b->type == AIMEE_BLK_THINKING && !b->text))
         goto allocation_failure;
   }
   return 0;
allocation_failure:
   aimee_response_free(out);
   if (err && errn)
      snprintf(err, errn, "bedrock_converse_parse: allocation failure");
   return -1;
}
