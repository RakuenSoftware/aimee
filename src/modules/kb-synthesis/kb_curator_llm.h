/* Curator model execution owns canonical requests/responses. Business JSON is
 * projected only after terminal and content-type validation. */
#ifndef KB_CURATOR_LLM_H
#define KB_CURATOR_LLM_H
#include "kb_curator_provider.h"
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C"
{
#endif
   /* out is zero-initialized or already owns IR. Failure preserves existing out. */
   int kb_curator_llm_request_init(aimee_request_t *out, const char *system_prompt,
                                   const char *job_json, const struct cJSON *json_schema);
   /* Requires admitted canonical input before provider resolution. out retains full
    * model metadata, including failed/refused responses when available. No command
    * model fallback: configure a canonical provider instead. */
   int kb_curator_llm_run(kb_curator_stage_t stage, const aimee_request_t *request,
                          aimee_response_t *out, char *errbuf, size_t errlen);
   /* Explicit business-artifact projection: TEXT only from a complete successful
    * turn; THINKING stays in owned IR and is never used as an answer. Caller frees. */
   char *kb_curator_llm_text(const aimee_response_t *response, char *errbuf, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
