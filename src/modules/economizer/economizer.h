/* economizer.h -- the public facade for what REMAINS of the economizer in C.
 *
 * Context reduction itself now lives in server-go/modules/economizer and is
 * reached over the event bus; economizer_module_client.h is the whole call
 * surface. What stays here is the C-side seam the server still owns:
 *   - the gateway mutate helpers + buffered orchestration,
 *   - coord_closet and tool_condense, which still have callers outside the
 *     economizer (session_compact, agent_tools_dispatch, server_state).
 */
#ifndef DEC_ECONOMIZER_H
#define DEC_ECONOMIZER_H 1

#include "economizer_module_client.h"
#include "coord_closet.h" /* session_compact conserves identifiers with it */
#include "economizer_json.h" /* agent_runtime compacts fresh tool results with it */
#include "economizer_proof.h" /* registry facts economizer_wire_snapshot still asserts on */ /* agent_runtime compacts fresh tool results with it */
#include "fold_register.h" /* session_compact classifies turns with it */
#include "tool_condense.h"

#endif /* DEC_ECONOMIZER_H */
