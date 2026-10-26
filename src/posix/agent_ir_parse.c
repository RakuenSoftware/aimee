/* Native response entry returns owned canonical IR. Tool-call rescue belongs
 * to this boundary and runs once on typed TEXT. No flat response projection. */
#include "aimee.h"

#include "agent_protocol.h"
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/aimee_ir.h>
#include <aimee/ir/session_event.h>
#include <aimee/ir/aimee_ir_metrics.h>
#include <aimee/delegates/aimee_ir_rescue.h>
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static void ir_observe_reasoning(const aimee_response_t *ir);

int agent_ir_decode_json_response_bound(const cJSON *root, int anthropic, int rescue_mode,
                                        int *n_rescued, const aimee_anthropic_replay_t *request,
                                        aimee_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;
   char err[128] = "";
   if (!aimee_ir_json_unique(root))
      goto fail;
   int rc = anthropic ? anthropic_backend_parse(root, out, err, sizeof(err))
                      : openai_backend_parse(root, out, err, sizeof(err));
   if (rc != 0)
      goto fail;
   if (anthropic && request && aimee_anthropic_bind_response(request, out))
      goto fail;
   int rescued = rescue_mode >= 0 ? aimee_ir_rescue_tool_calls(out, rescue_mode) : 0;
   if (rescued < 0 || aimee_ir_response_admit(out) != 0)
      goto fail;
   if (n_rescued)
      *n_rescued = rescued;
   ir_observe_reasoning(out);
   if (aimee_ir_session_emit_buffered_response(out, NULL) != 0)
   {
      aimee_response_free(out);
      return AGENT_IR_CAPTURE_FAILED;
   }
   return 0;
fail:
   aimee_response_free(out);
   aimee_ir_session_fail_capture();
   return -1;
}

int agent_ir_decode_json_response(const cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                  aimee_response_t *out)
{
   return agent_ir_decode_json_response_bound(root, anthropic, rescue_mode, n_rescued, NULL, out);
}

/* Cap matching the relay tap's: reasoning is only ever MATCHED against, never
 * replayed, so a bound costs recall of late thought where an unbounded copy would
 * scale with the model's chain of thought. */
#define IR_REASONING_OBSERVE_MAX 16384

/* Observation-only reasoning tap for the BUFFERED wires, the counterpart of the
 * streaming relay's. It sits at the shared response entry boundary every
 * JSON-wire response parse passes through -- anthropic, openai-chat and responses
 * alike -- so one call covers all three without naming a provider. The rule is the
 * same neutral one the streaming tap uses: read AIMEE_BLK_THINKING off the IR.
 *
 * Execution and persistence retain the reasoning blocks. This tap only observes
 * them; it never changes or substitutes for the canonical content.
 *
 * Truncation is FLAGGED, and a reasoning block whose text did not survive parsing
 * (an empty thought) counts as incomplete rather than observed -- a block that is
 * present but empty must never be mistaken for a thought something can act on. */
static void ir_observe_reasoning(const aimee_response_t *ir)
{
   int saw_block = 0;
   for (int i = 0; i < ir->n_content; i++)
      if (ir->content[i].type == AIMEE_BLK_THINKING)
         saw_block = 1;
   if (!saw_block)
      return;

   char buf[IR_REASONING_OBSERVE_MAX];
   size_t n = aimee_ir_response_reasoning(ir, buf, sizeof buf);
   if (n > 0)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_UNKNOWN);
   /* n == 0 with a THINKING block present means the block carried no text: the
    * reasoning was dropped somewhere upstream of here, which is exactly the failure
    * that must not read as "no reasoning in this turn". */
   if (n == 0 || n >= sizeof buf - 1)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_INCOMPLETE, AIMEE_WIRE_UNKNOWN);
}

int agent_ir_decode_responses(const char *body, int rescue_mode, int *n_rescued,
                              aimee_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;
   char error[128];
   int rc = responses_backend_decode_body(body, out, error, sizeof error);
   if (rc != 0)
      goto fail;
   int rescued = rescue_mode >= 0 ? aimee_ir_rescue_tool_calls(out, rescue_mode) : 0;
   if (rescued < 0 || aimee_ir_response_admit(out) != 0)
      goto fail;
   if (n_rescued)
      *n_rescued = rescued;
   ir_observe_reasoning(out);
   if (aimee_ir_session_emit_buffered_response(out, NULL) != 0)
   {
      aimee_response_free(out);
      return AGENT_IR_CAPTURE_FAILED;
   }
   return 0;
fail:
   aimee_response_free(out);
   aimee_ir_session_fail_capture();
   return -1;
}
