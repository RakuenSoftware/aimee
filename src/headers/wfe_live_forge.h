/* wfe_live_forge.h: register the live forge provider (F4).
 *
 * Installs a wfe_forge_t that does real git push + `gh` PR/CI/merge — unless the
 * operator has set wfe_live_forge_enabled=false (default ON). When disabled the
 * engine keeps its fail-closed stub. Called from wfe_autonomy_register. */
#ifndef DEC_WFE_LIVE_FORGE_H
#define DEC_WFE_LIVE_FORGE_H 1

void wfe_live_forge_register(void);

#endif /* DEC_WFE_LIVE_FORGE_H */
