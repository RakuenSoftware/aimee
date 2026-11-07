/* Pure canonical curator request assembly and business-text projection. */
#include "kb_curator_llm.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>

int kb_curator_llm_request_init(aimee_request_t *out, const char *system_prompt,
                                const char *job_json, const cJSON *json_schema)
{
   if (!out || !job_json)
      return -1;
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = (char *)system_prompt};
   aimee_block_t input = {.type = AIMEE_BLK_TEXT, .text = (char *)job_json};
   aimee_ir_block_set_context(&system, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
   aimee_ir_block_set_context(&input, AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_message_t message = {.role = "user", .blocks = &input, .n_blocks = 1};
   aimee_request_t request = {.system = &system,
                              .n_system = system_prompt && system_prompt[0] ? 1 : 0,
                              .messages = &message,
                              .n_messages = 1};
   if (json_schema)
      request.output_format = (aimee_output_format_t){.kind = AIMEE_OUTPUT_JSON_SCHEMA,
                                                      .name = "curator_output",
                                                      .schema = (cJSON *)json_schema,
                                                      .strict = AIMEE_STRICT_TRUE};
   return aimee_ir_request_clone(&request, out);
}

char *kb_curator_llm_text(const aimee_response_t *response, char *errbuf, size_t errlen)
{
   if (aimee_ir_response_complete_text_valid(response))
      goto invalid;
   char *text = aimee_ir_response_text_alloc(response);
   if (text && text[0])
      return text;
   free(text);
invalid:
   if (errbuf && errlen)
      snprintf(errbuf, errlen, "curator requires a complete canonical text response");
   return NULL;
}
