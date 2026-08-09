/* gw_stage_governance.h -- Response GOVERNANCE as a togglable event-bus module.
 * The response registry owns local parsed-response mutation, while the
 * separately supervised module is the sole source of policy decisions. */
#ifndef DEC_GW_STAGE_GOVERNANCE_H
#define DEC_GW_STAGE_GOVERNANCE_H 1

#include <aimee/governance/module_api.h>

struct parsed_response;

/* The server-side event-bus bridge implements this provider. The governance
 * stage never evaluates policy locally: when enabled, every decision comes
 * from the separately supervised governance module over the event bus. */
typedef int (*gw_governance_decision_provider_fn)(int policy_active, const char *const *tool_names,
                                                  uint32_t tool_count, const char *stop_reason,
                                                  aimee_governance_decision_t *decision);

void gw_response_governance_register_provider(gw_governance_decision_provider_fn provider);

/* The DEPRECATED env fallback is opt-in: AIMEE_STAGE_GOVERNANCE accepts
 * 1/on/true/yes and otherwise resolves off. This must agree with the process
 * descriptor's enabled_by_default=false; enabling the response stage while the
 * separately supervised module is absent makes its intentional fail-closed path
 * reject every ordinary tool call. The config-store `modules.governance` toggle
 * is canonical; wire sites resolve it via config_module_enabled() with this as
 * the fallback. Kept pure so the module stays config-free. */
int gw_response_governance_enabled(void);

/* Run the (togglable) governance response stage over `parsed` via the response registry +
 * runner. `enabled` is the caller-resolved module toggle and `policy_active` is the
 * already-resolved core enforcement gate. When enabled, the decision is obtained only through
 * the registered event-bus provider. A missing/failed/invalid provider response drops every
 * pending tool call (fail closed); there is no local policy fallback. Returns the intervention
 * count (>=0), or 0 when governance is disabled or `parsed` is NULL. */
int gw_response_run_governance(struct parsed_response *parsed, int enabled, int policy_active);

#endif /* DEC_GW_STAGE_GOVERNANCE_H */
