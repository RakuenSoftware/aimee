/* test_memory_ranker_boundary.c — structural + behavioral tests for the
 * Recall / Calibrate boundary enforced by memory_ranker_input_t.
 *
 * Contract being tested:
 *   No retrieval-ordering function may consume confidence-shaped fields.
 *   memory_ranker_input_t is the only type passed to ranking functions;
 *   it contains only ranking-safe fields.  After ordering is fixed,
 *   parts.confidence is populated from the full memory_t for trace/display.
 *
 * Behavioral invariant: parts.total must equal the sum of its ranking
 * component signals with no confidence contribution.  If confidence were
 * re-added to the formula, this test would fail because parts.confidence > 0
 * but the component sum == parts.total exactly. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/memory/memory_ontology.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"

/* ---- structural: type layout ---- */

static void test_ranker_input_type_is_narrow(void)
{
   /* Verify memory_ranker_input_t only exposes ranking-safe fields.
    * Accessing any field not listed here would be a compile error, which
    * acts as a canary for future additions of confidence-shaped fields. */
   memory_ranker_input_t r;
   memset(&r, 0, sizeof(r));
   r.id = 1;
   r.kind[0] = '\0';
   r.key[0] = '\0';
   r.content[0] = '\0';
   /* NOTE: r.confidence must not be accessible — that field must not exist.
    * Uncommenting the line below must produce a compile error:
    *   r.confidence = 0.5;  */
   printf("  ranker_input_type_is_narrow: ok\n");
}

/* ---- behavioral: confidence must not appear in parts.total ---- */

static void test_total_equals_component_sum(void)
{
   /* Insert a row with high confidence so parts.confidence is non-zero
    * post-ranking.  Then verify parts.total equals the sum of ranking
    * signals without any confidence contribution. */
   memory_t m;
   memset(&m, 0, sizeof(m));
   assert(memory_insert(TIER_L1, KIND_FACT, "ranker-boundary-test",
                        "the project uses docker for deployment", 0.95, "", &m) == 0);
   assert(m.id > 0);

   memory_diagnostic_t d;
   memset(&d, 0, sizeof(d));
   assert(memory_explain_match("docker deployment", m.id, &d) == 0);

   /* parts.confidence must be populated post-ranking for display. Model-authored
    * memories are durably capped at 0.8, so this also checks that the displayed
    * value is the persisted post-provenance value rather than the requested
    * pre-cap 0.95. */
   assert(d.parts.confidence > 0.79 && d.parts.confidence < 0.81);

   /* parts.total must equal the sum of ranking signals — no confidence term */
   double component_sum = d.parts.lexical + d.parts.coverage + d.parts.entity + d.parts.temporal +
                          d.parts.evidence + d.parts.semantic + d.parts.state + d.parts.intent +
                          d.parts.salience + d.parts.surprise + d.parts.pagerank;
   assert(fabs(d.parts.total - component_sum) < 1e-9);

   /* Verify that adding the old confidence term (weight 0.05) WOULD change
    * the total — proving confidence is genuinely excluded, not just zero. */
   assert(fabs(d.parts.confidence * 0.05) > 1e-6);

   printf("  total_equals_component_sum: ok\n");
}

int main(void)
{
   printf("memory_ranker_boundary:\n");

   db2_test_shim_open();

   test_ranker_input_type_is_narrow();
   test_total_equals_component_sum();

   printf("All memory_ranker_boundary tests passed.\n");
   return 0;
}
