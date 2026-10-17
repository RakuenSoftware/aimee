/* aimee_backend_anthropic.c -- IR <-> Anthropic Messages API (upstream provider).
 * The IR is Anthropic-shaped by design, so this is the most direct mapping; the
 * OpenAI/Responses backends convert the same IR to their wire. See aimee_backend.h. */
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
   return s ? strdup(s) : NULL;
}

static void add_cache_control(cJSON *el, const char *cc, const char *ttl)
{
   if (!cc)
      return;
   cJSON *o = cJSON_AddObjectToObject(el, "cache_control");
   cJSON_AddStringToObject(o, "type", cc[0] ? cc : "ephemeral");
   if (ttl)
      cJSON_AddStringToObject(o, "ttl", ttl);
}

/* Explicit stable-prefix policy normalizes cache markers for translated requests.
 * Native Anthropic requests preserve client breakpoints and TTLs by default. */
static void strip_cache_control(cJSON *arr)
{
   if (!cJSON_IsArray(arr))
      return;
   int n = cJSON_GetArraySize(arr);
   for (int i = 0; i < n; i++)
   {
      cJSON *el = cJSON_GetArrayItem(arr, i);
      if (el && cJSON_IsObject(el))
         cJSON_DeleteItemFromObjectCaseSensitive(el, "cache_control");
   }
}

static void mark_cache_prefix(cJSON *arr)
{
   /* This operates only on provider-generated translated structures. Native
    * Anthropic parity requests bypass this backend and retain client cache fields.
    * Cache decoration is backend policy and is never controlled by economizer mode. */
   strip_cache_control(arr);
   if (!cJSON_IsArray(arr))
      return;
   int n = cJSON_GetArraySize(arr);
   if (n <= 0)
      return;
   cJSON *last = cJSON_GetArrayItem(arr, n - 1);
   if (last && cJSON_IsObject(last))
      add_cache_control(last, "ephemeral", NULL);
}

/* Canonicalize a tool_result's content so an anthropic-sourced tool_result (verbatim
 * block array) and an openai-sourced one (string) with the same text serialize
 * IDENTICALLY. An empty or single-text-block array collapses to the plain string form
 * (matching the NULL default and the OpenAI string). Multi-block / non-text / object
 * content is preserved verbatim -- canonicalizing image/document blocks *inside* a
 * tool_result is out of scope (the IR stores tool_result content opaquely). */
static cJSON *tool_result_content(const cJSON *tr)
{
   if (!tr)
      return cJSON_CreateString("");
   if (cJSON_IsArray(tr))
   {
      int n = cJSON_GetArraySize(tr);
      if (n == 0)
         return cJSON_CreateString(""); /* empty content == the NULL default, not [] */
      if (n == 1)
      {
         cJSON *only = cJSON_GetArrayItem((cJSON *)tr, 0);
         const cJSON *t = only ? cJSON_GetObjectItemCaseSensitive(only, "type") : NULL;
         if (t && cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, "text") == 0)
         {
            const cJSON *txt = cJSON_GetObjectItemCaseSensitive(only, "text");
            return cJSON_CreateString(
                (txt && cJSON_IsString(txt) && txt->valuestring) ? txt->valuestring : "");
         }
      }
   }
   return cJSON_Duplicate((cJSON *)tr, 1);
}

/* one IR block -> its Anthropic wire JSON (owned). NULL if not renderable. */
static cJSON *block_to_anthropic(const aimee_block_t *b, int preserve_cache)
{
   if (preserve_cache && cJSON_IsObject(b->raw))
   {
      aimee_block_t original = {0};
      anthropic_ir_parse_block(b->raw, &original);
      aimee_block_t current = *b;
      current.context = original.context; /* Host provenance does not alter native bytes. */
      current.message_group = original.message_group;
      aimee_request_t left = {.system = &current, .n_system = 1};
      aimee_request_t right = {.system = &original, .n_system = 1};
      int unchanged = aimee_ir_request_equal(&left, &right);
      aimee_block_free_contents(&original);
      if (unchanged)
         return cJSON_Duplicate(b->raw, 1);
   }
   cJSON *el = cJSON_CreateObject();
   switch (b->type)
   {
   case AIMEE_BLK_TEXT:
      cJSON_AddStringToObject(el, "type", "text");
      cJSON_AddStringToObject(el, "text", b->text ? b->text : "");
      break;
   case AIMEE_BLK_THINKING:
      if (b->thinking_redacted)
      {
         cJSON_AddStringToObject(el, "type", "redacted_thinking");
         cJSON_AddStringToObject(el, "data", b->thinking_redacted);
         break;
      }
      cJSON_AddStringToObject(el, "type", "thinking");
      cJSON_AddStringToObject(el, "thinking", b->text ? b->text : "");
      if (b->thinking_signature)
         cJSON_AddStringToObject(el, "signature", b->thinking_signature);
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
      cJSON_AddItemToObject(el, "content", tool_result_content(b->tool_result));
      break;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
   {
      cJSON_AddStringToObject(el, "type", b->type == AIMEE_BLK_IMAGE ? "image" : "document");
      cJSON *src = cJSON_AddObjectToObject(el, "source");
      const char *ref = b->media_ref ? b->media_ref : "";
      if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8) ||
          (!b->media_type && strncmp(ref, "data:", 5) && strncmp(ref, "file-", 5)))
      {
         cJSON_AddStringToObject(src, "type", "url");
         cJSON_AddStringToObject(src, "url", ref);
      }
      else
      {
         const char *data = ref, *type = b->media_type;
         char *owned_type = NULL;
         if (!strncmp(ref, "data:", 5))
         {
            const char *comma = strchr(ref + 5, ',');
            const char *separator = comma && comma - ref >= 12 ? comma - 7 : NULL;
            if (!separator || strncmp(separator, ";base64", 7))
            {
               /* A delimiter inside the payload is not a base64 header. */
               cJSON_AddStringToObject(src, "type", "url");
               cJSON_AddStringToObject(src, "url", ref);
               break;
            }
            owned_type = strndup(ref + 5, (size_t)(separator - ref - 5));
            if (!owned_type)
            {
               cJSON_Delete(el);
               return NULL;
            }
            type = owned_type;
            data = separator + 8;
         }
         if (!type || !type[0] || !strncmp(ref, "file-", 5))
         {
            free(owned_type);
            cJSON_Delete(el);
            return NULL;
         }
         cJSON_AddStringToObject(src, "type", "base64");
         cJSON_AddStringToObject(src, "media_type", type);
         cJSON_AddStringToObject(src, "data", data);
         free(owned_type);
      }
      break;
   }
   case AIMEE_BLK_UNKNOWN:
   default:
      /* replay the original wire form if we have it, else drop */
      cJSON_Delete(el);
      return b->raw ? cJSON_Duplicate(b->raw, 1) : NULL;
   }
   if (preserve_cache)
      add_cache_control(el, b->cache_control, b->cache_ttl);
   return el;
}

static cJSON *blocks_to_anthropic(const aimee_block_t *blocks, int n, int preserve_cache)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *el = block_to_anthropic(&blocks[i], preserve_cache);
      if (!el || !cJSON_AddItemToArray(arr, el))
      {
         cJSON_Delete(el);
         cJSON_Delete(arr);
         return NULL;
      }
   }
   return arr;
}

/* Render the typed IR tool choice in Anthropic's spelling. Parallel tool use rides
 * inside the choice object as its negation, so a client that only set
 * parallel_tool_calls:false still needs a choice object to carry it. */
static void anthropic_add_tool_choice(cJSON *out, const aimee_request_t *ir)
{
   const char *type = NULL;
   switch (ir->tool_choice)
   {
   case AIMEE_TOOL_CHOICE_AUTO:
      type = "auto";
      break;
   case AIMEE_TOOL_CHOICE_NONE:
      type = "none";
      break;
   case AIMEE_TOOL_CHOICE_ANY:
      type = "any";
      break;
   case AIMEE_TOOL_CHOICE_TOOL:
      type = "tool";
      break;
   case AIMEE_TOOL_CHOICE_UNSET:
      if (ir->has_parallel_tool_calls && !ir->parallel_tool_calls)
         type = "auto";
      break;
   }
   if (!type)
      return;
   cJSON *tc = cJSON_AddObjectToObject(out, "tool_choice");
   if (!tc)
      return;
   cJSON_AddStringToObject(tc, "type", type);
   if (ir->tool_choice == AIMEE_TOOL_CHOICE_TOOL)
      cJSON_AddStringToObject(tc, "name", ir->tool_choice_name ? ir->tool_choice_name : "");
   if (ir->has_parallel_tool_calls && ir->tool_choice != AIMEE_TOOL_CHOICE_NONE)
      cJSON_AddBoolToObject(tc, "disable_parallel_tool_use", ir->parallel_tool_calls ? 0 : 1);
}

/* A native edit replaces every represented field while retaining client-owned
 * extensions. Unchanged sections retain their original representation. */
static cJSON *native_overlay(const cJSON *original, cJSON *generated, const char *const *fields,
                             size_t count)
{
   if (!cJSON_IsObject(original))
      return generated;
   cJSON *out = cJSON_Duplicate(original, 1);
   if (!out)
   {
      cJSON_Delete(generated);
      return NULL;
   }
   for (size_t i = 0; i < count; i++)
   {
      cJSON *field = cJSON_DetachItemFromObjectCaseSensitive(generated, fields[i]);
      cJSON_DeleteItemFromObjectCaseSensitive(out, fields[i]);
      if (field && !cJSON_AddItemToObject(out, fields[i], field))
      {
         cJSON_Delete(field);
         cJSON_Delete(out);
         cJSON_Delete(generated);
         return NULL;
      }
   }
   cJSON_Delete(generated);
   return out;
}
static int restore_native_section(cJSON *out, const cJSON *raw, const char *field)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(raw, field);
   if (!value)
      return 0;
   cJSON *copy = cJSON_Duplicate(value, 1);
   if (!copy)
      return -1;
   cJSON_DeleteItemFromObjectCaseSensitive(out, field);
   if (!cJSON_AddItemToObject(out, field, copy))
   {
      cJSON_Delete(copy);
      return -1;
   }
   return 0;
}
static cJSON *native_request_overlay(const aimee_request_t *ir, cJSON *out)
{
   aimee_request_t original = {0};
   char error[128];
   if (anthropic_frontend_parse(ir->raw, &original, error, sizeof error) == 0)
   {
      aimee_request_t left = {.system = ir->system, .n_system = ir->n_system};
      aimee_request_t right = {.system = original.system, .n_system = original.n_system};
      int failed =
          aimee_ir_request_equal(&left, &right) && restore_native_section(out, ir->raw, "system");
      left = (aimee_request_t){.messages = ir->messages, .n_messages = ir->n_messages};
      right = (aimee_request_t){.messages = original.messages, .n_messages = original.n_messages};
      failed |=
          aimee_ir_request_equal(&left, &right) && restore_native_section(out, ir->raw, "messages");
      left = (aimee_request_t){.tools = ir->tools, .n_tools = ir->n_tools};
      right = (aimee_request_t){.tools = original.tools, .n_tools = original.n_tools};
      failed |=
          aimee_ir_request_equal(&left, &right) && restore_native_section(out, ir->raw, "tools");
      aimee_request_free(&original);
      if (failed)
      {
         cJSON_Delete(out);
         return NULL;
      }
   }
   else
      aimee_request_free(&original);
   static const char *const fields[] = {"model",       "system",        "messages",     "tools",
                                        "tool_choice", "max_tokens",    "temperature",  "top_p",
                                        "top_k",       "metadata",      "service_tier", "thinking",
                                        "stream",      "stop_sequences"};
   return native_overlay(ir->raw, out, fields, sizeof fields / sizeof fields[0]);
}

cJSON *anthropic_backend_build(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   /* Native replay retains client cache breakpoints, TTLs, field ordering and
    * unknown native settings. Verify the typed payload against its entry
    * snapshot as well as the mutation flag: a stage forgetting to report an
    * edit must never cause stale user/tool content to be replayed. */
   if (ir->frontend == AIMEE_WIRE_ANTHROPIC && ir->raw && !ir->mutated &&
       ir->cache_policy == AIMEE_CACHE_DEFAULT)
   {
      aimee_request_t original = {0};
      char error[128];
      if (anthropic_frontend_parse(ir->raw, &original, error, sizeof error) == 0)
      {
         int same_model = (!ir->model && !original.model) ||
                          (ir->model && original.model && !strcmp(ir->model, original.model));
         int unchanged = same_model && aimee_ir_request_equal(ir, &original);
         aimee_request_free(&original);
         if (unchanged)
            return cJSON_Duplicate(ir->raw, 1);
      }
      else
         aimee_request_free(&original);
   }
   int preserve_cache =
       ir->frontend == AIMEE_WIRE_ANTHROPIC && ir->cache_policy == AIMEE_CACHE_DEFAULT;
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
   {
      cJSON *sys = blocks_to_anthropic(ir->system, ir->n_system, preserve_cache);
      if (!sys)
      {
         cJSON_Delete(out);
         return NULL;
      }
      cJSON_AddItemToObject(out, "system", sys);
      if (!preserve_cache)
         mark_cache_prefix(sys);
   }
   cJSON *msgs = cJSON_AddArrayToObject(out, "messages");
   for (int i = 0; i < ir->n_messages; i++)
   {
      cJSON *m = cJSON_CreateObject();
      const char *role = ir->messages[i].role;
      cJSON_AddStringToObject(m, "role", role && strcmp(role, "tool") ? role : "user");
      cJSON *content =
          blocks_to_anthropic(ir->messages[i].blocks, ir->messages[i].n_blocks, preserve_cache);
      if (!content)
      {
         cJSON_Delete(m);
         cJSON_Delete(out);
         return NULL;
      }
      if (!preserve_cache)
         strip_cache_control(content);
      cJSON_AddItemToObject(m, "content", content);
      if (preserve_cache)
      {
         static const char *const fields[] = {"role", "content"};
         m = native_overlay(ir->messages[i].raw, m, fields, 2);
      }
      if (!m || !cJSON_AddItemToArray(msgs, m))
      {
         cJSON_Delete(m);
         cJSON_Delete(out);
         return NULL;
      }
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
         if (ir->tools[i].schema || !preserve_cache || !ir->tools[i].raw)
            cJSON_AddItemToObject(t, "input_schema",
                                  ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                      : cJSON_CreateObject());
         if (preserve_cache)
            add_cache_control(t, ir->tools[i].cache_control, ir->tools[i].cache_ttl);
         if (preserve_cache)
         {
            static const char *const fields[] = {"name", "description", "input_schema",
                                                 "cache_control"};
            t = native_overlay(ir->tools[i].raw, t, fields, 4);
         }
         if (!t || !cJSON_AddItemToArray(tools, t))
         {
            cJSON_Delete(t);
            cJSON_Delete(out);
            return NULL;
         }
      }
      if (!preserve_cache)
         mark_cache_prefix(tools);
   }
   anthropic_add_tool_choice(out, ir);
   if (ir->n_stop > 0)
   {
      cJSON *stop = cJSON_AddArrayToObject(out, "stop_sequences");
      for (int i = 0; i < ir->n_stop; i++)
         cJSON_AddItemToArray(
             stop, cJSON_CreateString(ir->stop_sequences[i] ? ir->stop_sequences[i] : ""));
   }
   return preserve_cache && ir->raw ? native_request_overlay(ir, out) : out;
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
            anthropic_ir_parse_block(el, b);
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
