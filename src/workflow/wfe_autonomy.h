/* wfe_autonomy.h: the autonomy driver + the human-only gate-override.
 *
 * In autonomous mode the driver auto-advances machine gates (via the engine) and
 * parks at EVERY human gate (pending_human) and at a degraded roundtable. A human
 * gate is inviolable: the driver never auto-satisfies one — not even with node
 * params like policy:preauthorized or optional:true (those are rejected at
 * workflow validation) — and never forges a human approval. Only a human's
 * signed action (gate-override / the gate endpoint, attributed to an attested
 * principal) clears a human gate; autonomous runs cannot invoke it. */
#ifndef DEC_WFE_AUTONOMY_H
#define DEC_WFE_AUTONOMY_H 1

#include <stddef.h>

#define WFE_MAX_OVERRIDES 2

/* Default per-(work item, stage) budget for auto-retrying a TRANSIENT roundtable
 * park (panel_degraded / panel_unreachable). With one retry per scheduler backstop
 * sweep this is roughly "how many sweeps a persistent degradation is tolerated
 * before it escalates to a human." Override with AIMEE_AUTONOMY_PANEL_RETRIES
 * (an explicit 0 disables auto-retry; a malformed/negative value floors here). */
#define WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT 6

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
