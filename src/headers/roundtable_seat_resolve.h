/* roundtable_seat_resolve.h: resolve a roundtable seat's model to a concrete
 * agent, honoring the two seat kinds a user configures in the GUI:
 *
 *   - "$random"        -> pick ANY agent that can serve the role, retrying a
 *                         different one until one is accepted (diversity via the
 *                         caller's `used` exclusion set);
 *   - a specific model -> that EXACT agent, with NO substitution. If it is not
 *                         enabled/routable for the role, resolution fails so the
 *                         caller can fail the run (a pinned model must be honored
 *                         or the run stops — it is never silently swapped).
 *
 * Shared by the autonomous workflow gate panel (wfe_live_panel.c) and the
 * interactive ensemble path (delegate_ensemble.c) so the two kinds behave
 * identically wherever a seat model is consumed. */
#ifndef DEC_ROUNDTABLE_SEAT_RESOLVE_H
#define DEC_ROUNDTABLE_SEAT_RESOLVE_H 1

/* config.h first: agent_config.h pulls in agent_types.h (which uses MAX_PATH_LEN)
 * before its own config.h include, so define the base constants up front to keep
 * this header self-contained regardless of include order. */
#include "config.h"
#include "agent_config.h"

/* The sentinel a seat's model carries to mean "any role-capable agent". Kept in
 * sync with the GUI seat editor and wfe_delegate_resolve.c's delegate sentinel. */
#define RT_SEAT_RANDOM "$random"

typedef enum
{
   RT_SEAT_OK = 0,             /* *out_idx holds a viable agent index in cfg */
   RT_SEAT_RANDOM_EXHAUSTED,   /* "$random": no eligible role agent remains (excl. `used`) */
   RT_SEAT_PINNED_UNAVAILABLE, /* pinned model absent/disabled/unroutable for the role */
   RT_SEAT_INVALID             /* bad args */
} rt_seat_resolve_t;

/* 1 iff `model` is the "$random" sentinel (NULL/empty is treated as random too,
 * so an unset seat degrades to "any role-capable agent" rather than failing). */
int rt_seat_is_random(const char *model);

/* Resolve `model` for `role`. On RT_SEAT_OK sets *out_idx to the chosen agent's
 * index in `cfg`. For "$random", `used`/`nused` are the agent names already seated
 * (excluded for diversity); re-call with the failed agent appended to retry. A
 * pinned model ignores `used` (it is a single fixed choice). */
rt_seat_resolve_t rt_resolve_seat_model(agent_config_t *cfg, const char *model, const char *role,
                                        const char *const used[], int nused, int *out_idx);

#endif /* DEC_ROUNDTABLE_SEAT_RESOLVE_H */
