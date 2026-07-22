/* test_aimee_ir_shadow.c -- the request-side shadow observer: a gated no-op when
 * AIMEE_IR_SHADOW is unset, and a BYTE-exact same-protocol parity check when
 * enabled. A request the Anthropic backend reproduces byte-for-byte counts as
 * REBUILD_MATCH; one it cannot (an unmodeled field it drops) counts as
 * REBUILD_MISMATCH. This is the caching gate for retiring the verbatim passthrough. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "aimee_ir_metrics.h"
#include "aimee_ir_shadow.h"
#include "cJSON.h"

int main(void)
{
   printf("ir-shadow: ");
   aimee_ir_metrics_reset();

   /* A request already in the Anthropic backend's canonical key order, so
    * serialize(backend_build(parse(req))) == serialize(req): a byte-faithful
    * round-trip. */
   cJSON *req = cJSON_Parse(
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
       "\"system\":[{\"type\":\"text\",\"text\":\"sys\"}],"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
       "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]}");
   assert(req);

   /* disabled (env unset) -> pure no-op */
   unsetenv("AIMEE_IR_SHADOW");
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 0);

   /* enabled -> the observer parses (IR_PATH) and runs FIELD-COVERAGE detection: a
    * request using only IR-modeled top-level keys scores REBUILD_MATCH; one carrying an
    * unmodeled top-level key scores REBUILD_MISMATCH (the canonical egress would drop
    * it). The byte-parity gate was retired with the sidecar; this replaces it with the
    * detection that actually matters post-retirement -- silent field drop. */
   setenv("AIMEE_IR_SHADOW", "1", 1);
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 1);
   assert(aimee_ir_metric_total(AIMEE_IR_M_PARSE_FAIL) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MATCH) == 1);    /* all keys modeled */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MISMATCH) == 0); /* no field dropped */

   /* a non-Anthropic frontend is skipped in this observer */
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_OPENAI_CHAT);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 1); /* unchanged */

   /* modeled fields (incl. top_p, now modeled) still score MATCH */
   cJSON *modeled = cJSON_Parse(
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
       "\"top_p\":0.9}");
   assert(modeled);
   aimee_ir_shadow_observe_request(modeled, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 2);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MATCH) == 2);
   cJSON_Delete(modeled);

   /* an UNMODELED top-level field (a future Anthropic key the IR does not model) is
    * flagged: the canonical egress would silently drop it. */
   cJSON *unmodeled = cJSON_Parse(
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
       "\"future_knob\":true}");
   assert(unmodeled);
   aimee_ir_shadow_observe_request(unmodeled, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MISMATCH) == 1); /* field-drop detected */
   cJSON_Delete(unmodeled);

   /* NULL request is safe */
   aimee_ir_shadow_observe_request(NULL, AIMEE_WIRE_ANTHROPIC);

   cJSON_Delete(req);
   printf("ok\n");
   return 0;
}
