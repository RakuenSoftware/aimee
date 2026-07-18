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

   /* enabled -> byte-faithful request scores a MATCH, no mismatch */
   setenv("AIMEE_IR_SHADOW", "1", 1);
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 1);
   assert(aimee_ir_metric_total(AIMEE_IR_M_PARSE_FAIL) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_BACKEND_BUILD_FAIL) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MATCH) == 1);    /* byte-exact */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MISMATCH) == 0); /* clean */

   /* a non-Anthropic frontend is skipped in this slice */
   aimee_ir_shadow_observe_request(req, AIMEE_WIRE_OPENAI_CHAT);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 1); /* unchanged */

   /* a request carrying a field the IR does not model (top_p) parses fine but the
    * backend drops it on rebuild -> the bytes differ -> REBUILD_MISMATCH. This is the
    * exact class (silent field loss) the byte gate must catch before we can retire
    * the passthrough. */
   cJSON *lossy = cJSON_Parse(
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
       "\"top_p\":0.9}");
   assert(lossy);
   aimee_ir_shadow_observe_request(lossy, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 2);          /* parsed + observed */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MISMATCH) == 1); /* dropped top_p */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MATCH) == 1);    /* still just the first */
   cJSON_Delete(lossy);

   /* NULL request is safe */
   aimee_ir_shadow_observe_request(NULL, AIMEE_WIRE_ANTHROPIC);

   cJSON_Delete(req);
   printf("ok\n");
   return 0;
}
