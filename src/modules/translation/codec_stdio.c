/* One codec owner across C and non-C callers; the pipe carries existing IR. */
#include <aimee/translation/codec_stdio.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define CODEC_FRAME_MAX (8U * 1024U * 1024U)

int aimee_model_codec_stdio(FILE *input, FILE *output)
{
   int status = 1;
   char *frame = malloc(CODEC_FRAME_MAX + 1U), *body = NULL, *encoded = NULL;
   cJSON *envelope = NULL, *reply = NULL;
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   if (!frame || !input || !output)
      goto done;
   size_t length = fread(frame, 1, CODEC_FRAME_MAX, input);
   if (ferror(input) || !length || fgetc(input) != EOF)
      goto done;
   frame[length] = 0;
   envelope = aimee_ir_json_parse_frame(frame, length);
   if (!cJSON_IsObject(envelope))
      goto done;
   const cJSON *entry;
   cJSON_ArrayForEach(
       entry, envelope) if (!entry->string ||
                            (strcmp(entry->string, "operation") && strcmp(entry->string, "wire") &&
                             strcmp(entry->string, "profile") && strcmp(entry->string, "request") &&
                             strcmp(entry->string, "body") &&
                             strcmp(entry->string, "disable_local_thinking"))) goto done;
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "operation"));
   const char *wire_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "wire"));
   const char *profile_name =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "profile"));
   aimee_wire_t wire = wire_name && !strcmp(wire_name, "anthropic")     ? AIMEE_WIRE_ANTHROPIC
                       : wire_name && !strcmp(wire_name, "openai_chat") ? AIMEE_WIRE_OPENAI_CHAT
                       : wire_name && !strcmp(wire_name, "responses")   ? AIMEE_WIRE_RESPONSES
                                                                        : AIMEE_WIRE_UNKNOWN;
   if (!operation || !wire || !profile_name ||
       (strcmp(profile_name, "api") && strcmp(profile_name, "codex")) ||
       (wire != AIMEE_WIRE_RESPONSES && strcmp(profile_name, "api")))
      goto done;
   reply = cJSON_CreateObject();
   if (!reply || !cJSON_AddStringToObject(reply, "status", "ok"))
      goto done;
   if (!strcmp(operation, "render"))
   {
      if (cJSON_GetObjectItemCaseSensitive(envelope, "body") ||
          aimee_ir_request_parse(cJSON_GetObjectItemCaseSensitive(envelope, "request"), &request))
         goto done;
      const cJSON *disable = cJSON_GetObjectItemCaseSensitive(envelope, "disable_local_thinking");
      if ((disable && !cJSON_IsBool(disable)) ||
          (cJSON_IsTrue(disable) && wire != AIMEE_WIRE_OPENAI_CHAT))
         goto done;
      body = wire == AIMEE_WIRE_OPENAI_CHAT
                 ? openai_backend_render_local(&request, cJSON_IsTrue(disable))
                 : aimee_backend_render_wire_profile(
                       &request, wire,
                       !strcmp(profile_name, "codex") ? AIMEE_RESPONSES_CODEX : AIMEE_RESPONSES_API,
                       NULL);
      if (!body || !cJSON_AddStringToObject(reply, "body", body))
         goto done;
   }
   else if (!strcmp(operation, "parse"))
   {
      const char *native = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "body"));
      char error[128];
      if (!native || cJSON_GetObjectItemCaseSensitive(envelope, "request") ||
          cJSON_GetObjectItemCaseSensitive(envelope, "disable_local_thinking") ||
          aimee_backend_parse_wire(native, strlen(native), wire, &response, error, sizeof error))
         goto done;
      cJSON *ir = aimee_ir_response_json(&response);
      if (!ir || !cJSON_AddItemToObject(reply, "response", ir))
      {
         cJSON_Delete(ir);
         goto done;
      }
   }
   else
      goto done;
   encoded = cJSON_PrintUnformatted(reply);
   if (encoded && strlen(encoded) <= CODEC_FRAME_MAX && fputs(encoded, output) >= 0 &&
       fputc('\n', output) != EOF && !fflush(output))
      status = 0;
done:
   if (status && output)
   {
      fputs("{\"status\":\"error\"}\n", output);
      fflush(output);
   }
   aimee_request_free(&request);
   aimee_response_free(&response);
   cJSON_Delete(envelope);
   cJSON_Delete(reply);
   free(frame);
   free(body);
   free(encoded);
   return status;
}
