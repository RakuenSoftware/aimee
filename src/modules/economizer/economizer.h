/* economizer.h -- the public facade for what REMAINS of the economizer in C.
 *
 * Context reduction itself now lives in server-go/modules/economizer and is
 * reached over the event bus; economizer_module_client.h is the whole call
 * surface. What stays here is the C-side seam the server still owns:
 *   - the gateway mutate helpers + buffered orchestration,
 *   - coord_closet and tool_condense, which have callers OUTSIDE the economizer
 *     (execution-policy, agent_policy, server_state) and so did not move with it.
 */
#ifndef DEC_ECONOMIZER_H
#define DEC_ECONOMIZER_H 1

#include "coord_closet.h"
#include "economizer_module_client.h"
#include "economizer_json.h" /* agent_runtime compacts fresh tool results with it */
#include "economizer_proof.h" /* registry facts economizer_wire_snapshot still asserts on */ /* agent_runtime compacts fresh tool results with it */
#include "fold_register.h" /* session_compact classifies turns with it */
#include "tool_condense.h"

#endif /* DEC_ECONOMIZER_H */
