/* wfe_live_delegate.h: server-side registration of the workflow engine's live
 * providers (the delegate that actually does the work + the full block/gate
 * executor set). Called once at server_init. Autonomous development is core
 * functionality and ships DEFAULT-ON; see
 * docs/proposals/pending/full-autonomous-development.md. */
#ifndef DEC_WFE_LIVE_DELEGATE_H
#define DEC_WFE_LIVE_DELEGATE_H 1

/* Register the live delegate provider + the default block executors + the
 * roundtable/human gate executors so the engine can run real work items
 * server-side. Idempotent enough to call once at startup. */
void wfe_autonomy_register(void);

#include "agent_config.h"

/* Resolve a workflow step's delegate name to a concrete agent. "" -> "" (route
 * by role); a specific name -> itself; the sentinel "$random" -> a uniformly
 * random ENABLED agent from `acfg`. Returns 0 on success (out filled, possibly
 * ""), -1 if "$random" but no enabled agent exists (fail fast — never leak the
 * sentinel). */
int wfe_resolve_delegate(const char *name, agent_config_t *acfg, char *out, size_t outn);

/* Seed the $random selector for deterministic tests. */
void wfe_resolve_delegate_seed(unsigned seed);

#endif /* DEC_WFE_LIVE_DELEGATE_H */
