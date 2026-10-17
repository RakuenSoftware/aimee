/* Native request framing enters here. Raw bytes are an immutable replay sidecar;
 * typed fields remain the input to every transform and provider renderer. */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   int rc =
       wire == AIMEE_WIRE_ANTHROPIC ? anthropic_frontend_parse(request, out, error, error_capacity)
       : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_frontend_parse(request, out, error, error_capacity)
       : wire == AIMEE_WIRE_RESPONSES
           ? responses_frontend_parse(request, out, error, error_capacity)
           : -1;
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
   if (!request)
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
