/* wfe_roundtable.h: the gate.roundtable executor + the panel-provider seam.
 * The live provider calls the ensemble engine and is gated on
 * roundtable-panel-composition (§0); tests inject a mock provider. */
#ifndef DEC_WFE_ROUNDTABLE_H
#define DEC_WFE_ROUNDTABLE_H 1

#include "wfe_verdict.h"

/* A panel provider runs the configured panel against the artifact and fills up
 * to `max` verdicts. Returns the number of verdicts, or -1 if the panel could
 * not be reached/composed (-> the gate parks DEGRADED). */
typedef int (*wfe_panel_run_fn)(const char *artifact_hash, const char *const *required, int nreq,
                                const char *const *eligible, int nelig, wfe_verdict_t *out,
                                int max);

/* Install a panel provider (tests inject a mock; NULL restores the default). */
void wfe_set_panel_provider(wfe_panel_run_fn fn);

/* Register the gate.roundtable executor behind the wfe_iface vtable. */
void wfe_register_roundtable_gate(void);

#endif /* DEC_WFE_ROUNDTABLE_H */
