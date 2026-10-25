#include <aimee/ir/session_event.h>
#include "cJSON.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static _Thread_local aimee_ir_session_sink_fn session_sink;
static _Thread_local void *session_context;
static _Thread_local int session_capture_failed;

static const char *block_kind(aimee_block_type_t type)
{
   static const char *const names[] = {"text",     "tool_use", "tool_result", "image",
                                       "document", "thinking", "unknown"};
   return type >= AIMEE_BLK_TEXT && type <= AIMEE_BLK_UNKNOWN ? names[type] : NULL;
}

static int add_string(cJSON *object, const char *key, const char *value)
{
   return !value || cJSON_AddStringToObject(object, key, value) != NULL;
}

static int add_copy(cJSON *object, const char *key, const cJSON *value)
{
   if (!value)
      return 1;
   cJSON *copy = cJSON_Duplicate(value, 1);
   if (!copy)
      return 0;
   if (cJSON_AddItemToObject(object, key, copy))
      return 1;
   cJSON_Delete(copy);
   return 0;
}

cJSON *aimee_ir_block_json(const aimee_block_t *block)
{
   if (!aimee_ir_block_valid(block))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   int ok = add_string(out, "type", block_kind(block->type)) &&
            add_string(out, "cache_control", block->cache_control) &&
            add_string(out, "cache_ttl", block->cache_ttl);
   if (block->message_group)
      ok = ok && cJSON_AddNumberToObject(out, "message_group", block->message_group) != NULL;
   cJSON *context = cJSON_AddObjectToObject(out, "context");
   if (!context || !cJSON_AddNumberToObject(context, "origin", block->context.origin) ||
       !cJSON_AddNumberToObject(context, "authority", block->context.authority) ||
       !cJSON_AddNumberToObject(context, "trust", block->context.trust) ||
       !cJSON_AddNumberToObject(context, "sensitivity", block->context.sensitivity) ||
       !cJSON_AddBoolToObject(context, "model_visible", block->context.model_visible) ||
       !add_string(context, "revision_domain", block->context.revision_domain) ||
       !add_string(context, "revision_scope", block->context.revision_scope))
      ok = 0;
   char epoch[32];
   snprintf(epoch, sizeof(epoch), "%llu", block->context.revision_epoch);
   ok = ok && add_string(context, "revision_epoch", epoch);
   switch (block->type)
   {
   case AIMEE_BLK_TEXT:
   case AIMEE_BLK_THINKING:
      ok = ok && add_string(out, "text", block->text ? block->text : "");
      if (block->type == AIMEE_BLK_THINKING)
         ok = ok && add_string(out, "signature", block->thinking_signature) &&
              add_string(out, "thinking_binding", block->thinking_binding) &&
              add_string(out, "thinking_id", block->thinking_id) &&
              add_string(out, "encrypted_content", block->thinking_encrypted_content) &&
              add_string(out, "redacted_content", block->thinking_redacted);
      break;
   case AIMEE_BLK_TOOL_USE:
      ok = ok && add_string(out, "tool_id", block->tool_id) &&
           add_string(out, "name", block->tool_name) &&
           add_string(out, "namespace", block->tool_namespace) &&
           add_copy(out, "input", block->tool_input);
      break;
   case AIMEE_BLK_TOOL_RESULT:
      ok = ok && add_string(out, "tool_id", block->tool_id) &&
           add_copy(out, "content", block->tool_result) &&
           cJSON_AddBoolToObject(out, "is_error", block->tool_is_error) != NULL;
      break;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
      ok = ok && add_string(out, "media_type", block->media_type) &&
           add_string(out, "media_ref", block->media_ref);
      break;
   default:
      ok = 0;
   }
   if (!ok)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

cJSON *aimee_ir_message_json(const aimee_message_t *message)
{
   if (!message || !message->role || !message->role[0] || message->n_blocks < 0 ||
       message->n_blocks > 4096 || (message->n_blocks && !message->blocks))
      return NULL;
   if ((message->content_scalar != 0 && message->content_scalar != 1) ||
       (message->compaction_boundary != 0 && message->compaction_boundary != 1))
      return NULL;
   cJSON *out = cJSON_CreateObject(), *blocks = cJSON_CreateArray();
   if (!out || !blocks || !add_string(out, "role", message->role))
      goto fail;
   if (message->content_scalar && !cJSON_AddBoolToObject(out, "content_scalar", 1))
      goto fail;
   if (message->compaction_boundary && !cJSON_AddBoolToObject(out, "_compaction_boundary", 1))
      goto fail;
   for (int i = 0; i < message->n_blocks; i++)
   {
      cJSON *block = aimee_ir_block_json(&message->blocks[i]);
      if (!block)
         goto fail;
      if (!cJSON_AddItemToArray(blocks, block))
      {
         cJSON_Delete(block);
         goto fail;
      }
   }
   if (!cJSON_AddItemToObject(out, "blocks", blocks))
      goto fail;
   return out;
fail:
   cJSON_Delete(blocks);
   cJSON_Delete(out);
   return NULL;
}

static int read_string(const cJSON *json, const char *key, char **out, int required)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
   if (!value)
      return required ? -1 : 0;
   if (!cJSON_IsString(value) || !value->valuestring)
      return -1;
   *out = strdup(value->valuestring);
   return *out ? 0 : -1;
}

static int read_enum(const cJSON *json, const char *key, int max, int *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
   if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > max ||
       value->valuedouble != value->valueint)
      return -1;
   *out = value->valueint;
   return 0;
}

static int read_context(const cJSON *json, aimee_context_meta_t *out)
{
   if (!json)
      return 0;
   int origin, authority, trust, sensitivity;
   if (!cJSON_IsObject(json) || read_enum(json, "origin", AIMEE_CTX_ORIGIN_MODEL, &origin) ||
       read_enum(json, "authority", AIMEE_CTX_AUTH_POLICY, &authority) ||
       read_enum(json, "trust", AIMEE_CTX_TRUST_VERIFIED, &trust) ||
       read_enum(json, "sensitivity", AIMEE_CTX_SENS_RESTRICTED, &sensitivity))
      return -1;
   const cJSON *visible = cJSON_GetObjectItemCaseSensitive(json, "model_visible");
   const char *domain =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_domain"));
   const char *scope =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_scope"));
   const char *epoch =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "revision_epoch"));
   if (!cJSON_IsBool(visible) || !domain || !scope || !epoch || !*epoch ||
       strlen(domain) >= sizeof out->revision_domain || strlen(scope) >= sizeof out->revision_scope)
      return -1;
   for (const char *p = epoch; *p; p++)
      if (*p < '0' || *p > '9')
         return -1;
   errno = 0;
   unsigned long long revision = strtoull(epoch, NULL, 10);
   if (errno == ERANGE)
      return -1;
   out->origin = origin;
   out->authority = authority;
   out->trust = trust;
   out->sensitivity = sensitivity;
   out->model_visible = cJSON_IsTrue(visible);
   strcpy(out->revision_domain, domain);
   strcpy(out->revision_scope, scope);
   out->revision_epoch = revision;
   return 0;
}

static int parse_block(const cJSON *json, aimee_block_t *out)
{
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "type"));
   if (!cJSON_IsObject(json) || !type)
      return -1;
   int kind;
   for (kind = AIMEE_BLK_TEXT; kind < AIMEE_BLK_UNKNOWN; kind++)
      if (!strcmp(type, block_kind(kind)))
         break;
   if (kind == AIMEE_BLK_UNKNOWN)
      return -1;
   out->type = kind;
   const cJSON *group = cJSON_GetObjectItemCaseSensitive(json, "message_group");
   if (group)
   {
      if (!cJSON_IsNumber(group) || group->valuedouble < 1 || group->valuedouble > 4096 ||
          group->valuedouble != group->valueint)
         return -1;
      out->message_group = group->valueint;
   }
   if (read_context(cJSON_GetObjectItemCaseSensitive(json, "context"), &out->context) ||
       read_string(json, "cache_control", &out->cache_control, 0) ||
       read_string(json, "cache_ttl", &out->cache_ttl, 0))
      return -1;
   const cJSON *value;
   switch (out->type)
   {
   case AIMEE_BLK_TEXT:
   case AIMEE_BLK_THINKING:
      return read_string(json, "text", &out->text, 1) ||
                     read_string(json, "signature", &out->thinking_signature, 0) ||
                     read_string(json, "thinking_binding", &out->thinking_binding, 0) ||
                     !aimee_ir_thinking_binding_valid(out->thinking_binding) ||
                     (out->thinking_binding && out->type != AIMEE_BLK_THINKING) ||
                     read_string(json, "thinking_id", &out->thinking_id, 0) ||
                     read_string(json, "encrypted_content", &out->thinking_encrypted_content, 0) ||
                     read_string(json, "redacted_content", &out->thinking_redacted, 0)
                 ? -1
                 : 0;
   case AIMEE_BLK_TOOL_USE:
      if (read_string(json, "tool_id", &out->tool_id, 1) || !out->tool_id[0] ||
          read_string(json, "name", &out->tool_name, 1) || !out->tool_name[0] ||
          read_string(json, "namespace", &out->tool_namespace, 0))
         return -1;
      value = cJSON_GetObjectItemCaseSensitive(json, "input");
      out->tool_input = value ? cJSON_Duplicate(value, 1) : NULL;
      return value && !out->tool_input ? -1 : 0;
   case AIMEE_BLK_TOOL_RESULT:
      if (read_string(json, "tool_id", &out->tool_id, 1) || !out->tool_id[0])
         return -1;
      value = cJSON_GetObjectItemCaseSensitive(json, "is_error");
      if (!cJSON_IsBool(value))
         return -1;
      out->tool_is_error = cJSON_IsTrue(value);
      value = cJSON_GetObjectItemCaseSensitive(json, "content");
      out->tool_result = value ? cJSON_Duplicate(value, 1) : NULL;
      return value && !out->tool_result ? -1 : 0;
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
      return read_string(json, "media_type", &out->media_type, 0) ||
                     (out->media_type && !out->media_type[0]) ||
                     read_string(json, "media_ref", &out->media_ref, 1) || !out->media_ref[0]
                 ? -1
                 : 0;
   default:
      return -1;
   }
}

int aimee_ir_message_parse(const cJSON *json, aimee_message_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(json, "blocks");
   if (!cJSON_IsObject(json) || !aimee_ir_json_unique(json) || !cJSON_IsArray(blocks) ||
       cJSON_GetArraySize(blocks) > 4096 || read_string(json, "role", &out->role, 1) ||
       !out->role[0])
      goto fail;
   const cJSON *scalar = cJSON_GetObjectItemCaseSensitive(json, "content_scalar");
   if (scalar && !cJSON_IsBool(scalar))
      goto fail;
   out->content_scalar = cJSON_IsTrue(scalar);
   const cJSON *boundary = cJSON_GetObjectItemCaseSensitive(json, "_compaction_boundary");
   if (boundary && !cJSON_IsBool(boundary))
      goto fail;
   out->compaction_boundary = cJSON_IsTrue(boundary);
   int count = cJSON_GetArraySize(blocks);
   if (count)
   {
      out->blocks = calloc((size_t)count, sizeof *out->blocks);
      if (!out->blocks)
         goto fail;
   }
   for (int i = 0; i < count; i++)
   {
      out->n_blocks++;
      if (parse_block(cJSON_GetArrayItem(blocks, i), &out->blocks[i]))
         goto fail;
   }
   return 0;
fail:
   aimee_message_free(out);
   return -1;
}

cJSON *aimee_ir_history_clone(const cJSON *messages)
{
   if (messages && !cJSON_IsArray(messages))
      return NULL;
   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;
   const cJSON *item;
   cJSON_ArrayForEach(item, messages)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(item, &message))
         goto fail;
      cJSON *copy = aimee_ir_message_json(&message);
      aimee_message_free(&message);
      if (!copy || !cJSON_AddItemToArray(out, copy))
      {
         cJSON_Delete(copy);
         goto fail;
      }
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

static _Atomic(aimee_ir_host_history_admission_fn) host_history_admission;
void aimee_ir_history_set_host_admission(aimee_ir_host_history_admission_fn admit)
{
   atomic_store(&host_history_admission, admit);
}
int aimee_ir_history_admit_host(const cJSON *messages)
{
   aimee_ir_host_history_admission_fn admit = atomic_load(&host_history_admission);
   return admit ? admit(messages) : aimee_ir_history_admit(messages);
}

int aimee_ir_history_admit(const cJSON *messages)
{
   if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) > 4096)
      return -1;
   const cJSON *value;
   cJSON_ArrayForEach(value, messages)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(value, &message))
         return -1;
      int unresolved = 0;
      for (int i = 0; i < message.n_blocks; i++)
      {
         const aimee_block_t *block = &message.blocks[i];
         unresolved |= block->type == AIMEE_BLK_TOOL_RESULT &&
                       ((!strcmp(block->context.revision_domain, "native-interruption")) ||
                        (block->context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
                         block->context.authority == AIMEE_CTX_AUTH_DIAGNOSTIC &&
                         !strcmp(block->context.revision_domain, "history-repair") &&
                         !strcmp(block->context.revision_scope, "unresolved-tool-result")));
      }
      aimee_message_free(&message);
      if (unresolved)
         return -1;
   }
   /* Reuse the ordered cycle contract on a private comparison view. Refusal
    * never changes the source or presents a synthetic repair as actual output. */
   cJSON *view = cJSON_Duplicate(messages, 1);
   int repairs = view ? aimee_ir_history_repair(view) : -1;
   cJSON_Delete(view);
   return repairs == 0 ? 0 : -1;
}

static int flush_pending(cJSON *messages, const char **ids, int count)
{
   int repaired = 0;
   for (int i = 0; i < count; i++)
   {
      if (!ids[i])
         continue;
      aimee_block_t block = {.type = AIMEE_BLK_TOOL_RESULT,
                             .tool_id = (char *)ids[i],
                             .tool_result = cJSON_CreateString(
                                 "[No tool result is available in this history. Execution outcome "
                                 "is unknown; reconcile before retrying.]"),
                             .tool_is_error = 1};
      /* Missing history proves neither execution nor cancellation. This is a
       * platform diagnostic, never a tool owner's outcome/effect receipt. */
      aimee_ir_block_set_context(&block, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_DIAGNOSTIC,
                                 AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1,
                                 "history-repair", "unresolved-tool-result", 0);
      aimee_message_t message = {.role = "tool", .blocks = &block, .n_blocks = 1};
      aimee_request_t request = {.messages = &message, .n_messages = 1};
      aimee_ir_classify_context(&request);
      cJSON *item = block.tool_result ? aimee_ir_message_json(&message) : NULL;
      cJSON_Delete(block.tool_result);
      if (!item || !cJSON_AddItemToArray(messages, item))
      {
         cJSON_Delete(item);
         return -1;
      }
      repaired++;
   }
   return repaired;
}

int aimee_ir_history_repair(cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return -1;
   cJSON *rebuilt = cJSON_CreateArray();
   const char **pending = calloc(4096, sizeof *pending);
   if (!rebuilt || !pending)
      goto fail;
   int count = 0, repairs = 0;
   const cJSON *message;
   cJSON_ArrayForEach(message, messages)
   {
      const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(message, "blocks");
      if (!cJSON_IsArray(blocks) || cJSON_GetArraySize(blocks) > 4096)
         goto fail;
      int has_results = 0;
      const cJSON *block;
      cJSON_ArrayForEach(block, blocks)
      {
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
         has_results |= kind && !strcmp(kind, "tool_result");
      }
      if (!has_results)
      {
         int n = flush_pending(rebuilt, pending, count);
         if (n < 0)
            goto fail;
         repairs += n;
         count = 0;
      }
      cJSON *copy = cJSON_Duplicate(message, 1);
      if (!copy)
         goto fail;
      cJSON *copied_blocks = cJSON_GetObjectItemCaseSensitive(copy, "blocks");
      cJSON *current = copied_blocks->child;
      while (current)
      {
         cJSON *next = current->next;
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "type"));
         const char *id =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "tool_id"));
         if (kind && !strcmp(kind, "tool_result"))
         {
            int matched = 0;
            for (int i = 0; i < count; i++)
               if (id && pending[i] && !strcmp(id, pending[i]))
               {
                  pending[i] = NULL;
                  matched = 1;
                  break;
               }
            if (!matched)
            {
               cJSON_Delete(cJSON_DetachItemViaPointer(copied_blocks, current));
               repairs++;
            }
         }
         current = next;
      }
      /* Keep opaque IDs borrowed from the original, which remains unchanged
       * until every allocation and validation has succeeded. */
      cJSON_ArrayForEach(block, blocks)
      {
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
         if (kind && !strcmp(kind, "tool_use"))
         {
            const char *id =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "tool_id"));
            if (!id || !id[0] || count == 4096)
            {
               cJSON_Delete(copy);
               goto fail;
            }
            pending[count++] = id;
         }
      }
      if (cJSON_GetArraySize(copied_blocks) || !cJSON_GetArraySize(blocks))
         cJSON_AddItemToArray(rebuilt, copy);
      else
         cJSON_Delete(copy);
   }
   int n = flush_pending(rebuilt, pending, count);
   if (n < 0)
      goto fail;
   repairs += n;
   if (repairs)
   {
      while (messages->child)
         cJSON_Delete(cJSON_DetachItemViaPointer(messages, messages->child));
      while (rebuilt->child)
         cJSON_AddItemToArray(messages, cJSON_DetachItemViaPointer(rebuilt, rebuilt->child));
   }
   free(pending);
   cJSON_Delete(rebuilt);
   return repairs;
fail:
   free(pending);
   cJSON_Delete(rebuilt);
   return -1;
}

static const char *request_wire_name(aimee_wire_t wire)
{
   return wire == AIMEE_WIRE_ANTHROPIC     ? "anthropic"
          : wire == AIMEE_WIRE_OPENAI_CHAT ? "openai_chat"
          : wire == AIMEE_WIRE_RESPONSES   ? "responses"
                                           : "unspecified";
}
static const char *strict_name(aimee_strict_mode_t strict)
{
   static const char *const names[] = {"unset", "null", "false", "true"};
   return strict >= AIMEE_STRICT_UNSET && strict <= AIMEE_STRICT_TRUE ? names[strict] : NULL;
}
static cJSON *request_tool_json(const aimee_tool_t *tool, int details)
{
   if (!aimee_ir_tool_valid(tool))
      return NULL;
   cJSON *json = cJSON_CreateObject();
   const char *kind = tool->kind == AIMEE_TOOL_FUNCTION    ? "function"
                      : tool->kind == AIMEE_TOOL_NAMESPACE ? "namespace"
                                                           : "hosted";
   if (!json || !add_string(json, "kind", kind) || !add_string(json, "name", tool->name))
      goto fail;
   if (details && (!add_string(json, "description", tool->description) ||
                   !add_copy(json, "schema", tool->schema) ||
                   !add_string(json, "strict", strict_name(tool->strict)) ||
                   !add_string(json, "cache_control", tool->cache_control) ||
                   !add_string(json, "cache_ttl", tool->cache_ttl) ||
                   (tool->has_defer_loading &&
                    !cJSON_AddBoolToObject(json, "defer_loading", tool->defer_loading))))
      goto fail;
   if (tool->kind == AIMEE_TOOL_HOSTED)
   {
      if (!add_string(json, "provider", request_wire_name(tool->provider_wire)) ||
          !add_string(json, "native_type", tool->native_type) ||
          (details && !add_copy(json, "native_options", tool->native_options)))
         goto fail;
   }
   if (tool->kind == AIMEE_TOOL_NAMESPACE)
   {
      cJSON *children = cJSON_AddArrayToObject(json, "children");
      if (!children)
         goto fail;
      for (int i = 0; i < tool->n_children; i++)
      {
         cJSON *child = request_tool_json(&tool->children[i], details);
         if (!child || !cJSON_AddItemToArray(children, child))
         {
            cJSON_Delete(child);
            goto fail;
         }
      }
   }
   return json;
fail:
   cJSON_Delete(json);
   return NULL;
}

cJSON *aimee_ir_request_json(const aimee_request_t *request)
{
   if (aimee_ir_request_admit(request))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddNumberToObject(out, "version", 1) ||
       !add_string(out, "model", request->model) ||
       !add_string(out, "source_wire", request_wire_name(request->frontend)) ||
       !add_string(out, "cache_policy",
                   request->cache_policy == AIMEE_CACHE_STABLE_PREFIX ? "stable_prefix" : "native"))
      goto fail;
   cJSON *system = cJSON_AddArrayToObject(out, "system"),
         *messages = cJSON_AddArrayToObject(out, "messages"),
         *tools = cJSON_AddArrayToObject(out, "tools");
   if (!system || !messages || !tools)
      goto fail;
   for (int i = 0; i < request->n_system; i++)
   {
      cJSON *block = aimee_ir_block_json(&request->system[i]);
      if (!block || !cJSON_AddItemToArray(system, block))
      {
         cJSON_Delete(block);
         goto fail;
      }
   }
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = aimee_ir_message_json(&request->messages[i]);
      if (!message || !cJSON_AddItemToArray(messages, message))
      {
         cJSON_Delete(message);
         goto fail;
      }
   }
   for (int i = 0; i < request->n_tools; i++)
   {
      cJSON *tool = request_tool_json(&request->tools[i], 1);
      if (!tool || !cJSON_AddItemToArray(tools, tool))
      {
         cJSON_Delete(tool);
         goto fail;
      }
   }
   static const char *const choices[] = {"unset", "auto", "none", "any", "tool"};
   if (request->tool_choice < AIMEE_TOOL_CHOICE_UNSET ||
       request->tool_choice > AIMEE_TOOL_CHOICE_TOOL)
      goto fail;
   cJSON *choice = cJSON_AddObjectToObject(out, "tool_choice");
   if (!choice || !add_string(choice, "mode", choices[request->tool_choice]) ||
       !add_string(choice, "name", request->tool_choice_name))
      goto fail;
   if ((request->has_parallel_tool_calls &&
        !cJSON_AddBoolToObject(out, "parallel_tool_calls", request->parallel_tool_calls)) ||
       (request->has_max_tokens &&
        !cJSON_AddNumberToObject(out, "max_tokens", request->max_tokens)) ||
       (request->has_temperature &&
        !cJSON_AddNumberToObject(out, "temperature", request->temperature)) ||
       (request->has_top_p && !cJSON_AddNumberToObject(out, "top_p", request->top_p)) ||
       (request->has_top_k && !cJSON_AddNumberToObject(out, "top_k", request->top_k)) ||
       !cJSON_AddBoolToObject(out, "stream", request->stream) ||
       !add_copy(out, "metadata", request->metadata) ||
       !add_string(out, "service_tier", request->service_tier) ||
       !add_string(out, "prompt_cache_key", request->prompt_cache_key) ||
       !add_string(out, "prompt_cache_retention", request->prompt_cache_retention) ||
       !add_copy(out, "prompt_cache_options", request->prompt_cache_options))
      goto fail;
   cJSON *stops = cJSON_AddArrayToObject(out, "stop_sequences");
   if (!stops || request->n_stop < 0 || request->n_stop > AIMEE_IR_MAX_BLOCKS ||
       (request->n_stop && !request->stop_sequences))
      goto fail;
   for (int i = 0; i < request->n_stop; i++)
   {
      cJSON *stop =
          request->stop_sequences[i] ? cJSON_CreateString(request->stop_sequences[i]) : NULL;
      if (!stop || !cJSON_AddItemToArray(stops, stop))
      {
         cJSON_Delete(stop);
         goto fail;
      }
   }
   if (request->reasoning_effort != AIMEE_EFFORT_UNSET &&
       !add_string(out, "reasoning_effort", aimee_ir_effort_name(request->reasoning_effort)))
      goto fail;
   if (!add_string(out, "reasoning_summary", request->reasoning_summary))
      goto fail;
   if (request->thinking)
   {
      cJSON *config = cJSON_AddObjectToObject(out, "provider_config");
      if (!config || !add_string(config, "wire", "anthropic") ||
          !add_copy(config, "thinking", request->thinking))
         goto fail;
   }
   const aimee_output_format_t *format = &request->output_format;
   if (format->kind != AIMEE_OUTPUT_UNSET)
   {
      const char *kind = format->kind == AIMEE_OUTPUT_TEXT          ? "text"
                         : format->kind == AIMEE_OUTPUT_JSON_OBJECT ? "json_object"
                         : format->kind == AIMEE_OUTPUT_JSON_SCHEMA ? "json_schema"
                                                                    : NULL;
      cJSON *output = cJSON_AddObjectToObject(out, "output_format");
      if (!kind || !output || !add_string(output, "kind", kind) ||
          !add_string(output, "name", format->name) ||
          !add_string(output, "description", format->description) ||
          !add_copy(output, "schema", format->schema) ||
          !add_string(output, "strict", strict_name(format->strict)))
         goto fail;
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

/* This declared policy projection excludes prompt bodies and opaque schemas.
 * Only the last user text and catalog identities are consumed by these stages. */
cJSON *aimee_ir_policy_view_json(const aimee_request_t *request)
{
   if (aimee_ir_request_admit(request))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddNumberToObject(out, "version", 1) ||
       !add_string(out, "view", "gateway_policy"))
      goto fail;
   cJSON *messages = cJSON_AddArrayToObject(out, "messages"),
         *tools = cJSON_AddArrayToObject(out, "tools");
   if (!messages || !tools)
      goto fail;
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = cJSON_CreateObject();
      if (!message || !add_string(message, "role", request->messages[i].role) ||
          !cJSON_AddItemToArray(messages, message))
      {
         cJSON_Delete(message);
         goto fail;
      }
   }
   for (int i = 0; i < request->n_tools; i++)
   {
      cJSON *tool = request_tool_json(&request->tools[i], 0);
      if (!tool || !cJSON_AddItemToArray(tools, tool))
      {
         cJSON_Delete(tool);
         goto fail;
      }
   }
   char *text = malloc(65536);
   if (!text)
      goto fail;
   size_t length = aimee_ir_last_user_text(request, text, 65536);
   /* Never send a partial final UTF-8 code point over the strict JSON bus. */
   if (length)
   {
      size_t start = length - 1;
      while (start && ((unsigned char)text[start] & 0xc0) == 0x80)
         start--;
      unsigned char first = (unsigned char)text[start];
      size_t needed = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : first >= 0xc0 ? 2 : 1;
      if (length - start < needed)
         text[start] = 0;
   }
   int added = add_string(out, "last_user_text", text);
   free(text);
   if (!added)
      goto fail;
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

cJSON *aimee_ir_delta_json(const aimee_delta_t *delta)
{
   static const char *const types[] = {"TURN_START", "BLOCK_START", "BLOCK_DELTA",
                                       "BLOCK_STOP", "TURN_STOP",   "ERROR"};
   if (!delta || delta->type < AIMEE_DELTA_TURN_START || delta->type > AIMEE_DELTA_ERROR ||
       (delta->type == AIMEE_DELTA_TURN_STOP &&
        (delta->usage_in < 0 || delta->usage_out < 0 || delta->usage_cache_read < 0 ||
         delta->usage_cache_write < 0 || delta->usage_reasoning < 0 ||
         (delta->usage_unobserved != 0 && delta->usage_unobserved != 1) ||
         (delta->usage_unobserved &&
          (delta->usage_in || delta->usage_out || delta->usage_cache_read ||
           delta->usage_cache_write || delta->usage_reasoning)))))
      return NULL;
   if (delta->refusal &&
       (delta->type != AIMEE_DELTA_TURN_STOP || delta->stop_reason != AIMEE_STOP_CONTENT_FILTER ||
        !aimee_refusal_details_valid(delta->refusal)))
      return NULL;
   if (delta->message_group &&
       (delta->message_group < 1 || delta->message_group > AIMEE_IR_MAX_BLOCKS ||
        delta->type != AIMEE_DELTA_BLOCK_START || delta->kind != AIMEE_BLK_TEXT))
      return NULL;
   if ((delta->thinking_id || delta->thinking_encrypted_content) &&
       (delta->kind != AIMEE_BLK_THINKING ||
        (delta->type != AIMEE_DELTA_BLOCK_START && delta->type != AIMEE_DELTA_BLOCK_DELTA)))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   int ok = add_string(out, "type", types[delta->type]);
   if (delta->message_group)
      ok = ok && cJSON_AddNumberToObject(out, "message_group", delta->message_group) != NULL;
   if (delta->type >= AIMEE_DELTA_BLOCK_START && delta->type <= AIMEE_DELTA_BLOCK_STOP)
   {
      ok = ok && delta->block_id >= 0 && delta->block_id <= 4095 &&
           (delta->kind == AIMEE_BLK_TEXT || delta->kind == AIMEE_BLK_THINKING ||
            delta->kind == AIMEE_BLK_TOOL_USE) &&
           block_kind(delta->kind) &&
           cJSON_AddNumberToObject(out, "block_id", delta->block_id) != NULL &&
           add_string(out, "kind", block_kind(delta->kind));
      if (delta->type == AIMEE_DELTA_BLOCK_START && delta->kind == AIMEE_BLK_TOOL_USE &&
          (!delta->tool_id || !delta->tool_id[0] || !delta->tool_name || !delta->tool_name[0]))
         ok = 0;
      if (delta->type == AIMEE_DELTA_BLOCK_DELTA &&
          ((delta->text_delta && delta->kind != AIMEE_BLK_TEXT &&
            delta->kind != AIMEE_BLK_THINKING) ||
           (delta->tool_args_delta && delta->kind != AIMEE_BLK_TOOL_USE)))
         ok = 0;
      if (delta->type == AIMEE_DELTA_BLOCK_START)
      {
         ok = ok && add_string(out, "tool_id", delta->tool_id) &&
              add_string(out, "name", delta->tool_name) &&
              add_string(out, "namespace", delta->tool_namespace);
         if (delta->tool_input)
            ok = ok && delta->kind == AIMEE_BLK_TOOL_USE && cJSON_IsObject(delta->tool_input) &&
                 add_copy(out, "input", delta->tool_input);
         if (delta->redacted_content)
            ok = ok && delta->kind == AIMEE_BLK_THINKING &&
                 add_string(out, "redacted_content", delta->redacted_content);
      }
      if (delta->type == AIMEE_DELTA_BLOCK_START || delta->type == AIMEE_DELTA_BLOCK_DELTA)
      {
         ok = ok && add_string(out, "thinking_id", delta->thinking_id) &&
              add_string(out, "encrypted_content", delta->thinking_encrypted_content);
         if (delta->signature_delta)
            ok = ok && delta->kind == AIMEE_BLK_THINKING &&
                 add_string(out, "signature_delta", delta->signature_delta);
         if (delta->type == AIMEE_DELTA_BLOCK_START && delta->text_delta)
            ok = ok && (delta->kind == AIMEE_BLK_TEXT || delta->kind == AIMEE_BLK_THINKING) &&
                 add_string(out, "text_delta", delta->text_delta);
      }
      if (delta->type == AIMEE_DELTA_BLOCK_DELTA)
         ok = ok && add_string(out, "text_delta", delta->text_delta) &&
              add_string(out, "tool_args_delta", delta->tool_args_delta);
   }
   if (delta->type == AIMEE_DELTA_TURN_STOP)
   {
      ok = ok && add_string(out, "stop_reason", aimee_stop_reason_name(delta->stop_reason));
      if (!delta->usage_unobserved)
         ok = ok && cJSON_AddNumberToObject(out, "usage_in", delta->usage_in) != NULL &&
              cJSON_AddNumberToObject(out, "usage_out", delta->usage_out) != NULL;
   }
   if (delta->type == AIMEE_DELTA_TURN_STOP)
   {
      ok = ok && add_string(out, "raw_stop_reason", delta->raw_stop_reason);
      if (delta->refusal)
      {
         cJSON *details = aimee_refusal_details_json(delta->refusal);
         if (!details || !cJSON_AddItemToObject(out, "refusal", details))
         {
            cJSON_Delete(details);
            ok = 0;
         }
      }
      if (delta->usage_cache_read)
         ok = ok &&
              cJSON_AddNumberToObject(out, "usage_cache_read", delta->usage_cache_read) != NULL;
      if (delta->usage_cache_write)
         ok = ok &&
              cJSON_AddNumberToObject(out, "usage_cache_write", delta->usage_cache_write) != NULL;
      if (delta->usage_reasoning)
         ok = ok && cJSON_AddNumberToObject(out, "usage_reasoning", delta->usage_reasoning) != NULL;
   }
   if (delta->type == AIMEE_DELTA_ERROR)
      ok = ok && add_string(out, "error_message", delta->error_message);
   if (!ok)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

/* Exact member sets prevent this metadata carrier becoming an arbitrary JSON
 * escape. Cross-field custody and outcome qualification remain owner policy. */
static int verification_keys(const cJSON *object, const char *const *keys, size_t count)
{
   if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != (int)count)
      return 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, object)
   {
      size_t index = 0;
      if (!item->string)
         return 0;
      for (; index < count && strcmp(item->string, keys[index]); ++index)
      {
      }
      if (index == count)
         return 0;
      for (const cJSON *previous = object->child; previous != item; previous = previous->next)
         if (!strcmp(previous->string, item->string))
            return 0;
   }
   return 1;
}
static const char *verification_text(const cJSON *object, const char *key, size_t limit)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value && value[0] && strlen(value) <= limit ? value : NULL;
}
static int verification_hex(const cJSON *object, const char *key, size_t first, size_t second)
{
   const char *value = verification_text(object, key, second > first ? second : first);
   if (!value)
      return 0;
   size_t length = strlen(value);
   return (length == first || length == second) && strspn(value, "0123456789abcdef") == length;
}
static int child_learning_binding_valid(const cJSON *binding)
{
   static const char *const keys[] = {"version", "job_id",   "scope_kind", "scope_id",
                                      "agent",   "provider", "model"};
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(binding, "version");
   const char *job = verification_text(binding, "job_id", 10);
   const char *scope = verification_text(binding, "scope_kind", 32);
   if (!verification_keys(binding, keys, sizeof keys / sizeof *keys) || !cJSON_IsNumber(version) ||
       version->valuedouble != 1 || !job || job[0] == '0' ||
       strspn(job, "0123456789") != strlen(job) || strtoull(job, NULL, 10) > 2147483647ULL ||
       !scope || strcmp(scope, "project") || !verification_text(binding, "scope_id", 256) ||
       !verification_text(binding, "agent", 63) || !verification_text(binding, "provider", 127) ||
       !verification_text(binding, "model", 255))
      return 0;
   return 1;
}

int aimee_ir_verification_proof_valid(const cJSON *proof)
{
   static const char *const keys[] = {"version",
                                      "session_id",
                                      "turn_id",
                                      "actor",
                                      "scope_kind",
                                      "scope_id",
                                      "isolation",
                                      "snapshot_id",
                                      "snapshot_commit",
                                      "final_commit",
                                      "snapshot_tree",
                                      "final_tree",
                                      "configuration_sha256",
                                      "final_configuration_sha256",
                                      "profile_sha256",
                                      "completed",
                                      "observed_at",
                                      "steps"};
   static const char *const step_keys[] = {"name",    "command_sha256", "ran",
                                           "skipped", "termination",    "exit_code"};
   if (!verification_keys(proof, keys, sizeof keys / sizeof *keys))
      return 0;
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(proof, "version");
   const char *scope = verification_text(proof, "scope_kind", 32);
   const char *isolation = verification_text(proof, "isolation", 64);
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(proof, "steps");
   if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
       !verification_text(proof, "session_id", 256) || !verification_text(proof, "turn_id", 256) ||
       !verification_text(proof, "actor", 256) || !scope || strcmp(scope, "project") ||
       !verification_text(proof, "scope_id", 256) || !isolation ||
       strcmp(isolation, "private_source_snapshot") ||
       !verification_hex(proof, "snapshot_id", 32, 32) ||
       !verification_hex(proof, "snapshot_commit", 40, 64) ||
       !verification_hex(proof, "final_commit", 40, 64) ||
       !verification_hex(proof, "snapshot_tree", 40, 64) ||
       !verification_hex(proof, "final_tree", 40, 64) ||
       !verification_hex(proof, "configuration_sha256", 64, 64) ||
       !verification_hex(proof, "final_configuration_sha256", 64, 64) ||
       !verification_hex(proof, "profile_sha256", 64, 64) ||
       !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(proof, "completed")) ||
       !verification_text(proof, "observed_at", 64) || !cJSON_IsArray(steps) ||
       cJSON_GetArraySize(steps) < 1 || cJSON_GetArraySize(steps) > 64)
      return 0;
   const cJSON *step;
   cJSON_ArrayForEach(step, steps)
   {
      if (!verification_keys(step, step_keys, sizeof step_keys / sizeof *step_keys))
         return 0;
      const char *name = verification_text(step, "name", 128);
      const char *termination = verification_text(step, "termination", 16);
      const char *code = verification_text(step, "exit_code", 3);
      if (!name || !verification_hex(step, "command_sha256", 64, 64) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(step, "ran")) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(step, "skipped")) || !termination ||
          strcmp(termination, "exited") || !code || strspn(code, "0123456789") != strlen(code) ||
          (code[0] == '0' && code[1]) || atoi(code) > 255)
         return 0;
      for (const cJSON *previous = steps->child; previous != step; previous = previous->next)
         if (!strcmp(verification_text(previous, "name", 128), name))
            return 0;
   }
   char *encoded = cJSON_PrintUnformatted(proof);
   int bounded = encoded && strlen(encoded) <= 65536;
   free(encoded);
   return bounded;
}

static int native_owner_valid(const cJSON *owner)
{
   if (!cJSON_IsObject(owner) || cJSON_GetArraySize(owner) != 6)
      return 0;
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(owner, "version");
   const char *h = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(owner, "harness"));
   const char *keys[] = {"owner_id", "binding_sha256", "containment_sha256", "epoch"};
   const int sizes[] = {32, 64, 64, 32};
   if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !h ||
       (strcmp(h, "codex") && strcmp(h, "claude")))
      return 0;
   for (int i = 0; i < 4; i++)
   {
      const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(owner, keys[i]));
      if (!value)
         return 0;
      if (i == 3 && !strcmp(value, "0"))
         continue;
      if (strlen(value) != (size_t)sizes[i] ||
          strspn(value, "0123456789abcdef") != (size_t)sizes[i])
         return 0;
   }
   return 1;
}

cJSON *aimee_ir_session_event_json(const aimee_session_event_t *event)
{
   if (!event ||
       (event->native_owner &&
        (event->kind != AIMEE_SESSION_MESSAGE || !native_owner_valid(event->native_owner))) ||
       (event->message_partial &&
        (!event->native_owner || !event->message || !event->message->role ||
         strcmp(event->message->role, "assistant") || event->message->n_blocks < 1)))
      return NULL;
   cJSON *out = cJSON_CreateObject(), *payload = NULL;
   const char *kind = NULL;
   switch (event->kind)
   {
   case AIMEE_SESSION_DELTA:
      if (!event->role || !event->role[0] || !event->message_id || !event->message_id[0])
         goto fail;
      kind = "delta";
      payload = aimee_ir_delta_json(event->delta);
      break;
   case AIMEE_SESSION_MESSAGE:
      if (!event->message_id || !event->message_id[0])
         goto fail;
      kind = "message";
      payload = aimee_ir_message_json(event->message);
      break;
   case AIMEE_SESSION_CONTROL_REQUEST:
   case AIMEE_SESSION_CONTROL_RESPONSE:
      if (!event->request_id || !event->request_id[0] || !event->control_kind ||
          !event->control_kind[0] || !cJSON_IsObject(event->control))
         goto fail;
      kind = event->kind == AIMEE_SESSION_CONTROL_REQUEST ? "control_request" : "control_response";
      payload = cJSON_Duplicate(event->control, 1);
      break;
   case AIMEE_SESSION_NATIVE_COMPACTION:
      if (!event->native_harness ||
          (strcmp(event->native_harness, "codex") && strcmp(event->native_harness, "claude")))
         goto fail;
      kind = "native_compaction";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "harness", event->native_harness))
         goto fail;
      break;
   case AIMEE_SESSION_NATIVE_CHECKPOINT:
   {
      const aimee_native_checkpoint_t *checkpoint = event->native_checkpoint;
      if (!checkpoint || !checkpoint->scope || strlen(checkpoint->scope) != 64 ||
          strspn(checkpoint->scope, "0123456789abcdef") != 64 || !checkpoint->context_digest ||
          strlen(checkpoint->context_digest) != 64 ||
          strspn(checkpoint->context_digest, "0123456789abcdef") != 64 ||
          !checkpoint->prefix_digest || strlen(checkpoint->prefix_digest) != 64 ||
          strspn(checkpoint->prefix_digest, "0123456789abcdef") != 64 || !checkpoint->claim ||
          strlen(checkpoint->claim) != 32 || strspn(checkpoint->claim, "0123456789abcdef") != 32 ||
          !checkpoint->harness ||
          (strcmp(checkpoint->harness, "codex") && strcmp(checkpoint->harness, "claude")) ||
          !checkpoint->native_id || !checkpoint->native_id[0] ||
          strlen(checkpoint->native_id) > 1024 || checkpoint->message_count < 1 ||
          checkpoint->message_count > AIMEE_IR_MAX_BLOCKS)
         goto fail;
      kind = "native_checkpoint";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "scope", checkpoint->scope) ||
          !add_string(payload, "context_digest", checkpoint->context_digest) ||
          !add_string(payload, "prefix_digest", checkpoint->prefix_digest) ||
          !add_string(payload, "harness", checkpoint->harness) ||
          !add_string(payload, "native_id", checkpoint->native_id) ||
          !add_string(payload, "claim", checkpoint->claim) ||
          !cJSON_AddNumberToObject(payload, "message_count", checkpoint->message_count))
         goto fail;
      break;
   }
   case AIMEE_SESSION_REPLAY_PROOF:
      if (!event->replay_scope || strlen(event->replay_scope) != 64 ||
          strspn(event->replay_scope, "0123456789abcdef") != 64 || !event->replay_digest ||
          strlen(event->replay_digest) != 64 ||
          strspn(event->replay_digest, "0123456789abcdef") != 64 || !event->replay_wire ||
          (strcmp(event->replay_wire, "anthropic") && strcmp(event->replay_wire, "responses")) ||
          (!strcmp(event->replay_wire, "anthropic") &&
           (!event->replay_binding || !aimee_ir_thinking_binding_valid(event->replay_binding))) ||
          (!strcmp(event->replay_wire, "responses") && event->replay_binding))
         goto fail;
      kind = "replay_proof";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "scope", event->replay_scope) ||
          !add_string(payload, "block_digest", event->replay_digest) ||
          !add_string(payload, "wire", event->replay_wire) ||
          !add_string(payload, "thinking_binding", event->replay_binding))
         goto fail;
      break;
   case AIMEE_SESSION_PROJECTION:
      if (!event->projection_model || !event->projection_model[0] ||
          event->projection_reasoning < 0 || event->projection_cache_controls < 0 ||
          event->projection_request_controls < 0 ||
          event->projection_request_controls > AIMEE_IR_MAX_BLOCKS ||
          event->projection_history_messages_as_text < 0 ||
          event->projection_history_messages_as_text > AIMEE_IR_MAX_BLOCKS ||
          event->projection_host_context_blocks_as_text < 0 ||
          event->projection_host_context_blocks_as_text > AIMEE_IR_MAX_BLOCKS ||
          event->projection_unverified_external_tool_reports < 0 ||
          event->projection_unverified_external_tool_reports > AIMEE_IR_MAX_BLOCKS ||
          (!event->projection_reasoning && !event->projection_cache_controls &&
           !event->projection_request_controls &&
           !event->projection_history_messages_as_text &&
           !event->projection_host_context_blocks_as_text &&
           !event->projection_unverified_external_tool_reports) ||
          event->projection_opaque < 0 || event->projection_opaque > event->projection_reasoning ||
          !event->projection_boundary ||
          (strcmp(event->projection_boundary, "request") &&
           strcmp(event->projection_boundary, "response")))
         goto fail;
      kind = "projection";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "model", event->projection_model) ||
          !add_string(payload, "boundary", event->projection_boundary) ||
          !cJSON_AddNumberToObject(payload, "omitted_reasoning_blocks",
                                   event->projection_reasoning) ||
          !cJSON_AddNumberToObject(payload, "omitted_opaque_state_blocks",
                                   event->projection_opaque) ||
          (event->projection_cache_controls &&
           !cJSON_AddNumberToObject(payload, "omitted_cache_controls",
                                    event->projection_cache_controls)) ||
          (event->projection_request_controls &&
           !cJSON_AddNumberToObject(payload, "omitted_request_controls",
                                    event->projection_request_controls)) ||
          (event->projection_host_context_blocks_as_text &&
           !cJSON_AddNumberToObject(payload, "host_context_blocks_as_text",
                                    event->projection_host_context_blocks_as_text)) ||
          (event->projection_history_messages_as_text &&
           !cJSON_AddNumberToObject(payload, "history_messages_as_text",
                                    event->projection_history_messages_as_text)) ||
          (event->projection_unverified_external_tool_reports &&
           !cJSON_AddNumberToObject(payload, "unverified_external_tool_reports",
                                    event->projection_unverified_external_tool_reports)))
         goto fail;
      break;
   case AIMEE_SESSION_CHILD_EXECUTION:
      if (!event->child_session || !event->child_session[0] || strlen(event->child_session) > 127 ||
          !event->child_turn || !event->child_turn[0] || strlen(event->child_turn) > 127 ||
          !event->child_role || !event->child_role[0] || strlen(event->child_role) > 127 ||
          !event->execution_status ||
          (strcmp(event->execution_status, "running") &&
           strcmp(event->execution_status, "completed") &&
           strcmp(event->execution_status, "failed") &&
           strcmp(event->execution_status, "cancelled")))
         goto fail;
      kind = "child_execution";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "session_id", event->child_session) ||
          !add_string(payload, "turn_id", event->child_turn) ||
          !add_string(payload, "role", event->child_role) ||
          !add_string(payload, "status", event->execution_status) ||
          (event->child_learning_binding &&
           (!child_learning_binding_valid(event->child_learning_binding) ||
            !add_copy(payload, "learning_binding", event->child_learning_binding))))
         goto fail;
      break;
   case AIMEE_SESSION_EXECUTION:
      if (!event->execution_status || (strcmp(event->execution_status, "running") &&
                                       strcmp(event->execution_status, "completed") &&
                                       strcmp(event->execution_status, "failed") &&
                                       strcmp(event->execution_status, "cancelled")))
         goto fail;
      kind = "execution";
      payload = cJSON_CreateObject();
      if (!payload || !add_string(payload, "status", event->execution_status) ||
          !add_string(payload, "error", event->execution_error))
         goto fail;
      if (event->verification_proofs)
      {
         if (strcmp(event->execution_status, "completed") ||
             !cJSON_IsArray(event->verification_proofs) ||
             cJSON_GetArraySize(event->verification_proofs) < 1 ||
             cJSON_GetArraySize(event->verification_proofs) > 8)
            goto fail;
         const cJSON *proof;
         cJSON_ArrayForEach(proof, event->verification_proofs)
         {
            if (!aimee_ir_verification_proof_valid(proof))
               goto fail;
         }
         if (!add_copy(payload, "verification_proofs", event->verification_proofs))
            goto fail;
      }
      break;
   default:
      goto fail;
   }
   if (!out || !payload || !add_string(out, "kind", kind) ||
       !add_string(out, "message_id", event->message_id) || !add_string(out, "role", event->role) ||
       !add_string(out, "request_id", event->request_id) ||
       !add_string(out, "control_kind", event->control_kind))
      goto fail;
   if (event->native_owner)
   {
      cJSON *owner = cJSON_Duplicate(event->native_owner, 1);
      if (!owner || !cJSON_AddItemToObject(out, "native_owner", owner))
      {
         cJSON_Delete(owner);
         goto fail;
      }
   }
   if (event->message_partial && !cJSON_AddTrueToObject(out, "partial"))
      goto fail;
   if (!cJSON_AddItemToObject(out, "payload", payload))
      goto fail;
   return out;
fail:
   cJSON_Delete(payload);
   cJSON_Delete(out);
   return NULL;
}

void aimee_ir_session_set_sink(aimee_ir_session_sink_fn sink, void *context)
{
   session_sink = sink;
   session_context = sink ? context : NULL;
   if (!sink)
      session_capture_failed = 0;
}

void aimee_ir_session_begin_task(void)
{
   if (!session_sink)
      session_capture_failed = 0;
}

void aimee_ir_session_restore_sink(aimee_ir_session_sink_fn sink, void *context, int failed)
{
   session_sink = sink;
   session_context = sink ? context : NULL;
   session_capture_failed = session_capture_failed || failed;
}

aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context)
{
   if (context)
      *context = session_context;
   return session_sink;
}

int aimee_ir_session_emit(const aimee_session_event_t *event)
{
   int rc = session_sink ? session_sink(event, session_context) : 0;
   if (rc)
      session_capture_failed = 1;
   return rc;
}

void aimee_ir_session_fail_capture(void)
{
   if (session_sink)
      session_capture_failed = 1;
}

int aimee_ir_session_capture_failed(void)
{
   return session_capture_failed;
}

int aimee_ir_session_emit_response(const aimee_response_t *response, const char *message_id)
{
   if (!response)
      return -1;
   static _Thread_local unsigned long long counter;
   char generated[64];
   if (!message_id)
   {
      snprintf(generated, sizeof(generated), "response-%llu", ++counter);
      message_id = generated;
   }
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message = &message, .message_id = message_id};
   return aimee_ir_session_emit(&event);
}

int aimee_ir_session_emit_buffered_response(const aimee_response_t *response,
                                            const char *message_id)
{
   if (!response)
      return -1;
   static _Thread_local unsigned long long counter;
   char generated[64];
   if (!message_id)
   {
      snprintf(generated, sizeof generated, "buffered-%llu", ++counter);
      message_id = generated;
   }
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                  .role = response->role ? response->role : "assistant",
                                  .message_id = message_id,
                                  .delta = &delta};
   int rc = aimee_ir_session_emit(&event);
   if (rc)
      return rc;
   if (response->error_message)
   {
      delta = (aimee_delta_t){.type = AIMEE_DELTA_ERROR, .error_message = response->error_message};
      rc = aimee_ir_session_emit(&event);
   }
   else
      rc = aimee_ir_session_emit_response(response, message_id);
   if (rc)
      return rc;
   delta = (aimee_delta_t){.type = AIMEE_DELTA_TURN_STOP,
                           .stop_reason = response->stop_reason,
                           .raw_stop_reason = response->raw_stop_reason,
                           .refusal = response->refusal,
                           .usage_in = response->usage_in,
                           .usage_out = response->usage_out,
                           .usage_cache_read = response->usage_cache_read,
                           .usage_cache_write = response->usage_cache_write,
                           .usage_reasoning = response->usage_reasoning};
   delta.usage_unobserved = response->usage_unobserved;
   return aimee_ir_session_emit(&event);
}

int aimee_ir_session_emit_tool_result(const char *tool_id, const char *result, int is_error)
{
   static _Thread_local unsigned long long counter;
   char id[64];
   snprintf(id, sizeof(id), "tool-result-%llu", ++counter);
   cJSON *content = cJSON_CreateString(result ? result : "");
   if (!content)
      return -1;
   aimee_block_t block = {.type = AIMEE_BLK_TOOL_RESULT,
                          .tool_id = (char *)tool_id,
                          .tool_result = content,
                          .tool_is_error = is_error};
   aimee_message_t message = {.role = "tool", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &message};
   int rc = aimee_ir_session_emit(&event);
   cJSON_Delete(content);
   return rc;
}

/* MCP form controls use the restricted flat JSON Schema contract. The browser
 * is a renderer; native execution independently validates the submitted values. */
static int form_enum(const cJSON *schema, const cJSON *value)
{
   const cJSON *list = cJSON_GetObjectItemCaseSensitive(schema, "enum");
   int titled = 0;
   if (!list)
   {
      list = cJSON_GetObjectItemCaseSensitive(schema, "oneOf");
      if (!list)
         list = cJSON_GetObjectItemCaseSensitive(schema, "anyOf");
      titled = 1;
   }
   if (!list)
      return 1;
   if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list))
      return 0;
   const cJSON *option;
   cJSON_ArrayForEach(
       option,
       list) if (cJSON_Compare(titled ? cJSON_GetObjectItemCaseSensitive(option, "const") : option,
                               value, 1)) return 1;
   return 0;
}
static int form_bound(const cJSON *schema, const char *key, double value, int minimum)
{
   const cJSON *bound = cJSON_GetObjectItemCaseSensitive(schema, key);
   return !bound || (cJSON_IsNumber(bound) && isfinite(bound->valuedouble) &&
                     (minimum ? value >= bound->valuedouble : value <= bound->valuedouble));
}
static int form_value(const cJSON *schema, const cJSON *value)
{
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(schema, "type"));
   if (!cJSON_IsObject(schema))
      return 0;
   /* Titled enum array items omit type and use const alternatives instead. */
   if (!type)
      return cJSON_IsString(value) &&
             (cJSON_GetObjectItemCaseSensitive(schema, "oneOf") ||
              cJSON_GetObjectItemCaseSensitive(schema, "anyOf")) &&
             form_enum(schema, value);
   if (!strcmp(type, "boolean"))
      return cJSON_IsBool(value);
   if (!strcmp(type, "number") || !strcmp(type, "integer"))
   {
      if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
          fabs(value->valuedouble) > 9007199254740991.0)
         return 0;
      if (!strcmp(type, "integer") && (double)(long long)value->valuedouble != value->valuedouble)
         return 0;
      return form_bound(schema, "minimum", value->valuedouble, 1) &&
             form_bound(schema, "maximum", value->valuedouble, 0);
   }
   if (!strcmp(type, "string"))
   {
      if (!cJSON_IsString(value))
         return 0;
      size_t length = 0;
      for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; p++)
         if ((*p & 0xc0) != 0x80)
            length++;
      return form_bound(schema, "minLength", (double)length, 1) &&
             form_bound(schema, "maxLength", (double)length, 0) && form_enum(schema, value);
   }
   if (!strcmp(type, "array"))
   {
      if (!cJSON_IsArray(value) || !form_bound(schema, "minItems", cJSON_GetArraySize(value), 1) ||
          !form_bound(schema, "maxItems", cJSON_GetArraySize(value), 0))
         return 0;
      const cJSON *items = cJSON_GetObjectItemCaseSensitive(schema, "items"), *item;
      cJSON_ArrayForEach(item, value)
      {
         if (!cJSON_IsString(item) || !form_value(items, item))
            return 0;
         /* Multi-select content is a set, not repeated selections. */
         for (const cJSON *old = value->child; old != item; old = old->next)
            if (cJSON_Compare(old, item, 1))
               return 0;
      }
      return cJSON_IsObject(items);
   }
   return 0;
}
int aimee_ir_form_validate(const cJSON *schema, const cJSON *content)
{
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(schema, "type"));
   const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
   if (!type || strcmp(type, "object") || !cJSON_IsObject(properties) || !cJSON_IsObject(content) ||
       (required && !cJSON_IsArray(required)))
      return 0;
   const cJSON *field;
   cJSON_ArrayForEach(
       field,
       required) if (!cJSON_IsString(field) ||
                     !cJSON_GetObjectItemCaseSensitive(properties, field->valuestring) ||
                     !cJSON_GetObjectItemCaseSensitive(content, field->valuestring)) return 0;
   cJSON_ArrayForEach(field, content)
   {
      if (!field->string ||
          !form_value(cJSON_GetObjectItemCaseSensitive(properties, field->string), field))
         return 0;
      for (const cJSON *old = content->child; old != field; old = old->next)
         if (!strcmp(old->string, field->string))
            return 0;
   }
   return 1;
}
int aimee_ir_elicitation_url_valid(const cJSON *url)
{
   if (!cJSON_IsObject(url) || cJSON_GetArraySize(url) != 4)
      return 0;
   const char *href = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(url, "href"));
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(url, "elicitation_id"));
   const char *server = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(url, "server"));
   const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(url, "message"));
   if (!href || !id || !*id || !server || !*server || !message || strlen(href) > 8192 ||
       strlen(id) > 1024 || strlen(server) > 256 || strlen(message) > 65535)
      return 0;
   size_t scheme_length = strcspn(href, ":");
   char scheme[6] = {0};
   if (scheme_length != 4 && scheme_length != 5)
      return 0;
   for (size_t i = 0; i < scheme_length; i++)
      scheme[i] = href[i] >= 'A' && href[i] <= 'Z' ? href[i] + ('a' - 'A') : href[i];
   if ((strcmp(scheme, "https") && strcmp(scheme, "http")) ||
       strncmp(href + scheme_length, "://", 3))
      return 0;
   const char *host = href + scheme_length + 3;
   if (!host || !*host || *host == ':' || *host == '/' || *host == '?' || *host == '#')
      return 0;
   const char *end = host + strcspn(host, "/?#");
   if (memchr(host, '@', (size_t)(end - host)))
      return 0;
   for (const unsigned char *p = (const unsigned char *)href; *p; p++)
      if (*p <= 0x20 || *p == 0x7f || *p == '\\')
         return 0;
   return 1;
}

int aimee_ir_control_reply_valid(const char *kind, const cJSON *request, const cJSON *reply)
{
   const char *decision = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "decision"));
   if (!decision)
      return 0;
   const cJSON *answers = cJSON_GetObjectItemCaseSensitive(reply, "answers");
   const cJSON *content = cJSON_GetObjectItemCaseSensitive(reply, "content");
   if (strcmp(decision, "allow"))
      return (!strcmp(decision, "deny") || !strcmp(decision, "cancel")) && !answers && !content;
   if (!strcmp(kind, "elicitation"))
   {
      const cJSON *form = cJSON_GetObjectItemCaseSensitive(request, "form");
      const cJSON *url = cJSON_GetObjectItemCaseSensitive(request, "url");
      if (url)
         return !form && !answers && !content && aimee_ir_elicitation_url_valid(url);
      return !answers &&
             aimee_ir_form_validate(cJSON_GetObjectItemCaseSensitive(form, "schema"), content);
   }
   if (!strcmp(kind, "question"))
   {
      const cJSON *questions = cJSON_GetObjectItemCaseSensitive(request, "questions"), *q;
      if (content || !cJSON_IsArray(questions) || !cJSON_GetArraySize(questions) ||
          !cJSON_IsObject(answers) || cJSON_GetArraySize(answers) != cJSON_GetArraySize(questions))
         return 0;
      cJSON_ArrayForEach(q, questions)
      {
         const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q, "id"));
         const cJSON *values = id ? cJSON_GetObjectItemCaseSensitive(answers, id) : NULL, *v;
         if (!cJSON_IsArray(values) || !cJSON_GetArraySize(values))
            return 0;
         cJSON_ArrayForEach(v, values) if (!cJSON_IsString(v) || !v->valuestring[0]) return 0;
      }
      return 1;
   }
   return !strcmp(kind, "permission") && !answers && !content;
}

int aimee_ir_session_emit_projection(const char *model, int reasoning, int opaque,
                                     const char *boundary)
{
   return aimee_ir_session_emit_projection_controls(model, reasoning, opaque, 0, boundary);
}
int aimee_ir_session_emit_projection_controls(const char *model, int reasoning, int opaque,
                                              int cache_controls, const char *boundary)
{
   if (!reasoning && !opaque && !cache_controls)
      return 0;
   aimee_session_event_t event = {.kind = AIMEE_SESSION_PROJECTION,
                                  .projection_model = model && model[0] ? model : "selected model",
                                  .projection_boundary = boundary,
                                  .projection_reasoning = reasoning,
                                  .projection_opaque = opaque,
                                  .projection_cache_controls = cache_controls};
   return aimee_ir_session_emit(&event);
}
