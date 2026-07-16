/* agent_bridge.c: Provider layer — LLM request/response protocol, HTTP client, tunnel lifecycle */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "util.h"
#include "agent_request_shaping.h"
#include "agent_protocol.h"
#include "config.h"
#include "log.h"
#include "model_sampling.h"
#include "model_registry.h"
#include "tool_call_args.h"
#include "cJSON.h"
#include <string.h>
/* From agent_http.c */
#include "agent_exec.h"
#include <time.h>
/* From agent_tunnel.c */
#include "agent_tunnel.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* ================================================================
 * From: agent_protocol.c
 * ================================================================ */

/* --- Request builders --- */
static int agent_is_local_llama_compat(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "llama_native") == 0 || strcmp(agent->provider, "ollama") == 0 ||
       strcmp(agent->provider, "llama-eval") == 0)
      return 1;
   return str_contains_ci(agent->name, "llama") || str_contains_ci(agent->model, "qwen") ||
          str_contains_ci(agent->model, ".gguf");
}
static int agent_is_mistral_vibe_model(const agent_t *agent)
{
   return agent && strcmp(agent->provider, "mistral") == 0 &&
          str_contains_ci(agent->model, "mistral-vibe-cli");
}
static int json_array_contains_string(cJSON *arr, const char *value)
{
   if (!cJSON_IsArray(arr) || !value || !value[0])
      return 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (cJSON_IsString(item) && strcmp(item->valuestring, value) == 0)
         return 1;
   }
   return 0;
}
static void strip_private_json_fields(cJSON *node)
{
   if (!node)
      return;
   if (cJSON_IsObject(node))
   {
      cJSON *child = node->child;
      while (child)
      {
         cJSON *next = child->next;
         if (child->string && child->string[0] == '_')
            cJSON_DeleteItemFromObjectCaseSensitive(node, child->string);
         else
            strip_private_json_fields(child);
         child = next;
      }
      return;
   }
   if (cJSON_IsArray(node))
   {
      cJSON *child;
      cJSON_ArrayForEach(child, node) strip_private_json_fields(child);
   }
}
static cJSON *provider_payload_without_private_fields(cJSON *payload)
{
   cJSON *copy;
   if (!payload)
      return NULL;
   copy = cJSON_Duplicate(payload, 1);
   if (!copy)
      return NULL;
   strip_private_json_fields(copy);
   return copy;
}
static void openrouter_add_routing_hint(const agent_t *agent, cJSON *req)
{
   if (!agent || strcmp(agent->provider, "openrouter") != 0 || !req)
      return;

   cJSON_AddStringToObject(req, "route", "fallback");
   cJSON *models = cJSON_CreateArray();
   if (!models)
      return;
   if (agent->model[0])
      cJSON_AddItemToArray(models, cJSON_CreateString(agent->model));

   static const char *fallbacks[] = {"anthropic/claude-opus-4.7", "google/gemini-2.5-flash",
                                     "mistralai/mistral-large-2512", NULL};
   for (int i = 0; fallbacks[i]; i++)
   {
      if (!json_array_contains_string(models, fallbacks[i]))
         cJSON_AddItemToArray(models, cJSON_CreateString(fallbacks[i]));
   }
   cJSON_AddItemToObject(req, "models", models);
}
int agent_request_max_tokens(const agent_t *agent, int requested)
{
   if (requested > 0)
      return requested; /* caller pinned an explicit budget (e.g. a short ping) */
   if (agent && agent->max_tokens > 0)
      return agent->max_tokens; /* agents.json / --max-tokens pinned a cap */
   /* No explicit cap: use the model's own output ceiling, never a hardcoded one. */
   return model_max_output(agent ? agent->provider : NULL, agent ? agent->model : NULL);
}

cJSON *agent_build_request_openai(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *safe_messages = provider_payload_without_private_fields(messages);
   cJSON_AddStringToObject(req, "model", agent->model);
   agent_request_shape_openai_messages(agent, safe_messages ? safe_messages : messages);
   if (safe_messages)
      cJSON_AddItemToObject(req, "messages", safe_messages);
   else
      cJSON_AddItemReferenceToObject(req, "messages", messages);
   int has_tools = tools && cJSON_GetArraySize(tools) > 0;
   int is_minimax =
       agent && (strcmp(agent->provider, "minimax") == 0 ||
                 strstr(agent->endpoint, "api.minimax.") || strstr(agent->model, "MiniMax-M2.7"));
   if (has_tools)
   {
      cJSON_AddItemReferenceToObject(req, "tools", tools);
      if (agent_is_local_llama_compat(agent) || is_minimax)
         cJSON_AddBoolToObject(req, "parallel_tool_calls", 0);
      /* mistral and minimax default to not using tools without an explicit directive. */
      if ((agent && strcmp(agent->provider, "mistral") == 0) || is_minimax)
         cJSON_AddStringToObject(req, "tool_choice", "auto");
   }

   cJSON_AddNumberToObject(req, "max_tokens", agent_request_max_tokens(agent, max_tokens));
   if (agent_is_mistral_vibe_model(agent))
   {
      cJSON_AddStringToObject(req, "reasoning_effort", "high");
      cJSON_AddNumberToObject(req, "temperature", 1.0);
   }
   else
      model_sampling_apply_openai(agent, req, temperature);
   if (agent_is_local_llama_compat(agent) && !agent_request_prefers_no_think_prompt(agent))
   {
      cJSON *kwargs = cJSON_AddObjectToObject(req, "chat_template_kwargs");
      cJSON_AddBoolToObject(kwargs, "enable_thinking", 0);
   }
   openrouter_add_routing_hint(agent, req);
   /* M2.7-only: MiniMax-M3 rejects reasoning_split with HTTP 400, and it is never read back. */
   if (agent && strstr(agent->model, "MiniMax-M2.7"))
      cJSON_AddBoolToObject(req, "reasoning_split", 1);

   return req;
}
cJSON *agent_build_request_responses(const agent_t *agent, cJSON *input, cJSON *tools,
                                     const char *system_prompt)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *safe_input = provider_payload_without_private_fields(input);
   cJSON_AddStringToObject(req, "model", agent->model);

   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(req, "instructions", system_prompt);
   else
      cJSON_AddStringToObject(req, "instructions", "You are an execution agent.");

   cJSON_AddBoolToObject(req, "store", 0);
   cJSON_AddBoolToObject(req, "stream", 1);
   if (safe_input)
      cJSON_AddItemToObject(req, "input", safe_input);
   else
      cJSON_AddItemReferenceToObject(req, "input", input);
   if (tools && cJSON_GetArraySize(tools) > 0)
      cJSON_AddItemReferenceToObject(req, "tools", tools);

   return req;
}
cJSON *agent_build_request_anthropic(const agent_t *agent, cJSON *messages, cJSON *tools,
                                     const char *system_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *safe_messages = provider_payload_without_private_fields(messages);
   cJSON_AddStringToObject(req, "model", agent->model);

   cJSON_AddNumberToObject(req, "max_tokens", agent_request_max_tokens(agent, max_tokens));

   /* §3 cache-aware shaping: when enabled, mark the aimee-owned STABLE system
    * prefix cacheable on this (tool-bearing) Anthropic request, matching the
    * non-tools path. Default-off so the flag-rollout program can flip it
    * deliberately. The cache_min_chars floor is applied to the stable prefix
    * inside the helper, not the whole prompt. */
   config_t cfg;
   int cache_marking = (config_load(&cfg) == 0 && cfg.cache_shaping_enabled) ? 1 : 0;
   agent_anthropic_set_system(req, system_prompt, cache_marking,
                              cache_marking ? cfg.cache_min_chars : 0);

   if (safe_messages)
      cJSON_AddItemToObject(req, "messages", safe_messages);
   else
      cJSON_AddItemReferenceToObject(req, "messages", messages);
   if (tools && cJSON_GetArraySize(tools) > 0)
      cJSON_AddItemReferenceToObject(req, "tools", tools);

   model_sampling_apply_anthropic(agent, req, temperature);

   return req;
}

/* --- Response parsers --- */

/* Split a <think>...</think> reasoning preamble off a model response.
 * Qwen3 and similar reasoning models embed thinking in the content field when
 * the llama.cpp server is not configured to separate it into reasoning_content.
 *
 * Prefix-anchored via the shared text_split_reasoning_prefix(): this previously
 * removed <think> pairs (and bare close tags) from ANYWHERE in the text, which
 * silently corrupted any answer that legitimately discussed the tag — the same
 * defect that killed curator jobs summarising this very function. One rule, one
 * implementation, shared with provider_client.c and llm-chat.py.
 * Returns a new malloc'd string; caller must free. NULL on alloc failure. */
static char *strip_thinking_blocks(const char *text)
{
   if (!text)
      return NULL;
   return strdup(text_split_reasoning_prefix(text, NULL, NULL));
}
static int append_text(char **out, const char *text)
{
   if (!text || !text[0])
      return 0;
   if (!*out)
   {
      *out = strdup(text);
      return *out ? 0 : -1;
   }
   size_t old_len = strlen(*out);
   size_t add_len = strlen(text);
   char *grown = realloc(*out, old_len + add_len + 1);
   if (!grown)
      return -1;
   memcpy(grown + old_len, text, add_len + 1);
   *out = grown;
   return 0;
}
static char *openai_content_to_text(cJSON *content, int include_thinking)
{
   if (!content)
      return NULL;
   if (cJSON_IsString(content) && content->valuestring[0])
      return strdup(content->valuestring);
   if (!cJSON_IsArray(content))
      return NULL;

   char *out = NULL;
   int n = cJSON_GetArraySize(content);
   for (int i = 0; i < n; i++)
   {
      cJSON *part = cJSON_GetArrayItem(content, i);
      if (cJSON_IsString(part))
      {
         if (append_text(&out, part->valuestring) != 0)
            break;
         continue;
      }
      if (!cJSON_IsObject(part))
         continue;

      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(part, "type"));
      if (!type)
         continue;
      if (strcmp(type, "text") == 0 || strcmp(type, "output_text") == 0)
      {
         const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(part, "text"));
         if (append_text(&out, text) != 0)
            break;
      }
      else if (include_thinking &&
               (strcmp(type, "thinking") == 0 || strcmp(type, "reasoning") == 0))
      {
         cJSON *thinking = cJSON_GetObjectItem(part, "thinking");
         if (!thinking)
            thinking = cJSON_GetObjectItem(part, "reasoning");
         char *text = openai_content_to_text(thinking, 0);
         if (text)
         {
            if (append_text(&out, text) != 0)
            {
               free(text);
               break;
            }
            free(text);
         }
      }
   }
   return out;
}

/* Capture the provider-reported model id (the response's "model" field) into the
 * parsed result, so billing can prefer it over the requested/served alias. */
static void parse_capture_model(cJSON *root, parsed_response_t *out)
{
   cJSON *m = cJSON_GetObjectItem(root, "model");
   if (m && cJSON_IsString(m) && m->valuestring)
      snprintf(out->model, sizeof(out->model), "%s", m->valuestring);
}

void agent_parse_response_openai(cJSON *root, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   parse_capture_model(root, out);

   /* Usage */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
      cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
      if (pt && cJSON_IsNumber(pt))
         out->prompt_tokens = pt->valueint;
      if (ct && cJSON_IsNumber(ct))
         out->completion_tokens = ct->valueint;
   }

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
      return;

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   if (finish && cJSON_IsString(finish) && finish->valuestring)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", finish->valuestring);
   cJSON *message = cJSON_GetObjectItem(choice, "message");
   if (!message)
      return;

   if (finish && cJSON_IsString(finish) && strcmp(finish->valuestring, "tool_calls") == 0)
   {
      out->is_tool_call = 1;
      out->assistant_message = cJSON_Duplicate(message, 1);

      cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
      if (tool_calls && cJSON_IsArray(tool_calls))
      {
         int n = cJSON_GetArraySize(tool_calls);
         if (n > AGENT_MAX_TOOL_CALLS)
            n = AGENT_MAX_TOOL_CALLS;
         for (int i = 0; i < n; i++)
         {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            if (!fn)
               continue;
            cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
            cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");

            parsed_tool_call_t *call = &out->calls[out->call_count];
            if (tc_id && cJSON_IsString(tc_id))
               snprintf(call->id, sizeof(call->id), "%s", tc_id->valuestring);
            if (fn_name && cJSON_IsString(fn_name))
               snprintf(call->name, sizeof(call->name), "%s", fn_name->valuestring);
            call->arguments = tool_call_copy_valid_arguments(fn_args);
            tool_call_normalize_assistant_arguments(out->assistant_message, i, call->arguments);
            out->call_count++;
         }
         tool_call_sanitize_assistant_arguments(out->assistant_message);
      }
   }
   else
   {
      /* Text response */
      cJSON *content = cJSON_GetObjectItem(message, "content");
      out->content = openai_content_to_text(content, 0);

      /* Reasoning models (e.g. qwen3) may return empty content with
         reasoning_content holding the actual output. Fall back to it. */
      if (!out->content || !out->content[0])
      {
         cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
         if (reasoning)
         {
            free(out->content);
            out->content = openai_content_to_text(reasoning, 1);
         }
      }

      /* Strip <think>...</think> blocks that Qwen3/reasoning models embed in
       * content when the server doesn't separate them into reasoning_content. */
      if (out->content && (strstr(out->content, "<think>") || strstr(out->content, "</think>")))
      {
         char *stripped = strip_thinking_blocks(out->content);
         if (stripped)
         {
            free(out->content);
            out->content = stripped[0] ? stripped : (free(stripped), NULL);
         }
      }
      if (out->content)
      {
         char *stripped = strip_llm_private_scaffold(out->content);
         if (stripped)
         {
            free(out->content);
            out->content = stripped;
         }
      }

      /* Compatibility: some llama.cpp builds return finish_reason="stop" even
       * when tool_calls are present in the message. Detect and promote. */
      cJSON *tc_arr = cJSON_GetObjectItem(message, "tool_calls");
      if (tc_arr && cJSON_IsArray(tc_arr) && cJSON_GetArraySize(tc_arr) > 0)
      {
         free(out->content);
         out->content = NULL;
         out->is_tool_call = 1;
         out->assistant_message = cJSON_Duplicate(message, 1);
         int n = cJSON_GetArraySize(tc_arr);
         if (n > AGENT_MAX_TOOL_CALLS)
            n = AGENT_MAX_TOOL_CALLS;
         for (int i = 0; i < n; i++)
         {
            cJSON *tc = cJSON_GetArrayItem(tc_arr, i);
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            if (!fn)
               continue;
            cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
            cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
            parsed_tool_call_t *call = &out->calls[out->call_count];
            if (tc_id && cJSON_IsString(tc_id))
               snprintf(call->id, sizeof(call->id), "%s", tc_id->valuestring);
            if (fn_name && cJSON_IsString(fn_name))
               snprintf(call->name, sizeof(call->name), "%s", fn_name->valuestring);
            call->arguments = tool_call_copy_valid_arguments(fn_args);
            tool_call_normalize_assistant_arguments(out->assistant_message, i, call->arguments);
            out->call_count++;
         }
         tool_call_sanitize_assistant_arguments(out->assistant_message);
      }
   }
}

/* Extract content and tool calls from a single Responses API output item cJSON. */
static void parse_responses_output_item(cJSON *item, parsed_response_t *out)
{
   cJSON *type = cJSON_GetObjectItem(item, "type");
   if (!type || !cJSON_IsString(type))
      return;
   if (strcmp(type->valuestring, "function_call") == 0)
   {
      out->is_tool_call = 1;
      if (out->call_count < AGENT_MAX_TOOL_CALLS)
      {
         parsed_tool_call_t *call = &out->calls[out->call_count];
         cJSON *cid = cJSON_GetObjectItem(item, "call_id");
         cJSON *nm = cJSON_GetObjectItem(item, "name");
         cJSON *args = cJSON_GetObjectItem(item, "arguments");
         if (cid && cJSON_IsString(cid))
            snprintf(call->id, sizeof(call->id), "%s", cid->valuestring);
         if (nm && cJSON_IsString(nm))
            snprintf(call->name, sizeof(call->name), "%s", nm->valuestring);
         if (args && cJSON_IsString(args))
            call->arguments = strdup(args->valuestring);
         else
            call->arguments = strdup("{}");
         out->call_count++;
      }
   }
   else if (strcmp(type->valuestring, "message") == 0)
   {
      cJSON *content = cJSON_GetObjectItem(item, "content");
      if (content && cJSON_IsArray(content))
      {
         int appended_text = 0;
         int cn = cJSON_GetArraySize(content);
         for (int j = 0; j < cn; j++)
         {
            cJSON *part = cJSON_GetArrayItem(content, j);
            cJSON *pt = cJSON_GetObjectItem(part, "type");
            if (pt && cJSON_IsString(pt) && strcmp(pt->valuestring, "output_text") == 0)
            {
               cJSON *text = cJSON_GetObjectItem(part, "text");
               if (text && cJSON_IsString(text) && text->valuestring && text->valuestring[0])
               {
                  if (!appended_text && out->content && out->content[0])
                  {
                     size_t n = strlen(out->content);
                     const char *sep =
                         (n >= 2 && out->content[n - 1] == '\n' && out->content[n - 2] == '\n') ? ""
                         : out->content[n - 1] == '\n' ? "\n"
                                                       : "\n\n";
                     (void)append_text(&out->content, sep);
                  }
                  (void)append_text(&out->content, text->valuestring);
                  appended_text = 1;
               }
            }
         }
      }
   }
}
static void responses_take_longer_content(parsed_response_t *out, char **candidate)
{
   if (!out || !candidate || !*candidate)
      return;
   if (!(*candidate)[0])
   {
      free(*candidate);
      *candidate = NULL;
      return;
   }
   if (!out->content || strlen(*candidate) > strlen(out->content))
   {
      free(out->content);
      out->content = *candidate;
      *candidate = NULL;
      return;
   }
   free(*candidate);
   *candidate = NULL;
}
static void responses_append_output_text_part(cJSON *part, char **out_text)
{
   if (!part || !out_text)
      return;
   cJSON *type = cJSON_GetObjectItem(part, "type");
   if (!cJSON_IsString(type) || strcmp(type->valuestring, "output_text") != 0)
      return;
   cJSON *text = cJSON_GetObjectItem(part, "text");
   if (cJSON_IsString(text))
      (void)append_text(out_text, text->valuestring);
}
static int responses_append_sse_data(char **data, size_t *len, size_t *cap, const char *value,
                                     size_t value_len)
{
   if (!data || !len || !cap || !value)
      return -1;
   size_t extra = value_len + (*len > 0 ? 1 : 0) + 1;
   if (*len > SIZE_MAX - extra)
      return -1;
   size_t need = *len + extra;
   if (need > *cap)
   {
      size_t next = *cap ? *cap : 256;
      while (next < need)
      {
         if (next > SIZE_MAX / 2)
            return -1;
         next *= 2;
      }
      char *grown = realloc(*data, next);
      if (!grown)
         return -1;
      *data = grown;
      *cap = next;
   }
   if (*len > 0)
      (*data)[(*len)++] = '\n';
   memcpy(*data + *len, value, value_len);
   *len += value_len;
   (*data)[*len] = '\0';
   return 0;
}
static const char *responses_sse_field_value(const char *line, size_t line_len, const char *field,
                                             size_t field_len, size_t *value_len)
{
   if (!line || !field || !value_len || line_len <= field_len || line[field_len] != ':' ||
       strncmp(line, field, field_len) != 0)
      return NULL;
   size_t off = field_len + 1;
   if (off < line_len && line[off] == ' ')
      off++;
   *value_len = line_len - off;
   return line + off;
}
static const char *responses_text_value(cJSON *value)
{
   if (cJSON_IsString(value))
      return value->valuestring;
   if (!cJSON_IsObject(value))
      return NULL;
   cJSON *nested = cJSON_GetObjectItem(value, "text");
   if (!cJSON_IsString(nested))
      nested = cJSON_GetObjectItem(value, "value");
   return cJSON_IsString(nested) ? nested->valuestring : NULL;
}
static void responses_handle_sse_event(const char *event, const char *data, parsed_response_t *out,
                                       cJSON *collected_output, char **delta_text, char **done_text,
                                       char **part_text, cJSON **completed_response)
{
   if (!event || !event[0] || !data || !data[0])
      return;
   cJSON *ev = cJSON_Parse(data);
   if (!ev)
      return;
   if (strcmp(event, "response.output_item.done") == 0)
   {
      cJSON *item = cJSON_GetObjectItem(ev, "item");
      if (item)
      {
         parse_responses_output_item(item, out);
         cJSON *dup = cJSON_Duplicate(item, 1);
         if (dup)
            cJSON_AddItemToArray(collected_output, dup);
      }
   }
   else if (strcmp(event, "response.output_text.delta") == 0)
   {
      const char *text = responses_text_value(cJSON_GetObjectItem(ev, "delta"));
      if (text)
         (void)append_text(delta_text, text);
   }
   else if (strcmp(event, "response.output_text.done") == 0)
   {
      const char *text = responses_text_value(cJSON_GetObjectItem(ev, "text"));
      if (text)
         (void)append_text(done_text, text);
   }
   else if (strcmp(event, "response.content_part.done") == 0)
   {
      cJSON *part = cJSON_GetObjectItem(ev, "part");
      responses_append_output_text_part(part, part_text);
   }
   else if (strcmp(event, "response.completed") == 0)
   {
      cJSON *resp = cJSON_GetObjectItem(ev, "response");
      if (resp && completed_response)
      {
         cJSON_Delete(*completed_response);
         *completed_response = cJSON_Duplicate(resp, 1);
      }
   }

   cJSON_Delete(ev);
}
static void responses_parse_sse_events(const char *body, parsed_response_t *out,
                                       cJSON *collected_output, char **delta_text, char **done_text,
                                       char **part_text, cJSON **completed_response)
{
   char event[128] = {0};
   char *data = NULL;
   size_t data_len = 0;
   size_t data_cap = 0;

   const char *p = body;
   while (p && *p)
   {
      const char *line = p;
      const char *eol = strpbrk(p, "\r\n");
      size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
      if (eol)
      {
         if (*eol == '\r' && eol[1] == '\n')
            p = eol + 2;
         else
            p = eol + 1;
      }
      else
      {
         p = line + line_len;
      }
      if (line_len == 0)
      {
         responses_handle_sse_event(event, data, out, collected_output, delta_text, done_text,
                                    part_text, completed_response);
         event[0] = '\0';
         data_len = 0;
         if (data)
            data[0] = '\0';
         continue;
      }
      size_t value_len = 0;
      const char *value = responses_sse_field_value(line, line_len, "event", 5, &value_len);
      if (value)
      {
         size_t n = value_len < sizeof(event) - 1 ? value_len : sizeof(event) - 1;
         memcpy(event, value, n);
         event[n] = '\0';
         continue;
      }
      value = responses_sse_field_value(line, line_len, "data", 4, &value_len);
      if (value)
         (void)responses_append_sse_data(&data, &data_len, &data_cap, value, value_len);
   }

   responses_handle_sse_event(event, data, out, collected_output, delta_text, done_text, part_text,
                              completed_response);
   free(data);
}
void agent_parse_response_responses(const char *body, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!body)
      return;
   cJSON *collected_output = cJSON_CreateArray();
   char *delta_text = NULL;
   char *done_text = NULL;
   char *part_text = NULL;
   cJSON *completed_resp = NULL;
   responses_parse_sse_events(body, out, collected_output, &delta_text, &done_text, &part_text,
                              &completed_resp);
   responses_take_longer_content(out, &part_text);
   responses_take_longer_content(out, &done_text);
   responses_take_longer_content(out, &delta_text);

   /* Pass 2: if output_item.done yielded nothing, fall back to response.completed */
   if (!out->content && !out->is_tool_call)
   {
      cJSON *resp = NULL;
      if (completed_resp)
         resp = cJSON_Duplicate(completed_resp, 1);
      else
         resp = cJSON_Parse(body);
      if (resp)
      {
         cJSON *output = cJSON_GetObjectItem(resp, "output");
         if (output && cJSON_IsArray(output))
         {
            int n = cJSON_GetArraySize(output);
            for (int i = 0; i < n; i++)
               parse_responses_output_item(cJSON_GetArrayItem(output, i), out);
         }
         cJSON_Delete(resp);
      }
   }
   /* Collect usage from response.completed */
   if (completed_resp)
   {
      cJSON *usage = cJSON_GetObjectItem(completed_resp, "usage");
      if (usage)
      {
         cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
         cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
         if (it && cJSON_IsNumber(it))
            out->prompt_tokens = it->valueint;
         if (ot && cJSON_IsNumber(ot))
            out->completion_tokens = ot->valueint;
         cJSON *cw = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
         cJSON *cr = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
         if (cw && cJSON_IsNumber(cw))
            out->cache_write_tokens = cw->valueint;
         if (cr && cJSON_IsNumber(cr))
            out->cache_read_tokens = cr->valueint;
      }
   }
   cJSON_Delete(completed_resp);
   /* For multi-turn tool use, store collected output items as assistant_message */
   if (out->is_tool_call)
      out->assistant_message = collected_output;
   else
      cJSON_Delete(collected_output);
}
void agent_parse_response_anthropic(cJSON *root, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   parse_capture_model(root, out);

   /* Usage */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
      cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
      if (it && cJSON_IsNumber(it))
         out->prompt_tokens = it->valueint;
      if (ot && cJSON_IsNumber(ot))
         out->completion_tokens = ot->valueint;
      cJSON *cw = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
      cJSON *cr = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
      if (cw && cJSON_IsNumber(cw))
         out->cache_write_tokens = cw->valueint;
      if (cr && cJSON_IsNumber(cr))
         out->cache_read_tokens = cr->valueint;
   }

   /* Check stop_reason for tool_use */
   cJSON *stop = cJSON_GetObjectItem(root, "stop_reason");
   if (stop && cJSON_IsString(stop) && stop->valuestring)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", stop->valuestring);
   int has_tool_use = (stop && cJSON_IsString(stop) && strcmp(stop->valuestring, "tool_use") == 0);

   cJSON *content = cJSON_GetObjectItem(root, "content");
   if (!content || !cJSON_IsArray(content))
      return;

   /* Extract text and tool_use blocks from content array */
   int n = cJSON_GetArraySize(content);
   for (int i = 0; i < n; i++)
   {
      cJSON *block = cJSON_GetArrayItem(content, i);
      cJSON *type = cJSON_GetObjectItem(block, "type");
      if (!type || !cJSON_IsString(type))
         continue;

      if (strcmp(type->valuestring, "tool_use") == 0 && out->call_count < AGENT_MAX_TOOL_CALLS)
      {
         out->is_tool_call = 1;
         parsed_tool_call_t *call = &out->calls[out->call_count];
         cJSON *id = cJSON_GetObjectItem(block, "id");
         cJSON *nm = cJSON_GetObjectItem(block, "name");
         cJSON *input = cJSON_GetObjectItem(block, "input");

         if (id && cJSON_IsString(id))
            snprintf(call->id, sizeof(call->id), "%s", id->valuestring);
         if (nm && cJSON_IsString(nm))
            snprintf(call->name, sizeof(call->name), "%s", nm->valuestring);
         if (input)
         {
            char *s = cJSON_PrintUnformatted(input);
            call->arguments = s ? s : strdup("{}");
         }
         else
            call->arguments = strdup("{}");
         out->call_count++;
      }
      else if (strcmp(type->valuestring, "text") == 0 && !has_tool_use)
      {
         cJSON *text = cJSON_GetObjectItem(block, "text");
         if (text && cJSON_IsString(text))
            out->content = strdup(text->valuestring);
      }
   }

   /* Store the full content array as assistant_message for multi-turn */
   if (out->is_tool_call)
      out->assistant_message = cJSON_Duplicate(content, 1);
}
void agent_free_parsed_response(parsed_response_t *p)
{
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
   free(p->content);
   if (p->assistant_message)
      cJSON_Delete(p->assistant_message);
}

/* --- Message utilities --- */

/* Merge consecutive same-role messages in a cJSON messages array.
 * Returns the number of merges performed. Idempotent. */
int messages_compact_consecutive(cJSON *messages)
{
   if (!messages || !cJSON_IsArray(messages))
      return 0;

   int merged = 0;
   cJSON *cur = messages->child;

   while (cur && cur->next)
   {
      cJSON *next = cur->next;
      const char *cur_role = cJSON_GetStringValue(cJSON_GetObjectItem(cur, "role"));
      const char *next_role = cJSON_GetStringValue(cJSON_GetObjectItem(next, "role"));

      if (!cur_role || !next_role || strcmp(cur_role, next_role) != 0)
      {
         cur = next;
         continue;
      }

      /* Tool results are semantically keyed by tool_call_id. Merging two
       * consecutive tool messages would drop an ID and make the provider see
       * fewer tool responses than assistant tool calls. */
      if (strcmp(cur_role, "tool") == 0 || cJSON_GetObjectItem(cur, "tool_call_id") ||
          cJSON_GetObjectItem(next, "tool_call_id") || cJSON_GetObjectItem(cur, "tool_calls") ||
          cJSON_GetObjectItem(next, "tool_calls"))
      {
         cur = next;
         continue;
      }

      /* Same role — merge next's content into cur */
      cJSON *cur_content = cJSON_GetObjectItem(cur, "content");
      cJSON *next_content = cJSON_GetObjectItem(next, "content");

      const char *cur_text = cJSON_GetStringValue(cur_content);
      const char *next_text = cJSON_GetStringValue(next_content);

      /* Only merge string content; skip messages with structured content (tool_calls, etc.) */
      if (!cur_text || !next_text)
      {
         cur = next;
         continue;
      }

      size_t new_len = strlen(cur_text) + 2 + strlen(next_text) + 1;
      char *merged_text = malloc(new_len);
      if (!merged_text)
      {
         cur = next;
         continue;
      }
      snprintf(merged_text, new_len, "%s\n\n%s", cur_text, next_text);

      cJSON_ReplaceItemInObject(cur, "content", cJSON_CreateString(merged_text));
      free(merged_text);

      /* Remove next from the array and free it */
      cJSON_DetachItemViaPointer(messages, next);
      cJSON_Delete(next);

      merged++;
      /* Don't advance cur — check if the next one also matches */
   }

   return merged;
}

/* --- Message history repair ---
 *
 * Scan message history for inconsistencies and repair them:
 * 1. Orphaned tool calls: assistant requested tool_use but no matching result exists.
 *    -> Insert synthetic cancellation result.
 * 2. Orphaned tool results: a tool result exists with no matching tool call.
 *    -> Remove the orphan.
 * 3. Trailing tool calls: conversation ends with unanswered tool calls.
 *    -> Fill with cancellation results.
 *
 * Handles OpenAI (tool_calls/tool_call_id), Anthropic (tool_use/tool_result),
 * and Responses API (function_call/function_call_output) message formats.
 *
 * Idempotent: running twice produces the same result.
 */
static const char *CANCEL_MSG = "[Tool call was cancelled or timed out]";

/* Collect all tool call IDs from a message array.
 * Returns a cJSON object used as a set: keys are IDs, values are true. */
static cJSON *collect_tool_call_ids(cJSON *messages)
{
   cJSON *ids = cJSON_CreateObject();
   cJSON *msg;

   cJSON_ArrayForEach(msg, messages)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));

      /* OpenAI: assistant message with tool_calls array */
      if (role && strcmp(role, "assistant") == 0)
      {
         cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
         if (tcs && cJSON_IsArray(tcs))
         {
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcs)
            {
               const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
               if (id)
                  cJSON_AddBoolToObject(ids, id, 1);
            }
         }

         /* Anthropic: assistant message with content array containing tool_use blocks */
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_use") == 0)
               {
                  const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
                  if (id)
                     cJSON_AddBoolToObject(ids, id, 1);
               }
            }
         }
      }

      /* Responses API: top-level function_call items */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid)
            cJSON_AddBoolToObject(ids, cid, 1);
      }
   }

   return ids;
}

/* Collect all tool result IDs from a message array. */
static cJSON *collect_tool_result_ids(cJSON *messages)
{
   cJSON *ids = cJSON_CreateObject();
   cJSON *msg;

   cJSON_ArrayForEach(msg, messages)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));

      /* OpenAI: role=tool with tool_call_id */
      if (role && strcmp(role, "tool") == 0)
      {
         const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "tool_call_id"));
         if (id)
            cJSON_AddBoolToObject(ids, id, 1);
      }

      /* Anthropic: user message with content array containing tool_result blocks */
      if (role && strcmp(role, "user") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_result") == 0)
               {
                  const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_use_id"));
                  if (id)
                     cJSON_AddBoolToObject(ids, id, 1);
               }
            }
         }
      }

      /* Responses API: function_call_output with call_id */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call_output") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid)
            cJSON_AddBoolToObject(ids, cid, 1);
      }
   }

   return ids;
}

/* Detect the message format: 0=OpenAI, 1=Anthropic, 2=Responses API */
static int detect_format(cJSON *messages)
{
   cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      /* Responses API: top-level type=function_call or function_call_output */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && (strcmp(type, "function_call") == 0 || strcmp(type, "function_call_output") == 0))
         return 2;

      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (!role)
         continue;

      /* Anthropic: assistant with content array containing tool_use blocks */
      if (strcmp(role, "assistant") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *btype = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (btype && strcmp(btype, "tool_use") == 0)
                  return 1;
            }
         }
      }

      /* OpenAI: role=tool messages */
      if (strcmp(role, "tool") == 0)
         return 0;
   }

   return 0; /* default to OpenAI */
}

/* Insert synthetic cancellation results for orphaned tool calls (OpenAI format) */
static int repair_orphans_openai(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* Find orphaned calls (call exists but no result) and insert cancellation */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue; /* has a matching result */

      /* Insert a synthetic tool result after the assistant message that made the call */
      /* Find the assistant message containing this call */
      cJSON *msg;
      cJSON *insert_after = NULL;
      cJSON_ArrayForEach(msg, messages)
      {
         const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
         if (!role || strcmp(role, "assistant") != 0)
            continue;
         cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
         if (!tcs || !cJSON_IsArray(tcs))
            continue;
         cJSON *tc;
         cJSON_ArrayForEach(tc, tcs)
         {
            const char *tc_id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
            if (tc_id && strcmp(tc_id, id_item->string) == 0)
            {
               insert_after = msg;
               break;
            }
         }
         if (insert_after)
            break;
      }

      /* Create synthetic result */
      cJSON *tool_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(tool_msg, "role", "tool");
      cJSON_AddStringToObject(tool_msg, "tool_call_id", id_item->string);
      cJSON_AddStringToObject(tool_msg, "content", CANCEL_MSG);

      if (insert_after)
      {
         /* Insert right after the assistant message (or after existing tool results) */
         cJSON *pos = insert_after->next;
         while (pos)
         {
            const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(pos, "role"));
            if (!r || strcmp(r, "tool") != 0)
               break;
            pos = pos->next;
         }
         if (pos)
         {
            /* Insert before pos */
            tool_msg->next = pos;
            tool_msg->prev = pos->prev;
            if (pos->prev)
               pos->prev->next = tool_msg;
            pos->prev = tool_msg;
         }
         else
         {
            cJSON_AddItemToArray(messages, tool_msg);
         }
      }
      else
      {
         cJSON_AddItemToArray(messages, tool_msg);
      }
      repairs++;
   }

   /* Remove orphaned results (result exists but no matching call) */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next = msg->next;
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (role && strcmp(role, "tool") == 0)
      {
         const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "tool_call_id"));
         if (tcid && !cJSON_GetObjectItem(call_ids, tcid))
         {
            cJSON_DetachItemViaPointer(messages, msg);
            cJSON_Delete(msg);
            repairs++;
         }
      }
      msg = next;
   }

   return repairs;
}

/* Insert synthetic cancellation results for orphaned tool calls (Anthropic format) */
static int repair_orphans_anthropic(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* For each orphaned call, find the user message that should contain its result
    * and add a tool_result block, or create a new user message */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue;

      /* Find the assistant message with this tool_use, then look for the next user msg */
      int found_call = 0;
      cJSON *target_user = NULL;
      cJSON *msg;
      cJSON_ArrayForEach(msg, messages)
      {
         if (!found_call)
         {
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
            if (!role || strcmp(role, "assistant") != 0)
               continue;
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (!content || !cJSON_IsArray(content))
               continue;
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               const char *bid = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
               if (type && strcmp(type, "tool_use") == 0 && bid &&
                   strcmp(bid, id_item->string) == 0)
               {
                  found_call = 1;
                  break;
               }
            }
         }
         else
         {
            /* Look for the next user message */
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
            if (role && strcmp(role, "user") == 0)
            {
               cJSON *content = cJSON_GetObjectItem(msg, "content");
               if (content && cJSON_IsArray(content))
               {
                  target_user = msg;
                  break;
               }
            }
         }
      }

      /* Create the tool_result block */
      cJSON *tr = cJSON_CreateObject();
      cJSON_AddStringToObject(tr, "type", "tool_result");
      cJSON_AddStringToObject(tr, "tool_use_id", id_item->string);
      cJSON_AddStringToObject(tr, "content", CANCEL_MSG);

      if (target_user)
      {
         /* Append to existing user message's content array */
         cJSON *content = cJSON_GetObjectItem(target_user, "content");
         cJSON_AddItemToArray(content, tr);
      }
      else
      {
         /* Create a new user message */
         cJSON *user_msg = cJSON_CreateObject();
         cJSON_AddStringToObject(user_msg, "role", "user");
         cJSON *content = cJSON_AddArrayToObject(user_msg, "content");
         cJSON_AddItemToArray(content, tr);
         cJSON_AddItemToArray(messages, user_msg);
      }
      repairs++;
   }

   /* Remove orphaned tool_result blocks */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next_msg = msg->next;
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (role && strcmp(role, "user") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block = content->child;
            while (block)
            {
               cJSON *next_block = block->next;
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_result") == 0)
               {
                  const char *tuid =
                      cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_use_id"));
                  if (tuid && !cJSON_GetObjectItem(call_ids, tuid))
                  {
                     cJSON_DetachItemViaPointer(content, block);
                     cJSON_Delete(block);
                     repairs++;
                  }
               }
               block = next_block;
            }
            /* If the user message is now empty, remove it */
            if (cJSON_GetArraySize(content) == 0)
            {
               cJSON_DetachItemViaPointer(messages, msg);
               cJSON_Delete(msg);
            }
         }
      }
      msg = next_msg;
   }

   return repairs;
}

/* Insert synthetic cancellation results for orphaned calls (Responses API format) */
static int repair_orphans_responses(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* Add function_call_output for orphaned function_calls */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue;

      cJSON *out_item = cJSON_CreateObject();
      cJSON_AddStringToObject(out_item, "type", "function_call_output");
      cJSON_AddStringToObject(out_item, "call_id", id_item->string);
      cJSON_AddStringToObject(out_item, "output", CANCEL_MSG);
      cJSON_AddItemToArray(messages, out_item);
      repairs++;
   }

   /* Remove orphaned function_call_output items */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next = msg->next;
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call_output") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid && !cJSON_GetObjectItem(call_ids, cid))
         {
            cJSON_DetachItemViaPointer(messages, msg);
            cJSON_Delete(msg);
            repairs++;
         }
      }
      msg = next;
   }

   return repairs;
}
int message_history_repair(cJSON *messages)
{
   if (!messages || !cJSON_IsArray(messages))
      return 0;

   if (cJSON_GetArraySize(messages) == 0)
      return 0;

   /* Ensure every persisted assistant tool_call has a valid JSON-string
    * arguments field. Strict providers (e.g. MiniMax) reject the whole
    * request otherwise. Cheap idempotent walk; runs every turn. */
   {
      cJSON *msg;
      cJSON_ArrayForEach(msg, messages)
      {
         const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
         if (role && strcmp(role, "assistant") == 0)
            tool_call_sanitize_assistant_arguments(msg);
      }
   }

   cJSON *call_ids = collect_tool_call_ids(messages);
   cJSON *result_ids = collect_tool_result_ids(messages);

   /* If there are no tool calls at all, nothing to repair */
   if (!call_ids->child && !result_ids->child)
   {
      cJSON_Delete(call_ids);
      cJSON_Delete(result_ids);
      return 0;
   }

   int fmt = detect_format(messages);
   int repairs;

   switch (fmt)
   {
   case 1:
      repairs = repair_orphans_anthropic(messages, call_ids, result_ids);
      break;
   case 2:
      repairs = repair_orphans_responses(messages, call_ids, result_ids);
      break;
   default:
      repairs = repair_orphans_openai(messages, call_ids, result_ids);
      break;
   }

   cJSON_Delete(call_ids);
   cJSON_Delete(result_ids);
   return repairs;
}

/* ================================================================
 * Gemini request building and response parsing
 *
 * Gemini uses a different wire format from OpenAI/Anthropic:
 *   - Role "assistant" becomes "model"
 *   - Tool calls:  parts[{functionCall: {name, args}}]
 *   - Tool results: role "user", parts[{functionResponse: {name, response}}]
 *   - System prompt: separate "systemInstruction" field
 *
 * These helpers are also used by delegate_gemini.c so the driver vtable
 * and the direct agent-loop code share the same implementation.
 * ================================================================ */

/* ================================================================
 * From: agent_http.c
 * ================================================================ */

/* agent_http.c: HTTP client platform abstraction + shared provider health tracking +
 * provider prompt cache lifecycle (Gemini cached-content).
 * HTTP implementation is in posix/agent_http.c (Linux/macOS) and
 * windows/agent_http.c (Windows). */

/* ================================================================
 * Provider health tracking (shared across platforms)
 * ================================================================ */

#define MAX_TRACKED_PROVIDERS 8
static struct
{
   char name[64];
   provider_health_t health;
} s_provider_health[MAX_TRACKED_PROVIDERS];
static int s_provider_health_count;

provider_err_class_t provider_classify_error(int http_status)
{
   if (http_status < 0)
      return PROVIDER_ERR_NETWORK;
   if (http_status == 401 || http_status == 403)
      return PROVIDER_ERR_AUTH;
   if (http_status == 429)
      return PROVIDER_ERR_RATE_LIMIT;
   if (http_status >= 500 && http_status < 600)
      return PROVIDER_ERR_SERVER;
   if (http_status >= 400 && http_status < 500)
      return PROVIDER_ERR_CLIENT;
   if (http_status >= 200 && http_status < 300)
      return PROVIDER_ERR_NONE;
   return PROVIDER_ERR_UNKNOWN;
}
const char *provider_error_message(provider_err_class_t cls)
{
   switch (cls)
   {
   case PROVIDER_ERR_NONE:
      return "ok";
   case PROVIDER_ERR_NETWORK:
      return "unreachable (connection failed or local HTTP client unavailable). Check network "
             "connection and server HTTP/SSL initialization.";
   case PROVIDER_ERR_AUTH:
      return "authentication failed. Check delegate provider credentials.";
   case PROVIDER_ERR_RATE_LIMIT:
      return "rate limited. Retry later.";
   case PROVIDER_ERR_SERVER:
      return "server error. Retry later.";
   case PROVIDER_ERR_CLIENT:
      return "client error. Check request parameters.";
   case PROVIDER_ERR_UNKNOWN:
      return "unknown error.";
   }
   return "unknown error.";
}
static provider_health_t *find_or_create_health(const char *provider_name)
{
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
         return &s_provider_health[i].health;
   }
   if (s_provider_health_count >= MAX_TRACKED_PROVIDERS)
      return &s_provider_health[0].health; /* overwrite first if full */
   int idx = s_provider_health_count++;
   snprintf(s_provider_health[idx].name, sizeof(s_provider_health[idx].name), "%s", provider_name);
   s_provider_health[idx].health.available = -1;
   s_provider_health[idx].health.last_http_status = -1;
   return &s_provider_health[idx].health;
}
void provider_health_update(const char *provider_name, int http_status)
{
   if (!provider_name || !provider_name[0])
      return;
   provider_health_t *h = find_or_create_health(provider_name);
   h->last_http_status = http_status;
   h->last_check_ms = (int64_t)time(NULL) * 1000;

   provider_err_class_t cls = provider_classify_error(http_status);
   if (cls == PROVIDER_ERR_NONE)
   {
      h->available = 1;
      h->error[0] = '\0';
   }
   else
   {
      h->available = 0;
      snprintf(h->error, sizeof(h->error), "%s", provider_error_message(cls));
   }
}
const provider_health_t *provider_health_get(const char *provider_name)
{
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
         return &s_provider_health[i].health;
   }
   return NULL;
}

/* ================================================================
 * From: agent_tunnel.c
 * ================================================================ */

/* agent_tunnel.c: reverse SSH tunnel lifecycle for NAT piercing */
/* Agent tunnel implementation is in posix/agent_tunnel.c (Linux/macOS)
 * and windows/agent_tunnel.c (Windows). */
