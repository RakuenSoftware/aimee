/* economizer.h -- the public facade for what REMAINS of the economizer in C.
 *
 * Context reduction, folding, condensation policy and the proof planner all live
 * in server-go/modules/economizer now and are reached over the event bus;
 * economizer_module_client.h is the whole call surface.
 *
 * What stays here is the C-side seam plus the units that still have callers
 * OUTSIDE the economizer:
 *   - coord_closet    -- session_compact conserves identifiers with it
 *   - economizer_json -- agent_runtime compacts fresh tool results with it
 *   - fold_register   -- session_compact classifies turns with it
 *   - tool_condense   -- the tool_output_get recall handle and the stats endpoint
 *
 * Each of those is a separate cut-over rather than a leftover: they move when
 * their own callers do. Nothing here holds reduction policy any more.
 */
#ifndef DEC_ECONOMIZER_H
#define DEC_ECONOMIZER_H 1

#include "coord_closet.h"
#include "economizer_json.h"
#include "economizer_module_client.h"
#include "fold_register.h"
#include "tool_condense.h"

#endif /* DEC_ECONOMIZER_H */
