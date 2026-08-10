/* economizer.h -- the single public facade for the context-economizer module
 * (src/modules/economizer/). The economizer is the unified context-reduction subsystem:
 *   - the reduce orchestrator (context_reduce),
 *   - history folding (context_fold + fold_budget / fold_register / fold_recall + coord_closet
 *     + episode_seal + task_rail),
 *   - tool-output condensation (tool_condense).
 * External callers include ONLY this header; the individual sub-headers are module-internal
 * (they remain on the -Imodules/economizer path so the module's own .c files cross-include them,
 * but new external code should depend on economizer.h, not the sub-headers). */
#ifndef DEC_ECONOMIZER_H
#define DEC_ECONOMIZER_H 1

#include "economizer_proof.h" /* provider-specific cost-proof gate; empty live registry */
#include "economizer_json.h"  /* strict fresh-tool-result JSON compaction */
#include "context_reduce.h"   /* context_reduce(), reduce_config_t / reduce_result_t / seams */
#include "context_fold.h"     /* context_fold_view / context_compress_view, fold_config_t */
#include "tool_condense.h"    /* tool_condense_apply / _recall / _enabled, family parsers */
#include "fold_register.h" /* fold_register_parse / _label: settled-vs-transient turn classes */

#endif /* DEC_ECONOMIZER_H */
