#ifndef DEC_DELEGATE_ROLE_H
#define DEC_DELEGATE_ROLE_H 1

#include "aimee.h"
#include "agent_config.h"

/* Returns 1 when the role should have tool use enabled even without
 * an explicit --tools flag. */
int delegate_role_enable_tools_by_default(const char *role);

/* Returns 1 when it is safe to reuse an agent response keyed only by
 * (role, prompt). This is opt-in for pure text-transform roles; repository
 * inspection, execution, and custom roles must not use this cache because
 * identical prompts can refer to changed working-tree state. */
int delegate_role_result_cache_enabled(const char *role);

/* Returns 1 when implicit role-default tool use should be applied for this
 * invocation. A one-turn override is treated as a final-answer smoke probe
 * unless the caller explicitly requests tools. */
int delegate_role_auto_tools_for_invocation(const char *role, int max_turns, int explicit_tools);

/* Apply a one-shot CLI max-turn override to every configured delegate route.
 * max_turns < 0 means "no override"; 0 preserves the runtime's unlimited
 * sentinel. */
void delegate_apply_max_turns_override(agent_config_t *cfg, int max_turns);

/* Return the implicit max-turn cap for inspection-oriented roles, or -1 when
 * the role should keep the configured/global delegate default. */
int delegate_default_max_turns_for_role(const char *role);

/* Return the turn at which inspection delegates should stop tool use and
 * synthesize a final response, or -1 when the role has no early-final policy. */
int delegate_final_after_turns_for_role(const char *role);

/* Apply the complete per-invocation max-turn policy. Explicit max_turns keeps
 * existing override behavior; otherwise read-only inspection roles get a lower
 * cap so status/review/validate delegates do not burn the global delegate
 * budget before returning useful signal. */
void delegate_apply_max_turns_policy(agent_config_t *cfg, const char *role, int max_turns);

/* Canonicalize a delegate role name.  Returns the canonical name if role is
 * a known alias (e.g. "implement" -> "code"), otherwise returns role unchanged.
 * Never returns NULL.  The returned pointer is either role itself or a string
 * literal — do not free it. */
const char *delegate_role_canonicalize(const char *role);

/* Returns 1 if role is a write-capable role (code or refactor, including
 * aliases).  Write roles are subject to no-op edit detection. */
int delegate_role_is_write(const char *role);

#endif /* DEC_DELEGATE_ROLE_H */
