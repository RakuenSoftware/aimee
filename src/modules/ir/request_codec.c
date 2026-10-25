/* Owned canonical request snapshots. Native wire parsing stays in adapters. */
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int fields(const cJSON *json, const char *const *names, size_t count)
{
   if (!cJSON_IsObject(json))
      return -1;
   const cJSON *field;
   cJSON_ArrayForEach(field, json)
   {
      size_t i;
      for (i = 0; i < count; i++)
         if (!strcmp(field->string, names[i]))
            break;
      if (i == count)
         return -1;
   }
   return 0;
}
#define FIELDS(j, ...)                                                                             \
   fields(j, (const char *const[]){__VA_ARGS__},                                                   \
          sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))
static const cJSON *get(const cJSON *j, const char *key)
{
   return cJSON_GetObjectItemCaseSensitive(j, key);
}
static int string(const cJSON *j, const char *key, char **out)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   if (!cJSON_IsString(v) || !v->valuestring)
      return -1;
   *out = strdup(v->valuestring);
   return *out ? 0 : -1;
}
static int copy(const cJSON *j, const char *key, cJSON **out)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   *out = cJSON_Duplicate(v, 1);
   return *out ? 0 : -1;
}
static int enumeration(const cJSON *j, const char *key, const char *const *names, int count,
                       int *out, int required)
{
   const cJSON *v = get(j, key);
   if (!v)
      return required ? -1 : 0;
   if (!cJSON_IsString(v))
      return -1;
   for (int i = 0; i < count; i++)
      if (!strcmp(v->valuestring, names[i]))
      {
         *out = i;
         return 0;
      }
   return -1;
}
static int wire(const cJSON *j, const char *key, aimee_wire_t *out, int required)
{
   static const char *const names[] = {"unspecified", "anthropic", "openai_chat", "responses"};
   int value = 0;
   if (enumeration(j, key, names, 4, &value, required))
      return -1;
   *out = (aimee_wire_t)value;
   return 0;
}
static int strict(const cJSON *j, aimee_strict_mode_t *out, int required)
{
   static const char *const names[] = {"unset", "null", "false", "true"};
   int value = 0;
   if (enumeration(j, "strict", names, 4, &value, required))
      return -1;
   *out = (aimee_strict_mode_t)value;
   return 0;
}
static int boolean(const cJSON *j, const char *key, int *value, int *present)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   if (!cJSON_IsBool(v))
      return -1;
   *value = cJSON_IsTrue(v);
   *present = 1;
   return 0;
}
static int integer(const cJSON *j, const char *key, int *value, int *present)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < INT_MIN ||
       v->valuedouble > INT_MAX || v->valuedouble != v->valueint)
      return -1;
   *value = v->valueint;
   *present = 1;
   return 0;
}
static int number(const cJSON *j, const char *key, double *value, int *present)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble))
      return -1;
   *value = v->valuedouble;
   *present = 1;
   return 0;
}
static int tool(const cJSON *json, aimee_tool_t *out, int depth)
{
   if (depth > 1 || FIELDS(json, "kind", "name", "description", "schema", "strict", "defer_loading",
                           "cache_control", "cache_ttl", "provider", "native_type",
                           "native_options", "children"))
      return -1;
   static const char *const kinds[] = {"function", "namespace", "hosted"};
   int kind = 0;
   if (enumeration(json, "kind", kinds, 3, &kind, 1))
      return -1;
   out->kind = (aimee_tool_kind_t)kind;
   if (string(json, "name", &out->name) || string(json, "description", &out->description) ||
       copy(json, "schema", &out->schema) || strict(json, &out->strict, 0) ||
       string(json, "cache_control", &out->cache_control) ||
       string(json, "cache_ttl", &out->cache_ttl) ||
       boolean(json, "defer_loading", &out->defer_loading, &out->has_defer_loading))
      return -1;
   if (out->kind == AIMEE_TOOL_HOSTED)
   {
      if (wire(json, "provider", &out->provider_wire, 1) ||
          string(json, "native_type", &out->native_type) ||
          copy(json, "native_options", &out->native_options) || get(json, "children"))
         return -1;
   }
   else if (get(json, "provider") || get(json, "native_type") || get(json, "native_options"))
      return -1;
   const cJSON *children = get(json, "children");
   if (out->kind == AIMEE_TOOL_NAMESPACE)
   {
      if (children &&
          (!cJSON_IsArray(children) || cJSON_GetArraySize(children) > AIMEE_IR_MAX_BLOCKS))
         return -1;
      int count = children ? cJSON_GetArraySize(children) : 0;
      if (count && !(out->children = calloc((size_t)count, sizeof *out->children)))
         return -1;
      out->n_children = count;
      for (int i = 0; i < out->n_children; i++)
         if (tool(cJSON_GetArrayItem(children, i), &out->children[i], depth + 1))
            return -1;
   }
   else if (children)
      return -1;
   return aimee_ir_tool_valid(out) ? 0 : -1;
}
/* Admission rejects undeclared fields instead of silently discarding them. */
static int blocks_shape(const cJSON *blocks)
{
   if (!cJSON_IsArray(blocks))
      return -1;
   const cJSON *b;
   cJSON_ArrayForEach(b, blocks)
   {
      if (FIELDS(b, "type", "text", "signature", "thinking_binding", "thinking_id",
                 "encrypted_content", "redacted_content", "cache_control", "cache_ttl",
                 "message_group", "context", "tool_id", "name", "namespace", "input", "content",
                 "is_error", "media_type", "media_ref"))
         return -1;
      const cJSON *context = get(b, "context");
      if (context && FIELDS(context, "origin", "authority", "trust", "sensitivity", "model_visible",
                            "revision_domain", "revision_scope", "revision_epoch"))
         return -1;
      const char *kind = cJSON_GetStringValue(get(b, "type"));
      if (!kind)
         return -1;
      const cJSON *field;
      cJSON_ArrayForEach(field, b)
      {
         const char *name = field->string;
         if (!strcmp(name, "type") || !strcmp(name, "cache_control") ||
             !strcmp(name, "cache_ttl") || !strcmp(name, "message_group") ||
             !strcmp(name, "context"))
            continue;
         int allowed = 0;
         if (!strcmp(kind, "text"))
            allowed = !strcmp(name, "text");
         else if (!strcmp(kind, "thinking"))
            allowed = !strcmp(name, "text") || !strcmp(name, "signature") ||
                      !strcmp(name, "thinking_binding") || !strcmp(name, "thinking_id") ||
                      !strcmp(name, "encrypted_content") || !strcmp(name, "redacted_content");
         else if (!strcmp(kind, "tool_use"))
            allowed = !strcmp(name, "tool_id") || !strcmp(name, "name") ||
                      !strcmp(name, "namespace") || !strcmp(name, "input");
         else if (!strcmp(kind, "tool_result"))
            allowed =
                !strcmp(name, "tool_id") || !strcmp(name, "content") || !strcmp(name, "is_error");
         else if (!strcmp(kind, "image") || !strcmp(kind, "document"))
            allowed = !strcmp(name, "media_type") || !strcmp(name, "media_ref");
         if (!allowed)
            return -1;
      }
   }
   return 0;
}
static int system_blocks(const cJSON *blocks, aimee_request_t *out)
{
   if (blocks_shape(blocks) || cJSON_GetArraySize(blocks) > AIMEE_IR_MAX_BLOCKS)
      return -1;
   cJSON *message = cJSON_CreateObject(), *copy_blocks = cJSON_Duplicate(blocks, 1);
   if (!message || !copy_blocks || !cJSON_AddStringToObject(message, "role", "system"))
   {
      cJSON_Delete(message);
      cJSON_Delete(copy_blocks);
      return -1;
   }
   if (!cJSON_AddItemToObject(message, "blocks", copy_blocks))
   {
      cJSON_Delete(message);
      cJSON_Delete(copy_blocks);
      return -1;
   }
   aimee_message_t parsed = {0};
   int rc = aimee_ir_message_parse(message, &parsed);
   cJSON_Delete(message);
   if (rc)
      return -1;
   out->system = parsed.blocks;
   out->n_system = parsed.n_blocks;
   parsed.blocks = NULL;
   parsed.n_blocks = 0;
   aimee_message_free(&parsed);
   return 0;
}
int aimee_ir_request_parse(const cJSON *json, aimee_request_t *out)
{
   if (!out || !aimee_ir_json_unique(json) ||
       FIELDS(json, "version", "model", "source_wire", "cache_policy", "system", "messages",
              "tools", "tool_choice", "parallel_tool_calls", "max_tokens", "temperature", "top_p",
              "top_k", "stream", "stop_sequences", "metadata", "service_tier", "prompt_cache_key",
              "prompt_cache_retention", "prompt_cache_options", "reasoning_effort",
              "reasoning_summary", "provider_config", "output_format"))
      return -1;
   const cJSON *version = get(json, "version");
   if (!cJSON_IsNumber(version) || version->valuedouble != 1)
      return -1;
   aimee_request_t result = {0};
   static const char *const caches[] = {"native", "stable_prefix"};
   int cache = 0, stream_present = 0;
   if (string(json, "model", &result.model) || wire(json, "source_wire", &result.frontend, 1) ||
       enumeration(json, "cache_policy", caches, 2, &cache, 1) ||
       system_blocks(get(json, "system"), &result))
      goto fail;
   result.cache_policy = (aimee_cache_policy_t)cache;
   const cJSON *messages = get(json, "messages"), *tools = get(json, "tools"),
               *stops = get(json, "stop_sequences");
   if (!cJSON_IsArray(messages) || !cJSON_IsArray(tools) || !cJSON_IsArray(stops) ||
       cJSON_GetArraySize(messages) > AIMEE_IR_MAX_BLOCKS ||
       cJSON_GetArraySize(tools) > AIMEE_IR_MAX_BLOCKS ||
       cJSON_GetArraySize(stops) > AIMEE_IR_MAX_BLOCKS)
      goto fail;
   result.n_messages = cJSON_GetArraySize(messages);
   result.n_tools = cJSON_GetArraySize(tools);
   result.n_stop = cJSON_GetArraySize(stops);
   if ((result.n_messages &&
        !(result.messages = calloc((size_t)result.n_messages, sizeof *result.messages))) ||
       (result.n_tools && !(result.tools = calloc((size_t)result.n_tools, sizeof *result.tools))) ||
       (result.n_stop &&
        !(result.stop_sequences = calloc((size_t)result.n_stop, sizeof *result.stop_sequences))))
      goto fail;
   for (int i = 0; i < result.n_messages; i++)
   {
      const cJSON *m = cJSON_GetArrayItem(messages, i);
      if (FIELDS(m, "role", "blocks", "content_scalar", "_compaction_boundary") ||
          blocks_shape(get(m, "blocks")) || aimee_ir_message_parse(m, &result.messages[i]))
         goto fail;
   }
   for (int i = 0; i < result.n_tools; i++)
      if (tool(cJSON_GetArrayItem(tools, i), &result.tools[i], 0))
         goto fail;
   for (int i = 0; i < result.n_stop; i++)
   {
      const cJSON *v = cJSON_GetArrayItem(stops, i);
      if (!cJSON_IsString(v) || !(result.stop_sequences[i] = strdup(v->valuestring)))
         goto fail;
   }
   static const char *const choices[] = {"unset", "auto", "none", "any", "tool"};
   int choice = 0;
   const cJSON *tc = get(json, "tool_choice");
   if (FIELDS(tc, "mode", "name") || enumeration(tc, "mode", choices, 5, &choice, 1) ||
       string(tc, "name", &result.tool_choice_name))
      goto fail;
   result.tool_choice = (aimee_tool_choice_t)choice;
   if ((choice != AIMEE_TOOL_CHOICE_TOOL && result.tool_choice_name) ||
       boolean(json, "parallel_tool_calls", &result.parallel_tool_calls,
               &result.has_parallel_tool_calls) ||
       boolean(json, "stream", &result.stream, &stream_present) || !stream_present ||
       integer(json, "max_tokens", &result.max_tokens, &result.has_max_tokens) ||
       integer(json, "top_k", &result.top_k, &result.has_top_k) ||
       number(json, "temperature", &result.temperature, &result.has_temperature) ||
       number(json, "top_p", &result.top_p, &result.has_top_p) ||
       copy(json, "metadata", &result.metadata) ||
       copy(json, "prompt_cache_options", &result.prompt_cache_options) ||
       string(json, "service_tier", &result.service_tier) ||
       string(json, "prompt_cache_key", &result.prompt_cache_key) ||
       string(json, "prompt_cache_retention", &result.prompt_cache_retention) ||
       string(json, "reasoning_summary", &result.reasoning_summary))
      goto fail;
   const cJSON *effort = get(json, "reasoning_effort");
   if (effort && (!cJSON_IsString(effort) ||
                  aimee_ir_effort_parse(effort->valuestring, &result.reasoning_effort)))
      goto fail;
   const cJSON *provider = get(json, "provider_config");
   if (provider)
   {
      const cJSON *wire_name = get(provider, "wire");
      if (FIELDS(provider, "wire", "thinking") || !cJSON_IsString(wire_name) ||
          strcmp(wire_name->valuestring, "anthropic") ||
          !cJSON_IsObject(get(provider, "thinking")) ||
          copy(provider, "thinking", &result.thinking))
         goto fail;
   }
   const cJSON *output = get(json, "output_format");
   if (output)
   {
      static const char *const kinds[] = {"", "text", "json_object", "json_schema"};
      int kind = 0;
      if (FIELDS(output, "kind", "name", "description", "schema", "strict") ||
          enumeration(output, "kind", kinds, 4, &kind, 1) || !kind ||
          string(output, "name", &result.output_format.name) ||
          string(output, "description", &result.output_format.description) ||
          copy(output, "schema", &result.output_format.schema) ||
          strict(output, &result.output_format.strict, 1))
         goto fail;
      result.output_format.kind = (aimee_output_kind_t)kind;
   }
   if (aimee_ir_request_admit(&result))
      goto fail;
   aimee_request_free(out);
   *out = result;
   return 0;
fail:
   aimee_request_free(&result);
   return -1;
}
