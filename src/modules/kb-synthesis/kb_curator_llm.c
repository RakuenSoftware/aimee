/* kb_curator_llm.c: curator stage -> LLM dispatch. See header. */

#include "kb_curator_llm.h"

#include "cJSON.h"
#include "provider_client.h"

#include <stdio.h>
#include <stdlib.h>

/* THE CEILING HAS TO CLEAR THE SLOWEST SIDECAR WE SHIP, and at 300000 it did not.
 *
 * The history below already records 120s being shorter than a normal extraction on
 * the bundled CPU model. 300s is the same mistake one size up, and E4B is where it
 * shows: measured on the deployed aimee-llm-e4b sidecar, one realistic document
 * paragraph costs 1768 completion tokens at 4.98 tokens/s -- 375 seconds, with
 * multi-token prediction already accepting 78% of drafts. Every non-trivial
 * extraction on E4B was cut off by this deadline, retried, and failed for good,
 * while E2B (1087 tokens at ~10 tokens/s, 118s) fit and looked fine.
 *
 * 600s, and the ceiling is set by the STALE-LEASE REAPER, not by the token budget.
 * CE_STALE_LEASE / CCU_STALE_LEASE reclaim a job whose lease is older than 15
 * minutes, so a deadline at or near 900s would let the reaper hand a still-running
 * document to a second worker and pay for the same extraction twice. 600s sits
 * clearly inside that window and clears the measured 375s by 60%.
 *
 * That leaves a gap worth naming: 4096 tokens at E4B's 4.98 tokens/s is ~825s, so a
 * document that reasons all the way to the cap still exceeds this. That direction is
 * survivable and the other is not -- a deadline overrun is a transport error, which
 * kb_curator_error_is_provider_unavailable retries with backoff, while truncating at
 * the token cap yields invalid JSON and fails permanently. Raising both the deadline
 * and the lease windows together is the way to close it, and it needs the reaper
 * thought about rather than a constant nudged.
 *
 * None of this lets a dead peer hold a worker for ten minutes. agent_http keeps
 * SO_RCVTIMEO at min(timeout, 30s) per recv, so a peer that stops answering fails in
 * 30s; this wall-clock ceiling only extends while tokens are still arriving -- that
 * is, while the model is genuinely working. */
#define KB_CURATOR_PROVIDER_TIMEOUT_MS 600000

int kb_curator_llm_run(kb_curator_stage_t stage, const aimee_request_t *request,
                       aimee_response_t *out, char *errbuf, size_t errlen)
{
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!out || aimee_ir_request_admit(request))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "invalid canonical curator request");
      return -1;
   }
   provider_def_owned_t prov;
   if (!kb_curator_provider_for_stage(stage, &prov))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "no synthesis endpoint configured; configure canonical curator provider or "
                  "SYNTHESIS_ENDPOINT; command model fallbacks are unsupported");
      kb_curator_provider_backoff_note();
      return -1;
   }
   provider_def_t *def = &prov.def;
   if (def->max_tokens <= 0 && !request->has_max_tokens &&
       config_kb_curator_extract_max_tokens() > 0)
      def->max_tokens = config_kb_curator_extract_max_tokens();
   if (def->timeout_ms <= 0)
      def->timeout_ms = KB_CURATOR_PROVIDER_TIMEOUT_MS;
   if (def->max_attempts <= 0)
      def->max_attempts = 1;
   provider_completion_t completion = {0};
   int rc = provider_client_complete_ir(def, request, &completion, errbuf, errlen);
   aimee_response_free(out);
   *out = completion.ir;
   completion.ir = (aimee_response_t){0};
   provider_completion_free(&completion);
   if (rc)
   {
      if (kb_curator_error_is_provider_unavailable(errbuf))
         kb_curator_provider_backoff_note();
      else
         kb_curator_provider_backoff_recovered();
      return -1;
   }
   /* Do not declare a partial/tool-only response recovered just because HTTP
    * completed. The owned model response is preserved for the caller. */
   char *text = kb_curator_llm_text(out, errbuf, errlen);
   if (!text)
      return -1;
   free(text);
   kb_curator_provider_backoff_recovered();
   return 0;
}
