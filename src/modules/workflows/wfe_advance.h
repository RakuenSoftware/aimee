/* wfe_advance.h -- S2 sub-slice 3: the explicit `advance_request` tool and its
 * event-bus decision seam.
 *
 * A bound interactive session does NOT let the engine auto-run its work-item (that
 * is the autonomous scheduler's job). Instead the primary, having produced the
 * current block's artifact, calls ONE explicit `advance_request` tool to move the
 * work-item forward exactly one block. The advance is a server-authoritative
 * tool-executor action -- NEVER inferred from prose ("I've completed X") -- so a
 * free-text claim can never cross a gate (consult Q1, RT1).
 *
 * This C seam parses and validates tool arguments, then obtains the decision from
 * the separately supervised workflows module over the event bus. The binding
 * lookup, work-item read, actual `wfe_engine_advance`, and audit write remain in
 * the integration layer (wfe_advance_exec). */
#ifndef DEC_WFE_ADVANCE_H
#define DEC_WFE_ADVANCE_H 1

#include <stddef.h>

struct cJSON;

/* The tool the primary calls to advance its bound work-item. Named consistently
 * across the router / DB1 / lifecycle_event / audit (consult Q1 #47). Subject to
 * the per-block tool-strip (sub-slice 4): if the current block's allowed_tools
 * omits it, the gateway strips it so the primary cannot advance out of a
 * converse/review block to skip review. */
#define WFE_ADVANCE_TOOL_NAME "advance_request"

#define WFE_ADVANCE_WI_LEN    80
#define WFE_ADVANCE_STAGE_LEN 64
#define WFE_ADVANCE_NONCE_LEN 64

/* Parsed + validated `advance_request` arguments. The primary states the work-item
 * and the stage it OBSERVED as current (the CAS token, consult Q1) plus an optional
 * client-generated idempotency nonce so an exact turn retry is a no-op rather than
 * a double-advance. */
typedef struct
{
   char work_item_id[WFE_ADVANCE_WI_LEN];
   char observed_stage[WFE_ADVANCE_STAGE_LEN];
   char nonce[WFE_ADVANCE_NONCE_LEN];
   int have_nonce; /* 1 if a nonce was supplied */
} wfe_advance_args_t;

/* Parse the tool-call arguments object. `args_json` is the raw JSON arguments the
 * tool call carried. Fills `out` (zeroed first). Requires a non-empty, id-charset
 * `work_item_id` and `observed_stage`; a present-but-invalid nonce fails. Returns 0
 * on success, -1 on malformed / missing / out-of-charset input (fail closed). */
int wfe_advance_parse_args(const char *args_json, wfe_advance_args_t *out);

/* The driver decision, computed purely from already-fetched state. */
typedef enum
{
   WFE_ADV_OK = 0,   /* bound, fresh, CAS matched -> advance exactly one step */
   WFE_ADV_REPLAY,   /* same nonce as the last applied advance -> idempotent no-op */
   WFE_ADV_STALE,    /* observed_stage != the work-item's actual stage -> reject */
   WFE_ADV_UNBOUND,  /* session not bound, or bound to a different work-item -> reject */
   WFE_ADV_TERMINAL, /* work-item already accepted/rejected/abandoned -> reject */
   WFE_ADV_BADARGS   /* NULL/empty args struct */
} wfe_advance_outcome_t;

const char *wfe_advance_outcome_name(wfe_advance_outcome_t o);

/* Implemented by the server's event-bus bridge. The workflows seam never
 * evaluates advance policy locally. */
typedef int (*wfe_advance_decision_provider_fn)(const char *bound_wi,
                                                const wfe_advance_args_t *args,
                                                const char *actual_stage, const char *actual_state,
                                                const char *last_nonce,
                                                wfe_advance_outcome_t *outcome);

void wfe_advance_register_decision_provider(wfe_advance_decision_provider_fn provider);

/* Decide the outcome. `bound_wi` = the work-item the caller's session is bound to
 * ("" / NULL if unbound). `a` = parsed args. `actual_stage` / `actual_state` = the
 * work-item row's current stage + state ("active" | "accepted" | "rejected" |
 * "abandoned"). `last_nonce` = the nonce of the most recent APPLIED advance for
 * this work-item ("" / NULL if none).
 *
 * Returns 0 with the module's validated outcome, or -1 when no provider is
 * registered or the event-bus decision fails. There is no local fallback.
 */
int wfe_advance_decide(const char *bound_wi, const wfe_advance_args_t *a, const char *actual_stage,
                       const char *actual_state, const char *last_nonce,
                       wfe_advance_outcome_t *outcome);

/* Emit the `advance_request` tool JSON-Schema "parameters" object (fresh cJSON,
 * caller owns). Shared by the sub-slice-4 tool injector so the schema has a single
 * source of truth. Returns NULL only on allocation failure. */
struct cJSON *wfe_advance_tool_params(void);

/* The tool's one-line description, for the injector. */
const char *wfe_advance_tool_description(void);

#endif /* DEC_WFE_ADVANCE_H */
