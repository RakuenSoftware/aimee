/* Native request framing enters here. Raw bytes are an immutable replay sidecar;
 * typed fields remain the input to every transform and provider renderer. */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local aimee_frontend_state_scope_t state_scope;
aimee_frontend_state_scope_t aimee_frontend_bind_state_guard(aimee_frontend_state_guard_fn guard,
                                                             void *context)
{
   aimee_frontend_state_scope_t previous = state_scope;
   state_scope = (aimee_frontend_state_scope_t){guard, context};
   return previous;
}
static _Thread_local aimee_frontend_state_scope_t request_owner_scope;
aimee_frontend_state_scope_t aimee_frontend_bind_request_owner(aimee_frontend_state_guard_fn owner,
                                                               void *context)
{
   aimee_frontend_state_scope_t previous = request_owner_scope;
   request_owner_scope = (aimee_frontend_state_scope_t){owner, context};
   return previous;
}
static int admit_owned_request(aimee_request_t *request, char *error, size_t capacity)
{
   if (request_owner_scope.guard &&
       (request_owner_scope.guard(request, request_owner_scope.context) ||
        aimee_ir_request_admit(request)))
   {
      aimee_request_free(request);
      if (error && capacity)
         snprintf(error, capacity, "canonical host request context unavailable");
      return -1;
   }
   return aimee_frontend_guard_state(request, error, capacity);
}
int aimee_frontend_guard_state(aimee_request_t *request, char *error, size_t capacity)
{
   if (!request)
      return -1;
   if (!state_scope.guard || !state_scope.guard(request, state_scope.context))
      return 0;
   aimee_request_free(request);
   if (error && capacity)
      snprintf(error, capacity, "native replay evidence unavailable");
   return -1;
}

/* Prepared profiles carry only translation-owned layout extensions. A native
 * request sidecar must never reintroduce SDK conversations or output selectors. */
int aimee_backend_host_profile_valid(const aimee_request_t *profile)
{
   if (!profile)
      return 0;
   static const char *const chat[] = {"model", "min_p",  "repeat_penalty", "chat_template_kwargs",
                                      "route", "models", "reasoning_split"};
   static const char *const anthropic[] = {"system"};
   const char *const *keys = NULL;
   size_t count = 0;
   if (profile->frontend == AIMEE_WIRE_OPENAI_CHAT)
   {
      keys = chat;
      count = sizeof chat / sizeof *chat;
   }
   else if (profile->frontend == AIMEE_WIRE_ANTHROPIC)
   {
      keys = anthropic;
      count = sizeof anthropic / sizeof *anthropic;
   }
   else if (profile->frontend != AIMEE_WIRE_RESPONSES)
      return 0;
   if (!profile->raw)
      return 1;
   if (!cJSON_IsObject(profile->raw) || !aimee_ir_json_unique(profile->raw))
      return 0;
   const cJSON *entry;
   cJSON_ArrayForEach(entry, profile->raw)
   {
      int allowed = 0;
      for (size_t i = 0; entry->string && i < count; i++)
         allowed |= !strcmp(entry->string, keys[i]);
      if (!allowed)
         return 0;
   }
   const cJSON *model = cJSON_GetObjectItemCaseSensitive(profile->raw, "model");
   if (model &&
       (!cJSON_IsString(model) || !profile->model || strcmp(model->valuestring, profile->model)))
      return 0;
   return 1;
}

int aimee_backend_preserve_host_extensions(const aimee_request_t *source,
                                           aimee_request_t *projected, aimee_wire_t wire)
{
   if (!source || !projected || source == projected)
      return -1;
   if (projected->raw || !source->raw || wire != AIMEE_WIRE_OPENAI_CHAT ||
       source->frontend != wire || projected->frontend != wire ||
       !aimee_backend_host_profile_valid(source) || !source->model || !projected->model ||
       strcmp(source->model, projected->model))
      return 0;
   projected->raw = cJSON_Duplicate(source->raw, 1);
   return projected->raw ? 0 : -1;
}

static int parse_native_object(const cJSON *native, aimee_wire_t wire, aimee_request_t *out,
                               char *error, size_t capacity)
{
   if (!aimee_ir_json_unique(native))
      return -1;
   return wire == AIMEE_WIRE_ANTHROPIC     ? anthropic_frontend_parse(native, out, error, capacity)
          : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_frontend_parse(native, out, error, capacity)
          : wire == AIMEE_WIRE_RESPONSES   ? responses_frontend_parse(native, out, error, capacity)
                                           : -1;
}

static int parse_wire(const char *body, size_t length, aimee_wire_t wire, aimee_request_t *out,
                      char *error, size_t error_capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!body || !length || length == SIZE_MAX)
      return -1;
   cJSON *request = aimee_ir_json_parse_frame(body, length);
   if (!request)
   {
      cJSON_Delete(request);
      if (error && error_capacity)
         snprintf(error, error_capacity, "invalid native request framing");
      return -1;
   }
   int rc = parse_native_object(request, wire, out, error, error_capacity);
   cJSON_Delete(request);
   if (rc)
      return rc;
   out->raw_bytes = malloc(length + 1);
   if (!out->raw_bytes)
   {
      aimee_request_free(out);
      return -1;
   }
   memcpy(out->raw_bytes, body, length);
   out->raw_bytes[length] = '\0';
   out->raw_length = length;
   return 0;
}
int aimee_frontend_parse_wire(const char *body, size_t length, aimee_wire_t wire,
                              aimee_request_t *out, char *error, size_t error_capacity)
{
   int rc = parse_wire(body, length, wire, out, error, error_capacity);
   if (rc)
      return rc;
   return admit_owned_request(out, error, error_capacity);
}
char *aimee_backend_render_wire(const aimee_request_t *request, aimee_wire_t wire)
{
   if (aimee_ir_request_admit(request))
      return NULL;
   if (aimee_ir_request_needs_model_projection(request))
   {
      aimee_request_t projected = {0};
      if (aimee_ir_request_project_model(request, &projected))
         return NULL;
      if (aimee_backend_preserve_host_extensions(request, &projected, wire))
      {
         aimee_request_free(&projected);
         return NULL;
      }
      char *body = aimee_backend_render_wire(&projected, wire);
      aimee_request_free(&projected);
      return body;
   }
   int native_transport =
       wire != AIMEE_WIRE_RESPONSES ||
       (request->stream && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request->raw, "store")));
   if (native_transport && request->frontend == wire && request->raw_bytes &&
       request->cache_policy == AIMEE_CACHE_DEFAULT)
   {
      aimee_request_t original = {0};
      char error[128];
      if (!parse_wire(request->raw_bytes, request->raw_length, wire, &original, error,
                      sizeof error))
      {
         int same_model =
             (!request->model && !original.model) ||
             (request->model && original.model && !strcmp(request->model, original.model));
         int unchanged = same_model && aimee_ir_request_wire_equal(request, &original) &&
                         cJSON_Compare(request->raw, original.raw, 1);
         aimee_request_free(&original);
         if (unchanged &&
             (wire != AIMEE_WIRE_ANTHROPIC || aimee_anthropic_replay_valid(request, request->raw)))
         {
            char *copy = malloc(request->raw_length + 1);
            if (copy)
               memcpy(copy, request->raw_bytes, request->raw_length + 1);
            return copy;
         }
      }
      else
         aimee_request_free(&original);
   }
   cJSON *native = wire == AIMEE_WIRE_ANTHROPIC     ? anthropic_backend_build(request)
                   : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_backend_build(request)
                   : wire == AIMEE_WIRE_RESPONSES   ? responses_backend_build(request)
                                                    : NULL;
   char *bytes = native ? cJSON_PrintUnformatted(native) : NULL;
   cJSON_Delete(native);
   return bytes;
}

int aimee_frontend_parse_native_frame(const cJSON *native, const char *frame, size_t length,
                                      aimee_wire_t wire, aimee_request_t *out, char *error,
                                      size_t capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (parse_native_object(native, wire, out, error, capacity))
      return -1;
   if (frame)
   {
      aimee_request_t original = {0};
      if (parse_wire(frame, length, wire, &original, error, capacity))
      {
         aimee_request_free(out);
         return -1;
      }
      if (wire == AIMEE_WIRE_ANTHROPIC)
      {
         int mi = 0, bi = 0;
         for (int i = 0; i < out->n_messages; i++)
            for (int j = 0; j < out->messages[i].n_blocks; j++)
            {
               aimee_block_t *block = &out->messages[i].blocks[j];
               if (block->type != AIMEE_BLK_THINKING)
                  continue;
               const aimee_block_t *source = NULL;
               while (mi < original.n_messages)
               {
                  if (bi == original.messages[mi].n_blocks)
                  {
                     mi++;
                     bi = 0;
                     continue;
                  }
                  const aimee_block_t *candidate = &original.messages[mi].blocks[bi++];
                  if (candidate->type == AIMEE_BLK_THINKING)
                  {
                     source = candidate;
                     break;
                  }
               }
               aimee_block_t current = *block,
                             original_block = source ? *source : (aimee_block_t){0};
               /* Cache marker edits are excluded from the signed prefix. */
               current.cache_control = current.cache_ttl = NULL;
               original_block.cache_control = original_block.cache_ttl = NULL;
               aimee_request_t a = {.system = &current, .n_system = 1};
               aimee_request_t b = {.system = &original_block, .n_system = source ? 1 : 0};
               char *binding =
                   source && source->thinking_binding && aimee_ir_request_wire_equal(&a, &b)
                       ? strdup(source->thinking_binding)
                       : NULL;
               if (source && source->thinking_binding && aimee_ir_request_wire_equal(&a, &b) &&
                   !binding)
               {
                  aimee_request_free(&original);
                  aimee_request_free(out);
                  return -1;
               }
               free(block->thinking_binding);
               block->thinking_binding = binding;
            }
      }
      out->raw_bytes = original.raw_bytes;
      out->raw_length = original.raw_length;
      original.raw_bytes = NULL;
      original.raw_length = 0;
      aimee_request_free(&original);
   }
   return admit_owned_request(out, error, capacity);
}

int aimee_backend_parse_wire(const char *frame, size_t length, aimee_wire_t wire,
                             aimee_response_t *out, char *error, size_t capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (error && capacity)
      error[0] = 0;
   if (!frame || !length || length == SIZE_MAX || memchr(frame, 0, length))
   {
      if (error && capacity)
         snprintf(error, capacity, "invalid native provider response frame");
      return -1;
   }
   cJSON *root = aimee_ir_json_parse_frame(frame, length);
   if (!root && wire == AIMEE_WIRE_RESPONSES)
   {
      char *bounded = malloc(length + 1);
      if (!bounded)
         return -1;
      memcpy(bounded, frame, length);
      bounded[length] = 0;
      root = responses_backend_sse_object(bounded);
      free(bounded);
   }
   int shape = cJSON_IsObject(root);
   const cJSON *failure = cJSON_GetObjectItemCaseSensitive(root, "error");
   if (!failure || cJSON_IsNull(failure))
   {
      if (wire == AIMEE_WIRE_ANTHROPIC)
      {
         const char *stop =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "stop_reason"));
         shape = shape && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "content")) &&
                 stop && stop[0];
      }
      else if (wire == AIMEE_WIRE_OPENAI_CHAT)
      {
         const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
         const cJSON *choice = cJSON_GetArrayItem(choices, 0);
         const char *stop =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));
         shape = shape && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(choice, "message")) &&
                 stop && stop[0];
      }
      else if (wire == AIMEE_WIRE_RESPONSES)
      {
         const char *status =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "status"));
         shape = shape && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "output")) &&
                 status &&
                 (!strcmp(status, "completed") || !strcmp(status, "incomplete") ||
                  !strcmp(status, "failed") || !strcmp(status, "cancelled"));
      }
   }
   int rc = -1;
   if (shape && aimee_ir_json_unique(root))
      rc = wire == AIMEE_WIRE_ANTHROPIC     ? anthropic_backend_parse(root, out, error, capacity)
           : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_backend_parse(root, out, error, capacity)
           : wire == AIMEE_WIRE_RESPONSES   ? responses_backend_parse(root, out, error, capacity)
                                            : -1;
   cJSON_Delete(root);
   if (rc || aimee_ir_response_admit(out))
   {
      if (error && capacity && !error[0])
         snprintf(error, capacity, "invalid native provider response");
      aimee_response_free(out);
      return -1;
   }
   return 0;
}

static void report_blocks(const aimee_block_t *blocks, int count, aimee_wire_t wire,
                          aimee_wire_projection_report_t *report)
{
   for (int i = 0; i < count; i++)
   {
      const aimee_block_t *block = &blocks[i];
      if (block->type != AIMEE_BLK_THINKING)
         continue;
      int compatible = wire == AIMEE_WIRE_ANTHROPIC   ? anthropic_ir_block_replayable(block)
                       : wire == AIMEE_WIRE_RESPONSES ? block->thinking_id && block->thinking_id[0]
                                                      : 0;
      if (!compatible)
      {
         report->omitted_reasoning_blocks++;
         if (block->thinking_encrypted_content || block->thinking_signature ||
             block->thinking_redacted)
            report->omitted_opaque_state_blocks++;
      }
   }
}
void aimee_backend_projection_report(const aimee_request_t *request, aimee_wire_t wire,
                                     aimee_wire_projection_report_t *report)
{
   if (!report)
      return;
   memset(report, 0, sizeof *report);
   if (!request)
      return;
   if (wire == AIMEE_WIRE_ANTHROPIC)
   {
      aimee_anthropic_projection_report(request, report);
      return;
   }
   report_blocks(request->system, request->n_system, wire, report);
   for (int i = 0; i < request->n_messages; i++)
      report_blocks(request->messages[i].blocks, request->messages[i].n_blocks, wire, report);
}
