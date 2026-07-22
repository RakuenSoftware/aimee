/* wfe_live_panel.h -- the live roundtable panel provider (per-persona review
 * delegates -> verdicts). Registered from wfe_autonomy_register. */
#ifndef DEC_WFE_LIVE_PANEL_H
#define DEC_WFE_LIVE_PANEL_H 1

/* Register the live gate.roundtable panel provider (dispatches one read-only review
 * per required persona and maps each reply to a verdict). */
void wfe_live_panel_register(void);

#endif /* DEC_WFE_LIVE_PANEL_H */
