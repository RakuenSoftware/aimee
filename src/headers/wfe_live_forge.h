/* wfe_live_forge.h: register the live forge provider (F4).
 *
 * Installs a wfe_forge_t that does real git push + `gh` PR/CI/merge — but ONLY when
 * the operator has set wfe_live_forge_enabled=true (default OFF). Otherwise the
 * engine keeps its fail-closed stub. Called from wfe_autonomy_register. */
#ifndef DEC_WFE_LIVE_FORGE_H
#define DEC_WFE_LIVE_FORGE_H 1

void wfe_live_forge_register(void);

#endif /* DEC_WFE_LIVE_FORGE_H */
