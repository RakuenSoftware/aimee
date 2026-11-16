/* Cross-language oracle: execute the production C serializer/parser, not a
 * copied Go schema. stdin is a candidate; no stdin emits the rich C fixture. */
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define main request_codec_fixture_main
#include "../../../src/tests/test_ir_request_codec.c"
#undef main
int main(int argc, char **argv)
{
   if (argc > 1 && (!strcmp(argv[1], "request") || !strcmp(argv[1], "request-fixture")))
   {
      cJSON *input = NULL;
      if (!strcmp(argv[1], "request-fixture")) input = fixture();
      else {
         char *bytes = malloc(1048577); if (!bytes) return 2;
         size_t n=fread(bytes,1,1048576,stdin);bytes[n]=0;
         input=aimee_ir_json_parse_frame(bytes,n);free(bytes);
      }
      aimee_request_t request={0};
      if (!input || aimee_ir_request_parse(input,&request)) {cJSON_Delete(input);return 3;}
      cJSON_Delete(input);
      cJSON *wire=aimee_ir_request_json(&request);aimee_request_free(&request);
      char *bytes=wire?cJSON_PrintUnformatted(wire):NULL;cJSON_Delete(wire);
      if (!bytes)return 4;puts(bytes);free(bytes);return 0;
   }
   aimee_response_t decoded = {0};
   cJSON *value = NULL;
   if (argc > 1 && (!strcmp(argv[1], "parse") || !strcmp(argv[1], "text") || !strcmp(argv[1], "completion"))) {
      char *bytes = malloc(1048577); if (!bytes) return 2;
      size_t n = fread(bytes, 1, 1048576, stdin); bytes[n] = 0;
      value = cJSON_ParseWithLength(bytes, n + 1); free(bytes);
      if (!value || aimee_ir_response_parse(value, &decoded)) { cJSON_Delete(value); return 3; }
      cJSON_Delete(value);
      if (!strcmp(argv[1], "completion")) {
         printf("{\"complete\":%s,\"complete_text\":%s}\n",
                !aimee_ir_response_complete_valid(&decoded) ? "true" : "false",
                !aimee_ir_response_complete_text_valid(&decoded) ? "true" : "false");
         aimee_response_free(&decoded); return 0;
      }
      if (!strcmp(argv[1], "text")) {
         char *text = aimee_ir_response_text_alloc(&decoded); aimee_response_free(&decoded);
         if (!text) return 6; fputs(text, stdout); free(text); return 0;
      }
      value = aimee_ir_response_json(&decoded); aimee_response_free(&decoded);
   } else {
      cJSON *args=cJSON_Parse("{\"exact\":0.123456789012345678901234567890}");
      cJSON *sidecar=cJSON_Parse("{\"provider_future\":[1,\"opaque\"]}");
      aimee_block_t blocks[] = {
         {.type=AIMEE_BLK_THINKING,.text="reason",.thinking_signature="signature",
          .thinking_id="reason-id",.thinking_encrypted_content="encrypted",.thinking_redacted="redacted",.message_group=2,
          .context={.origin=AIMEE_CTX_ORIGIN_MODEL,.authority=AIMEE_CTX_AUTH_EVIDENCE,.trust=AIMEE_CTX_TRUST_UNVERIFIED,
                    .sensitivity=AIMEE_CTX_SENS_INTERNAL,.model_visible=1,.revision_domain="source",.revision_scope="turn",.revision_epoch=18446744073709551615ULL}},
         {.type=AIMEE_BLK_TOOL_USE,.tool_id="call",.tool_name="read",.tool_namespace="aimee",.tool_input=args,.raw=sidecar},
         {.type=AIMEE_BLK_TEXT,.text="final",.cache_control="ephemeral",.cache_ttl="5m"},
         {.type=AIMEE_BLK_IMAGE,.media_type="image/png",.media_ref="data:image/png;base64,AAAA"},
         {.type=AIMEE_BLK_DOCUMENT,.media_type="application/pdf",.media_ref="fixture.pdf"}
      };
      aimee_refusal_details_t refusal={.category="future-category",.explanation=NULL};
      aimee_response_t source={.role="assistant",.id="id",.model="model",.content=blocks,.n_content=5,
         .stop_reason=AIMEE_STOP_CONTENT_FILTER,.raw_stop_reason="refusal",.refusal=&refusal,
         .usage_in=9223372036854775807L,.usage_out=7,.usage_cache_read=6,.usage_cache_write=5,.usage_reasoning=4,.raw=sidecar};
      value=aimee_ir_response_json(&source); cJSON_Delete(args);cJSON_Delete(sidecar);
   }
   if(!value)return 4; char *bytes=cJSON_PrintUnformatted(value);cJSON_Delete(value);
   if(!bytes)return 5; puts(bytes);free(bytes);return 0;
}
