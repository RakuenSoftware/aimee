/* Native request framing enters here. Raw bytes are an immutable replay sidecar;
 * typed fields remain the input to every transform and provider renderer. */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_native_object(const cJSON *native, aimee_wire_t wire, aimee_request_t *out,
                               char *error, size_t capacity)
{
   return wire == AIMEE_WIRE_ANTHROPIC     ? anthropic_frontend_parse(native, out, error, capacity)
          : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_frontend_parse(native, out, error, capacity)
          : wire == AIMEE_WIRE_RESPONSES   ? responses_frontend_parse(native, out, error, capacity)
                                           : -1;
}

int aimee_frontend_parse_wire(const char *body, size_t length, aimee_wire_t wire,
                              aimee_request_t *out, char *error, size_t error_capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!body || !length || length == SIZE_MAX)
      return -1;
   const char *end = NULL;
   cJSON *request = cJSON_ParseWithLengthOpts(body, length, &end, 0);
   if (request && end)
      while (end < body + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
         end++;
   if (!request || !end || end != body + length)
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
char *aimee_backend_render_wire(const aimee_request_t *request, aimee_wire_t wire)
{
   if (aimee_ir_request_admit(request))
      return NULL;
   int native_transport =
       wire != AIMEE_WIRE_RESPONSES ||
       (request->stream && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request->raw, "store")));
   if (native_transport && request->frontend == wire && request->raw_bytes && !request->mutated &&
       request->cache_policy == AIMEE_CACHE_DEFAULT)
   {
      aimee_request_t original = {0};
      char error[128];
      if (!aimee_frontend_parse_wire(request->raw_bytes, request->raw_length, wire, &original,
                                     error, sizeof error))
      {
         int same_model =
             (!request->model && !original.model) ||
             (request->model && original.model && !strcmp(request->model, original.model));
         int unchanged = same_model && aimee_ir_request_equal(request, &original) &&
                         cJSON_Compare(request->raw, original.raw, 1);
         aimee_request_free(&original);
         if (unchanged)
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
      if (aimee_frontend_parse_wire(frame, length, wire, &original, error, capacity))
      {
         aimee_request_free(out);
         return -1;
      }
      out->raw_bytes = original.raw_bytes;
      out->raw_length = original.raw_length;
      original.raw_bytes = NULL;
      original.raw_length = 0;
      aimee_request_free(&original);
   }
   return 0;
}

int aimee_backend_parse_wire(const char *frame, size_t length, aimee_wire_t wire,
                             aimee_response_t *out, char *error, size_t capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (error && capacity)
      error[0] = 0;
   if (!frame || !length || length == SIZE_MAX)
      return -1;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(frame, length, &end, 0);
   if (root && end)
      while (end < frame + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
         end++;
   if (!root && wire == AIMEE_WIRE_RESPONSES)
   {
      char *bounded = malloc(length + 1);
      if (!bounded)
         return -1;
      memcpy(bounded, frame, length);
      bounded[length] = 0;
      root = responses_backend_sse_object(bounded);
      free(bounded);
      end = root ? frame + length : NULL;
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
   if (shape && end == frame + length)
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
   report_blocks(request->system, request->n_system, wire, report);
   for (int i = 0; i < request->n_messages; i++)
      report_blocks(request->messages[i].blocks, request->messages[i].n_blocks, wire, report);
}
