/* aimee_backend_anthropic.c -- IR <-> Anthropic Messages API (upstream provider).
 * The IR is Anthropic-shaped by design, so this is the most direct mapping; the
 * OpenAI/Responses backends convert the same IR to their wire. See aimee_backend.h. */
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>

#include "cJSON.h"
#include "usage_observation.h"
#include "anthropic_refusal.h"
#include "aimee_sha256.h"
#include <stdint.h>
#include <ctype.h>

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
      int unchanged = aimee_ir_request_wire_equal(&left, &right);
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
      cJSON_AddItemToObject(el, "content",
                            preserve_cache && b->tool_result ? cJSON_Duplicate(b->tool_result, 1)
                                                             : tool_result_content(b->tool_result));
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

int anthropic_ir_block_replayable(const aimee_block_t *block)
{
   if (!block)
      return 0;
   /* Foreign plaintext/Responses opaque reasoning has no Anthropic signature.
    * It stays canonical, but cannot become a fabricated native thinking block. */
   return block->type != AIMEE_BLK_THINKING ||
          (block->thinking_signature && block->thinking_signature[0]) ||
          (block->thinking_redacted && block->thinking_redacted[0]);
}

cJSON *anthropic_ir_render_block(const aimee_block_t *block)
{
   return anthropic_ir_block_replayable(block) && aimee_ir_block_valid(block)
              ? block_to_anthropic(block, 0)
              : NULL;
}

static cJSON *blocks_to_anthropic(const aimee_block_t *blocks, int n, int preserve_cache)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      if (!anthropic_ir_block_replayable(&blocks[i]))
         continue;
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

/* Replay fences bind exact decimal/string content, native message structure and
 * the thinking chain. They do not authenticate a provider signature. Cache
 * markers and request parameters outside system/tools/messages are excluded. */
static const char *binding_string(const cJSON *value, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
}
static int binding_string_equal(const char *a, const char *b)
{
   return a && b ? !strcmp(a, b) : a == b;
}
static void binding_strip_block_cache(cJSON *block)
{
   if (!cJSON_IsObject(block))
      return;
   cJSON_DeleteItemFromObjectCaseSensitive(block, "cache_control");
   const char *type = binding_string(block, "type");
   if (type && !strcmp(type, "tool_result"))
   {
      cJSON *parts = cJSON_GetObjectItemCaseSensitive(block, "content"), *part;
      if (cJSON_IsArray(parts))
         cJSON_ArrayForEach(part, parts) if (cJSON_IsObject(part))
             cJSON_DeleteItemFromObjectCaseSensitive(part, "cache_control");
   }
}
static int binding_hash(char digest[65], const char *tag, const cJSON *value, int cache_mode)
{
   cJSON *copy = value ? cJSON_Duplicate(value, 1) : NULL;
   if (value && !copy)
      return -1;
   if (cache_mode == 1)
      binding_strip_block_cache(copy);
   if (cache_mode == 2 && cJSON_IsArray(copy))
   {
      cJSON *part;
      cJSON_ArrayForEach(part, copy) binding_strip_block_cache(part);
   }
   cJSON *canonical = value ? aimee_ir_json_canonical_copy(copy) : NULL;
   if (value && !canonical)
   {
      cJSON_Delete(copy);
      return -1;
   }
   char *json = value ? cJSON_PrintUnformatted(canonical) : strdup("<absent>");
   cJSON_Delete(canonical);
   cJSON_Delete(copy);
   if (!json)
      return -1;
   size_t tag_length = strlen(tag), length = strlen(json);
   if (length > SIZE_MAX - tag_length - 67)
   {
      free(json);
      return -1;
   }
   char *input = malloc(66 + tag_length + length);
   if (!input)
   {
      free(json);
      return -1;
   }
   memcpy(input, digest, 64);
   input[64] = '|';
   memcpy(input + 65, tag, tag_length);
   input[65 + tag_length] = '|';
   memcpy(input + 66 + tag_length, json, length);
   int rc = aimee_sha256_hex(input, 66 + tag_length + length, digest);
   free(input);
   free(json);
   return rc;
}
static int binding_init(aimee_anthropic_replay_t *state, const cJSON *native)
{
   memset(state, 0, sizeof *state);
   static const char prefix[] = "aimee-anthropic-prefix-v1";
   static const char chain[] = "aimee-anthropic-thinking-chain-v1";
   if (!cJSON_IsObject(native) || aimee_sha256_hex(prefix, sizeof prefix - 1, state->prefix) ||
       aimee_sha256_hex(chain, sizeof chain - 1, state->chain) ||
       binding_hash(state->prefix, "system", cJSON_GetObjectItemCaseSensitive(native, "system"),
                    2) ||
       binding_hash(state->prefix, "tools", cJSON_GetObjectItemCaseSensitive(native, "tools"), 2))
      return -1;
   return 0;
}
typedef struct
{
   int message, block;
} binding_cursor_t;
static const aimee_block_t *binding_next(const aimee_request_t *request, binding_cursor_t *cursor,
                                         const cJSON *native)
{
   const char *type = binding_string(native, "type");
   while (request && cursor->message < request->n_messages)
   {
      const aimee_message_t *message = &request->messages[cursor->message];
      if (cursor->block >= message->n_blocks)
      {
         cursor->message++;
         cursor->block = 0;
         continue;
      }
      const aimee_block_t *block = &message->blocks[cursor->block++];
      if (block->type != AIMEE_BLK_THINKING)
         continue;
      if (type && !strcmp(type, "redacted_thinking"))
      {
         if (binding_string_equal(block->thinking_redacted, binding_string(native, "data")))
            return block;
      }
      else if (!block->thinking_redacted &&
               binding_string_equal(block->text, binding_string(native, "thinking")) &&
               binding_string_equal(block->thinking_signature, binding_string(native, "signature")))
         return block;
   }
   return NULL;
}
static int binding_header(aimee_anthropic_replay_t *state, const cJSON *message)
{
   cJSON *header = cJSON_Duplicate(message, 1);
   if (!header)
      return -1;
   cJSON_DeleteItemFromObjectCaseSensitive(header, "content");
   int rc = binding_hash(state->prefix, "message", header, 0);
   cJSON_Delete(header);
   return rc;
}
/* mode 0: actual sent request; 1: stamp native entry; 2: guard egress;
 * mode 3: verify exact replay without modifying the native object. */
static int binding_walk(cJSON *native, const aimee_request_t *request,
                        aimee_anthropic_replay_t *state, int mode, int *kept, int *opaque)
{
   if (binding_init(state, native))
      return -1;
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(native, "messages");
   if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) > AIMEE_IR_MAX_BLOCKS)
      return -1;
   binding_cursor_t cursor = {0};
   int retained_thinking = 0;
   cJSON *message = messages->child;
   while (message)
   {
      cJSON *next_message = message->next;
      char before_message[65];
      memcpy(before_message, state->prefix, sizeof before_message);
      if (!cJSON_IsObject(message) || binding_header(state, message))
         return -1;
      cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
      int removed = 0;
      if (cJSON_IsString(content))
      {
         if (binding_hash(state->prefix, "content-string", content, 0))
            return -1;
      }
      else if (cJSON_IsArray(content))
      {
         cJSON *block = content->child;
         while (block)
         {
            cJSON *next_block = block->next;
            const char *type = binding_string(block, "type");
            if (!type)
               return -1;
            if (!strcmp(type, "thinking") || !strcmp(type, "redacted_thinking"))
            {
               char before[65], chain[65], expected[199];
               const aimee_block_t *source = mode ? binding_next(request, &cursor, block) : NULL;
               memcpy(before, state->chain, sizeof before);
               /* Removing the oldest thinking blocks is legal. The first retained
                * block may start at its recorded predecessor; later gaps are not. */
               if (mode >= 2 && !retained_thinking && source && source->thinking_binding &&
                   aimee_ir_thinking_binding_valid(source->thinking_binding))
               {
                  memcpy(before, source->thinking_binding + 69, 64);
                  before[64] = 0;
               }
               memcpy(chain, before, sizeof chain);
               if (binding_hash(chain, "thinking", block, 1))
                  return -1;
               snprintf(expected, sizeof expected, "ap1:%s:%s:%s", state->prefix, before, chain);
               if (mode == 1)
               {
                  if (!source)
                     return -1;
                  char *copy = strdup(expected);
                  if (!copy)
                     return -1;
                  aimee_block_t *owned = (aimee_block_t *)source;
                  free(owned->thinking_binding);
                  owned->thinking_binding = copy;
               }
               else if (mode >= 2 &&
                        (!source || !anthropic_ir_block_replayable(source) ||
                         !source->thinking_binding || strcmp(source->thinking_binding, expected)))
               {
                  if (mode == 3)
                     return 1;
                  cJSON_Delete(cJSON_DetachItemViaPointer(content, block));
                  removed = 1;
                  block = next_block;
                  continue;
               }
               memcpy(state->chain, chain, sizeof chain);
               retained_thinking = 1;
               if (kept)
                  (*kept)++;
               if (opaque && source &&
                   (source->thinking_signature || source->thinking_redacted ||
                    source->thinking_encrypted_content))
                  (*opaque)++;
            }
            else if (binding_hash(state->prefix, "block", block, 1))
               return -1;
            block = next_block;
         }
      }
      else
         return -1;
      if (mode == 2 && removed && !cJSON_GetArraySize(content))
      {
         cJSON_Delete(cJSON_DetachItemViaPointer(messages, message));
         memcpy(state->prefix, before_message, sizeof before_message);
      }
      else if (binding_hash(state->prefix, "message-end", NULL, 0))
         return -1;
      message = next_message;
   }
   state->valid = 1;
   return 0;
}
int aimee_anthropic_bind_request(const cJSON *native, aimee_request_t *request)
{
   int thinking = 0;
   for (int i = 0; i < request->n_messages; i++)
      for (int j = 0; j < request->messages[i].n_blocks; j++)
         thinking |= request->messages[i].blocks[j].type == AIMEE_BLK_THINKING;
   if (!thinking)
      return 0;
   aimee_anthropic_replay_t state;
   return binding_walk((cJSON *)native, request, &state, 1, NULL, NULL);
}
int aimee_anthropic_replay_prepare(const void *frame, size_t length,
                                   aimee_anthropic_replay_t *state)
{
   const char *bytes = frame;
   if (!state)
      return -1;
   memset(state, 0, sizeof *state);
   if (!bytes || !length || length == SIZE_MAX)
      return -1;
   cJSON *native = aimee_ir_json_parse_frame(bytes, length);
   int rc = native ? binding_walk(native, NULL, state, 0, NULL, NULL) : -1;
   cJSON_Delete(native);
   return rc;
}
int aimee_anthropic_bind_response(const aimee_anthropic_replay_t *request,
                                  aimee_response_t *response)
{
   if (!request || !request->valid || !response)
      return -1;
   aimee_anthropic_replay_t state = *request;
   cJSON *header = cJSON_CreateObject();
   if (!header || !cJSON_AddStringToObject(header, "role", "assistant") ||
       binding_header(&state, header))
   {
      cJSON_Delete(header);
      return -1;
   }
   cJSON_Delete(header);
   for (int i = 0; i < response->n_content; i++)
   {
      aimee_block_t *block = &response->content[i];
      cJSON *native = block_to_anthropic(block, 1);
      if (!native)
         return -1;
      int rc;
      if (block->type == AIMEE_BLK_THINKING)
      {
         char before[65];
         memcpy(before, state.chain, sizeof before);
         rc = binding_hash(state.chain, "thinking", native, 1);
         if (!rc)
         {
            char binding[199];
            snprintf(binding, sizeof binding, "ap1:%s:%s:%s", state.prefix, before, state.chain);
            char *copy = strdup(binding);
            if (!copy)
               rc = -1;
            else
            {
               free(block->thinking_binding);
               block->thinking_binding = copy;
            }
         }
      }
      else
         rc = binding_hash(state.prefix, "block", native, 1);
      cJSON_Delete(native);
      if (rc)
         return -1;
   }
   return 0;
}
int aimee_anthropic_replay_valid(const aimee_request_t *request, const cJSON *native)
{
   aimee_anthropic_replay_t state;
   return binding_walk((cJSON *)native, request, &state, 3, NULL, NULL) == 0;
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
int anthropic_ir_set_host_system(aimee_request_t *ir, const char *system, int cache_marking,
                                 int min_chars)
{
   if (!ir || ir->frontend != AIMEE_WIRE_ANTHROPIC || ir->system || ir->n_system || ir->raw ||
       ir->raw_bytes)
      return -1;
   if (!system || !system[0])
      return 0;
   const char *context = strstr(system, "<aimee-context");
   size_t prefix = context ? (size_t)(context - system) : strlen(system);
   int content = 0;
   for (size_t i = 0; i < prefix; i++)
      content |= !isspace((unsigned char)system[i]);
   int mark = cache_marking && content && (min_chars <= 0 || prefix >= (size_t)min_chars);
   aimee_request_t prepared = {0};
   int count = mark && context ? 2 : 1;
   prepared.system = calloc((size_t)count, sizeof *prepared.system);
   if (!prepared.system)
      goto fail;
   prepared.n_system = count;
   for (int i = 0; i < prepared.n_system; i++)
      prepared.system[i].type = AIMEE_BLK_TEXT;
   prepared.system[0].text = mark ? strndup(system, prefix) : strdup(system);
   if (!prepared.system[0].text)
      goto fail;
   if (mark)
   {
      prepared.system[0].cache_control = strdup("ephemeral");
      if (!prepared.system[0].cache_control)
         goto fail;
   }
   if (prepared.n_system == 2)
   {
      prepared.system[1].text = strdup(context);
      if (!prepared.system[1].text)
         goto fail;
   }
   ir->system = prepared.system;
   ir->n_system = prepared.n_system;
   ir->system_scalar = !mark;
   return 0;
fail:
   aimee_request_free(&prepared);
   return -1;
}

static cJSON *native_request_overlay(const aimee_request_t *ir, cJSON *out)
{
   aimee_request_t original = {0};
   char error[128];
   if (anthropic_frontend_parse(ir->raw, &original, error, sizeof error) == 0)
   {
      aimee_request_t left = {.system = ir->system, .n_system = ir->n_system};
      aimee_request_t right = {.system = original.system, .n_system = original.n_system};
      int failed = aimee_ir_request_wire_equal(&left, &right) &&
                   restore_native_section(out, ir->raw, "system");
      left = (aimee_request_t){.messages = ir->messages, .n_messages = ir->n_messages};
      right = (aimee_request_t){.messages = original.messages, .n_messages = original.n_messages};
      failed |= !anthropic_ir_omitted_empty_messages(ir) &&
                aimee_ir_request_wire_equal(&left, &right) &&
                restore_native_section(out, ir->raw, "messages");
      left = (aimee_request_t){.tools = ir->tools, .n_tools = ir->n_tools};
      right = (aimee_request_t){.tools = original.tools, .n_tools = original.n_tools};
      failed |= aimee_ir_request_wire_equal(&left, &right) &&
                restore_native_section(out, ir->raw, "tools");
      aimee_request_free(&original);
      if (failed)
      {
         cJSON_Delete(out);
         return NULL;
      }
   }
   else
      aimee_request_free(&original);
   static const char *const fields[] = {"model",       "system",         "messages",     "tools",
                                        "tool_choice", "max_tokens",     "temperature",  "top_p",
                                        "top_k",       "metadata",       "service_tier", "thinking",
                                        "stream",      "stop_sequences", "output_config"};
   return native_overlay(ir->raw, out, fields, sizeof fields / sizeof fields[0]);
}

static int last_native_user(const aimee_request_t *request)
{
   for (int i = request->n_messages - 1; i >= 0; i--)
      if (request->messages[i].role &&
          (!strcmp(request->messages[i].role, "user") || !strcmp(request->messages[i].role, "tool")))
         return i;
   return -1;
}

int anthropic_ir_omitted_empty_messages(const aimee_request_t *request)
{
   if (!request)
      return 0;
   int last = last_native_user(request), omitted = 0;
   for (int i = 0; i < last; i++)
      omitted += !request->messages[i].n_blocks && request->messages[i].role &&
                 !strcmp(request->messages[i].role, "assistant");
   return omitted;
}

static cJSON *anthropic_backend_build_unbound(const aimee_request_t *ir)
{
   if (aimee_ir_request_admit(ir))
      return NULL;
   for (int i = 0; i < ir->n_system; i++)
      if (ir->system[i].type == AIMEE_BLK_THINKING)
         return NULL;
   /* Native replay retains client cache breakpoints, TTLs, field ordering and
    * unknown native settings. Verify the typed payload against its entry
    * snapshot as well as the mutation flag: a stage forgetting to report an
    * edit must never cause stale user/tool content to be replayed. */
   if (ir->frontend == AIMEE_WIRE_ANTHROPIC && ir->raw && ir->cache_policy == AIMEE_CACHE_DEFAULT &&
       !anthropic_ir_omitted_empty_messages(ir))
   {
      aimee_request_t original = {0};
      char error[128];
      if (anthropic_frontend_parse(ir->raw, &original, error, sizeof error) == 0)
      {
         int same_model = (!ir->model && !original.model) ||
                          (ir->model && original.model && !strcmp(ir->model, original.model));
         int unchanged = same_model && aimee_ir_request_wire_equal(ir, &original);
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
   if (ir->output_format.kind == AIMEE_OUTPUT_JSON_OBJECT)
   {
      cJSON_Delete(out);
      return NULL; /* This wire has schema-constrained output, not JSON-object mode. */
   }
   const cJSON *original_output = ir->frontend == AIMEE_WIRE_ANTHROPIC
                                      ? cJSON_GetObjectItemCaseSensitive(ir->raw, "output_config")
                                      : NULL;
   cJSON *output_config =
       cJSON_IsObject(original_output) ? cJSON_Duplicate(original_output, 1) : cJSON_CreateObject();
   if (!output_config)
   {
      cJSON_Delete(out);
      return NULL;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(output_config, "format");
   cJSON_DeleteItemFromObjectCaseSensitive(output_config, "effort");
   if (ir->reasoning_effort != AIMEE_EFFORT_UNSET)
   {
      const char *effort = aimee_ir_effort_name(ir->reasoning_effort);
      if (!effort || !cJSON_AddStringToObject(output_config, "effort", effort))
      {
         cJSON_Delete(output_config);
         cJSON_Delete(out);
         return NULL;
      }
   }
   if (ir->output_format.kind == AIMEE_OUTPUT_JSON_SCHEMA)
   {
      cJSON *format = cJSON_AddObjectToObject(output_config, "format");
      if (!cJSON_IsObject(ir->output_format.schema) || !format ||
          !cJSON_AddStringToObject(format, "type", "json_schema") ||
          !cJSON_AddItemToObject(format, "schema", cJSON_Duplicate(ir->output_format.schema, 1)))
      {
         cJSON_Delete(output_config);
         cJSON_Delete(out);
         return NULL;
      }
   }
   if (cJSON_GetArraySize(output_config))
      cJSON_AddItemToObject(out, "output_config", output_config);
   else
      cJSON_Delete(output_config);
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
      if (preserve_cache && ir->system_scalar && ir->n_system == 1 &&
          ir->system[0].type == AIMEE_BLK_TEXT && !ir->system[0].cache_control &&
          !ir->system[0].cache_ttl)
      {
         cJSON_Delete(sys);
         sys = cJSON_CreateString(ir->system[0].text ? ir->system[0].text : "");
         if (!sys)
         {
            cJSON_Delete(out);
            return NULL;
         }
      }
      cJSON_AddItemToObject(out, "system", sys);
      if (!preserve_cache)
         mark_cache_prefix(sys);
   }
   cJSON *msgs = cJSON_AddArrayToObject(out, "messages");
   int last_user = last_native_user(ir);
   for (int i = 0; i < ir->n_messages; i++)
   {
      if (i < last_user && !ir->messages[i].n_blocks && ir->messages[i].role &&
          !strcmp(ir->messages[i].role, "assistant"))
         continue; /* An observed empty assistant stays canonical, never fabricated native text. */
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
      if (!cJSON_GetArraySize(content) && ir->messages[i].n_blocks > 0)
      {
         cJSON_Delete(content);
         cJSON_Delete(m);
         continue; /* Only incompatible private reasoning remained. */
      }
      if (!preserve_cache)
         strip_cache_control(content);
      if (preserve_cache && ir->messages[i].content_scalar && ir->messages[i].n_blocks == 1 &&
          ir->messages[i].blocks[0].type == AIMEE_BLK_TEXT &&
          !ir->messages[i].blocks[0].cache_control)
      {
         cJSON_Delete(content);
         content = cJSON_CreateString(
             ir->messages[i].blocks[0].text ? ir->messages[i].blocks[0].text : "");
         if (!content)
         {
            cJSON_Delete(m);
            cJSON_Delete(out);
            return NULL;
         }
      }
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
         const aimee_tool_t *tool = &ir->tools[i];
         if (!aimee_ir_tool_valid(tool) || tool->kind == AIMEE_TOOL_NAMESPACE ||
             (tool->kind == AIMEE_TOOL_HOSTED && tool->provider_wire != AIMEE_WIRE_ANTHROPIC) ||
             (tool->kind == AIMEE_TOOL_FUNCTION && tool->native_options &&
              tool->provider_wire != AIMEE_WIRE_ANTHROPIC))
         {
            cJSON_Delete(out);
            return NULL;
         }
         cJSON *t = tool->kind == AIMEE_TOOL_HOSTED && tool->native_options
                        ? cJSON_Duplicate(tool->native_options, 1)
                        : cJSON_CreateObject();
         if (tool->kind == AIMEE_TOOL_HOSTED)
         {
            cJSON_DeleteItemFromObjectCaseSensitive(t, "type");
            cJSON_AddStringToObject(t, "type", tool->native_type);
         }
         if (tool->strict == AIMEE_STRICT_TRUE || tool->strict == AIMEE_STRICT_FALSE)
         {
            cJSON_DeleteItemFromObjectCaseSensitive(t, "strict");
            cJSON_AddBoolToObject(t, "strict", tool->strict == AIMEE_STRICT_TRUE);
         }
         if (tool->has_defer_loading)
         {
            cJSON_DeleteItemFromObjectCaseSensitive(t, "defer_loading");
            cJSON_AddBoolToObject(t, "defer_loading", tool->defer_loading);
         }
         cJSON_AddStringToObject(t, "name", tool->name ? tool->name : "");
         if (ir->tools[i].description)
            cJSON_AddStringToObject(t, "description", ir->tools[i].description);
         if (ir->tools[i].schema ||
             (tool->kind == AIMEE_TOOL_FUNCTION && (!preserve_cache || !ir->tools[i].raw)))
            cJSON_AddItemToObject(t, "input_schema",
                                  ir->tools[i].schema ? cJSON_Duplicate(ir->tools[i].schema, 1)
                                                      : cJSON_CreateObject());
         if (preserve_cache)
            add_cache_control(t, ir->tools[i].cache_control, ir->tools[i].cache_ttl);
         if (preserve_cache)
         {
            static const char *const fields[] = {"name",          "description", "input_schema",
                                                 "cache_control", "strict",      "defer_loading",
                                                 "type"};
            t = native_overlay(ir->tools[i].raw, t, fields,
                               tool->kind == AIMEE_TOOL_HOSTED ? 7 : 6);
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

cJSON *anthropic_backend_build(const aimee_request_t *request)
{
   cJSON *native = anthropic_backend_build_unbound(request);
   if (!native)
      return NULL;
   aimee_anthropic_replay_t state;
   if (binding_walk(native, request, &state, 2, NULL, NULL))
   {
      cJSON_Delete(native);
      return NULL;
   }
   return native;
}
static cJSON *plain_request_order(cJSON *native)
{
   cJSON *ordered = cJSON_CreateObject();
   if (!ordered)
   {
      cJSON_Delete(native);
      return NULL;
   }
   static const char *const fields[] = {
       "model",  "max_tokens", "metadata", "service_tier", "output_config", "thinking",
       "stream", "system",     "messages", "tools",        "tool_choice",   "stop_sequences"};
   for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
   {
      cJSON *field = cJSON_DetachItemFromObjectCaseSensitive(native, fields[i]);
      if (field && !cJSON_AddItemToObject(ordered, fields[i], field))
      {
         cJSON_Delete(field);
         goto fail;
      }
   }
   while (native->child)
   {
      cJSON *field = cJSON_DetachItemViaPointer(native, native->child);
      if (!cJSON_AddItemToObject(ordered, field->string, field))
      {
         cJSON_Delete(field);
         goto fail;
      }
   }
   cJSON_Delete(native);
   return ordered;
fail:
   cJSON_Delete(native);
   cJSON_Delete(ordered);
   return NULL;
}

cJSON *anthropic_backend_build_plain(const aimee_request_t *ir, int system_last)
{
   if (aimee_ir_request_admit(ir))
      return NULL;
   aimee_request_t content = *ir;
   content.has_temperature = content.has_top_p = content.has_top_k = 0;
   cJSON *out = anthropic_backend_build_unbound(&content);
   if (!out)
      return NULL;
   out = plain_request_order(out);
   if (!out)
      return NULL;
   if (system_last)
   {
      cJSON *system = cJSON_DetachItemFromObjectCaseSensitive(out, "system");
      if (system && !cJSON_AddItemToObject(out, "system", system))
      {
         cJSON_Delete(system);
         goto fail;
      }
   }
   if ((ir->has_temperature && !cJSON_AddNumberToObject(out, "temperature", ir->temperature)) ||
       (ir->has_top_p && !cJSON_AddNumberToObject(out, "top_p", ir->top_p)) ||
       (ir->has_top_k && !cJSON_AddNumberToObject(out, "top_k", ir->top_k)))
      goto fail;
   aimee_anthropic_replay_t state;
   if (binding_walk(out, ir, &state, 2, NULL, NULL))
      goto fail;
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

void aimee_anthropic_projection_report(const aimee_request_t *request,
                                       aimee_wire_projection_report_t *report)
{
   memset(report, 0, sizeof *report);
   aimee_request_t visible = {0};
   const aimee_request_t *view = request;
   if (aimee_ir_request_needs_model_projection(request) &&
       !aimee_ir_request_project_model(request, &visible))
      view = &visible;
   report->omitted_empty_messages = anthropic_ir_omitted_empty_messages(view);
   aimee_request_free(&visible);
   report->omitted_cache_controls = !!request->prompt_cache_key +
                                    !!request->prompt_cache_retention +
                                    !!request->prompt_cache_options;
   for (int i = 0; i < request->n_system; i++)
      if (request->system[i].type == AIMEE_BLK_THINKING)
      {
         report->omitted_reasoning_blocks++;
         report->omitted_opaque_state_blocks +=
             !!(request->system[i].thinking_signature || request->system[i].thinking_redacted ||
                request->system[i].thinking_encrypted_content);
      }
   for (int i = 0; i < request->n_messages; i++)
      for (int j = 0; j < request->messages[i].n_blocks; j++)
      {
         const aimee_block_t *block = &request->messages[i].blocks[j];
         if (block->type == AIMEE_BLK_THINKING)
         {
            report->omitted_reasoning_blocks++;
            report->omitted_opaque_state_blocks +=
                !!(block->thinking_signature || block->thinking_redacted ||
                   block->thinking_encrypted_content);
         }
      }
   cJSON *native = anthropic_backend_build_unbound(request);
   aimee_anthropic_replay_t state;
   int kept = 0, opaque = 0;
   if (native && !binding_walk(native, request, &state, 2, &kept, &opaque))
   {
      report->omitted_reasoning_blocks -= kept;
      report->omitted_opaque_state_blocks -= opaque;
   }
   cJSON_Delete(native);
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
   if (!resp || !cJSON_IsObject(resp) || !out || !aimee_ir_json_unique(resp))
   {
      if (err && errn)
         snprintf(err, errn, "anthropic_backend_parse: null/non-object response");
      return -1;
   }
   const cJSON *failure = cJSON_GetObjectItemCaseSensitive(resp, "error");
   const cJSON *stop_details = cJSON_GetObjectItemCaseSensitive(resp, "stop_details");
   if (failure && !cJSON_IsNull(failure) && stop_details && !cJSON_IsNull(stop_details))
   {
      if (err && errn)
         snprintf(err, errn, "invalid stop details on provider error");
      return -1;
   }
   out->raw = cJSON_Duplicate(resp, 1);
   const cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
   if (aimee_ir_read_usage(usage, "input_tokens", &out->usage_in) ||
       aimee_ir_read_usage(usage, "output_tokens", &out->usage_out) ||
       aimee_ir_read_usage(usage, "cache_read_input_tokens", &out->usage_cache_read) ||
       aimee_ir_read_usage(usage, "cache_creation_input_tokens", &out->usage_cache_write))
   {
      if (err && errn)
         snprintf(err, errn, "invalid provider usage counter");
      aimee_response_free(out);
      return -1;
   }
   wire_response_usage(out, usage, "input_tokens", "output_tokens");

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
   aimee_refusal_details_t refusal = {0};
   int has_refusal = 0;
   if (anthropic_refusal_read(cJSON_GetObjectItemCaseSensitive(resp, "stop_details"), sr, &refusal,
                              &has_refusal))
   {
      if (err && errn)
         snprintf(err, errn, "invalid provider stop details");
      aimee_response_free(out);
      return -1;
   }
   if (has_refusal)
   {
      out->refusal = aimee_refusal_details_clone(&refusal);
      cJSON *safe = anthropic_refusal_json(&refusal);
      if (!out->refusal || !safe ||
          !cJSON_ReplaceItemInObjectCaseSensitive(out->raw, "stop_details", safe))
      {
         cJSON_Delete(safe);
         aimee_response_free(out);
         return -1;
      }
   }

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

   return 0;
}
