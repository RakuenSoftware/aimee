/* wfe_autonomy.h: the autonomy driver + the human-only gate-override.
 *
 * In autonomous mode the driver auto-advances machine gates (via the engine) and
 * auto-satisfies ONLY human gates whose policy is preauthorized/optional; it
 * parks at every other human gate (pending_human) and at a degraded roundtable.
 * It NEVER forges a human approval: gate-override is a signed human action that
 * autonomous runs cannot invoke. */
#ifndef DEC_WFE_AUTONOMY_H
#define DEC_WFE_AUTONOMY_H 1

#include <stddef.h>

#define WFE_MAX_OVERRIDES 2

/* Drive a work item as far as its mode + gate policies allow. Returns 0 on a
 * clean stop (terminal or parked), -1 on error. */
int wfe_autonomy_run(const char *work_item_id, char *err, size_t errlen);

/* Human-only escape from a stuck gate: records a signed override (counts as a
 * human approval), increments override_count, and clears the pause so the next
 * run advances. On the (WFE_MAX_OVERRIDES+1)th override the work item is forced
 * to terminal `rejected`. Returns 0 on success, 1 if it forced rejection, -1 on
 * error. */
int wfe_gate_override(const char *work_item_id, const char *gate, const char *actor,
                      const char *reason, char *err, size_t errlen);

#endif /* DEC_WFE_AUTONOMY_H */
