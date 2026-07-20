/* aimee_backend_bedrock.c -- IR <-> AWS Bedrock Converse API. Pure IR<->cJSON, like
 * the anthropic/openai/responses backends: deterministic, fixture-testable against
 * AWS's documented Converse schema, with NO AWS-substrate (SigV4/eventstream)
 * dependency. The Converse body is IDENTICAL for Converse and ConverseStream (they
 * differ only by endpoint), so build is stream-agnostic. modelId is a URI parameter,
 * NOT a body field, so it is never emitted here. See aimee_backend.h. */
#include "aimee_backend.h"

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
      return "jpeg"; /* common alias -> the Converse enum name */
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
static cJSON *converse_tool_result_part(const cJSON *tr)
{
   cJSON *part = cJSON_CreateObject();
   if (tr && cJSON_IsString(tr))
      cJSON_AddStringToObject(part, "text", tr->valuestring ? tr->valuestring : "");
   else if (tr && (cJSON_IsObject(tr) || cJSON_IsArray(tr)))
      cJSON_AddItemToObject(part, "json", cJSON_Duplicate((cJSON *)tr, 1));
   else
      cJSON_AddStringToObject(part, "text", "");
   return part;
}

/* one IR block -> its Converse content-part JSON (owned). NULL if not renderable. */
static cJSON *block_to_converse(const aimee_block_t *b)
{
   switch (b->type)
   {
   case AIMEE_BLK_TEXT:
   {
      cJSON *el = cJSON_CreateObject();
      cJSON_AddStringToObject(el, "text", b->text ? b->text : "");
      return el;
   }
   case AIMEE_BLK_TOOL_USE:
   {
      cJSON *el = cJSON_CreateObject();
      cJSON *tu = cJSON_AddObjectToObject(el, "toolUse");
      cJSON_AddStringToObject(tu, "toolUseId", b->tool_id ? b->tool_id : "");
      /* Converse toolUse.input MUST be a JSON object; the IR's opaque tool_input
       * is an object for a well-formed call, but guard a non-object (string/array/
       * scalar) into an empty object so the emitted body stays schema-valid. */
      cJSON_AddStringToObject(tu, "name", b->tool_name ? b->tool_name : "");
      cJSON_AddItemToObject(tu, "input",
                            (b->tool_input && cJSON_IsObject(b->tool_input))
                                ? cJSON_Duplicate(b->tool_input, 1)
                                : cJSON_CreateObject());
      return el;
   }
   case AIMEE_BLK_TOOL_RESULT:
   {
      cJSON *el = cJSON_CreateObject();
      cJSON *tr = cJSON_AddObjectToObject(el, "toolResult");
      cJSON_AddStringToObject(tr, "toolUseId", b->tool_id ? b->tool_id : "");
      cJSON *content = cJSON_AddArrayToObject(tr, "content");
      cJSON_AddItemToArray(content, converse_tool_result_part(b->tool_result));
      cJSON_AddStringToObject(tr, "status", b->tool_is_error ? "error" : "success");
      return el;
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
      cJSON *el = cJSON_CreateObject();
      cJSON *img = cJSON_AddObjectToObject(el, "image");
      cJSON_AddStringToObject(img, "format", fmt);
      cJSON *src = cJSON_AddObjectToObject(img, "source");
      cJSON_AddStringToObject(src, "bytes", b->media_ref);
      return el;
   }
   case AIMEE_BLK_THINKING:
   {
      if (!b->text || !b->text[0])
         return NULL; /* skip an empty reasoning block */
      cJSON *el = cJSON_CreateObject();
      cJSON *rc = cJSON_AddObjectToObject(el, "reasoningContent");
      cJSON *rt = cJSON_AddObjectToObject(rc, "reasoningText");
      cJSON_AddStringToObject(rt, "text", b->text);
      if (b->thinking_signature)
         cJSON_AddStringToObject(rt, "signature", b->thinking_signature);
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
         return cJSON_Duplicate(b->raw, 1);
      return NULL;
   }
}

static cJSON *blocks_to_converse(const aimee_block_t *blocks, int n)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *el = block_to_converse(&blocks[i]);
      if (el)
         cJSON_AddItemToArray(arr, el);
   }
   return arr;
}

/* Converse `system[]` is text/guardContent only: emit a {text:...} part per TEXT
 * system block; skip non-text/empty. Returns a fresh array (may be empty). */
static cJSON *system_to_converse(const aimee_block_t *blocks, int n)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      if (blocks[i].type != AIMEE_BLK_TEXT)
         continue;
      cJSON *el = cJSON_CreateObject();
      cJSON_AddStringToObject(el, "text", blocks[i].text ? blocks[i].text : "");
      cJSON_AddItemToArray(arr, el);
   }
   return arr;
}

/* Translate the opaque Anthropic-style ir->tool_choice ({type:auto|any|tool,name})
 * into Converse's object-wrapped toolChoice ({auto:{}}|{any:{}}|{tool:{name:X}}).
 * Returns NULL (=> OMIT toolChoice) for an absent/unrecognized shape -- never a bare
 * string, never a malformed object. */
static cJSON *converse_tool_choice(const cJSON *tc)
{
   const char *type = ostr(tc, "type");
   if (!type)
      return NULL;
   if (strcmp(type, "auto") == 0)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddItemToObject(o, "auto", cJSON_CreateObject());
      return o;
   }
   if (strcmp(type, "any") == 0)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddItemToObject(o, "any", cJSON_CreateObject());
      return o;
   }
   if (strcmp(type, "tool") == 0)
   {
      const char *name = ostr(tc, "name");
      if (!name)
         return NULL;
      cJSON *o = cJSON_CreateObject();
      cJSON *t = cJSON_AddObjectToObject(o, "tool");
      cJSON_AddStringToObject(t, "name", name);
      return o;
   }
   return NULL;
}

cJSON *bedrock_converse_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   cJSON *out = cJSON_CreateObject();

   /* system[] -- omit entirely if no text system part is produced. */
   if (ir->n_system > 0)
   {
      cJSON *sys = system_to_converse(ir->system, ir->n_system);
      if (cJSON_GetArraySize(sys) > 0)
         cJSON_AddItemToObject(out, "system", sys);
      else
         cJSON_Delete(sys);
   }

   /* messages[] */
   cJSON *msgs = cJSON_AddArrayToObject(out, "messages");
   for (int i = 0; i < ir->n_messages; i++)
   {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "role", ir->messages[i].role ? ir->messages[i].role : "user");
      cJSON_AddItemToObject(m, "content",
                            blocks_to_converse(ir->messages[i].blocks, ir->messages[i].n_blocks));
      cJSON_AddItemToArray(msgs, m);
   }

   /* inferenceConfig -- only the present sub-fields; omit entirely if none set.
    * (top_k is NOT an inferenceConfig field -- it is additionalModelRequestFields,
    * deferred to P6c-egress -- so it is never emitted here.) */
   if (ir->has_max_tokens || ir->has_temperature || ir->has_top_p || ir->n_stop > 0)
   {
      cJSON *ic = cJSON_AddObjectToObject(out, "inferenceConfig");
      if (ir->has_max_tokens)
         cJSON_AddNumberToObject(ic, "maxTokens", ir->max_tokens);
      if (ir->has_temperature)
         cJSON_AddNumberToObject(ic, "temperature", ir->temperature);
      if (ir->has_top_p)
         cJSON_AddNumberToObject(ic, "topP", ir->top_p);
      if (ir->n_stop > 0)
      {
         cJSON *stop = cJSON_AddArrayToObject(ic, "stopSequences");
         for (int i = 0; i < ir->n_stop; i++)
            cJSON_AddItemToArray(
                stop, cJSON_CreateString(ir->stop_sequences[i] ? ir->stop_sequences[i] : ""));
      }
   }

   /* toolConfig -- omit entirely if n_tools==0 AND no toolChoice is produced. */
   cJSON *choice = converse_tool_choice(ir->tool_choice);
   if (ir->n_tools > 0 || choice)
   {
      cJSON *tcfg = cJSON_AddObjectToObject(out, "toolConfig");
      if (ir->n_tools > 0)
      {
         cJSON *tools = cJSON_AddArrayToObject(tcfg, "tools");
         for (int i = 0; i < ir->n_tools; i++)
         {
            cJSON *t = cJSON_CreateObject();
            cJSON *spec = cJSON_AddObjectToObject(t, "toolSpec");
            cJSON_AddStringToObject(spec, "name", ir->tools[i].name ? ir->tools[i].name : "");
            if (ir->tools[i].description)
               cJSON_AddStringToObject(spec, "description", ir->tools[i].description);
            cJSON *is = cJSON_AddObjectToObject(spec, "inputSchema");
            cJSON_AddItemToObject(is, "json",
                                  ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                      : cJSON_CreateObject());
            cJSON_AddItemToArray(tools, t);
         }
      }
      if (choice)
         cJSON_AddItemToObject(tcfg, "toolChoice", choice);
   }
   /* choice is non-NULL only inside the branch above (it forces the condition), so
    * there is no path here that would leak it. */

   return out;
}

/* --- parse: Converse response -> IR --- */

/* Map a Converse stopReason string to the canonical enum. raw_stop_reason keeps the
 * provider string verbatim in EVERY case, so guardrail-vs-filter is recoverable. */
static aimee_stop_reason_t converse_stop_reason(const char *sr)
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
   return 0;
}
