#ifndef DEC_DELEGATE_ROLE_H
#define DEC_DELEGATE_ROLE_H 1

#include "aimee.h"
#include "agent_config.h"

typedef int (*delegate_role_canonicalizer_fn)(const char *role, char *out, size_t out_cap);
void delegate_role_register_canonicalizer(delegate_role_canonicalizer_fn canonicalizer);

/* Role POLICY -- what a role implies about how it is run -- lives in the
 * delegates module (server-go/modules/delegates/rolepolicy.go). This is the
 * seam the C side calls through; with no provider registered every answer
 * below is the conservative one: not a write role, no implicit tools, not
 * cacheable, no early-final turn. Inventing "cacheable" would serve a stale
 * answer about a changed working tree, and inventing "tools on" would hand a
 * filesystem to a role that was never meant to have one.
 *
 * op selects the question; `a` carries max_turns and `b` explicit_tools for the
 * auto-tools op, and both are unused otherwise. */
#define DELEGATE_ROLE_OP_IS_WRITE     0
#define DELEGATE_ROLE_OP_BUILTIN      1
#define DELEGATE_ROLE_OP_CACHE        2
#define DELEGATE_ROLE_OP_AUTO_TOOLS   3
#define DELEGATE_ROLE_OP_FINAL_TURNS  4
#define DELEGATE_ROLE_OP_PARENT_DIFF  5
#define DELEGATE_ROLE_OP_TASK_SHAPE   6

typedef int (*delegate_role_policy_fn)(int op, const char *role, int a, int b, int c,
                                      int *out);
void delegate_register_role_policy_provider(delegate_role_policy_fn provider);


/* Returns 1 when it is safe to reuse an agent response keyed only by
 * (role, prompt). This is opt-in for pure text-transform roles; repository
 * inspection, execution, and custom roles must not use this cache because
 * identical prompts can refer to changed working-tree state. */
int delegate_role_result_cache_enabled(const char *role);

/* Returns 1 when implicit role-default tool use should be applied for this
 * invocation. A one-turn override is treated as a final-answer smoke probe
 * unless the caller explicitly requests tools. */
int delegate_auto_tools_for_invocation(int holds_tools, int max_turns, int explicit_tools);

/* Returns 1 when a read-only inspection role should be grounded in the PARENT
 * worktree's uncommitted diff.
 *
 * The ROLE half only. The caller composes the rest -- suppressed for a delegate
 * that may write, and for a review target supplied in the prompt -- because both
 * are conditions of the invocation rather than properties of the role.
 *
 * Fails to 0: with no provider the delegate simply runs without the extra
 * context, which is what it did before this evidence existed. */
int delegate_role_needs_parent_diff(const char *role);

/* What SHAPE of work this role does, as task_type_t (see agent_types.h). It
 * shapes context assembly and the opening instruction; it grants nothing.
 *
 * Fails to 0 (general), which is the neutral weighting -- the same answer the
 * keyword scan this replaced gave when it recognised nothing. */
int delegate_role_task_shape(const char *role);


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

/* Apply a per-invocation safety ceiling without raising a stricter agent/role
 * limit. Unlike an explicit max_turns override, this converts unlimited
 * (-1/0) to `cap` and clamps only eligible agents above it. */
void delegate_apply_max_turns_cap(agent_config_t *cfg, const char *role, int cap);

/* Canonicalize a delegate role name.  Returns the canonical name if role is
 * a known alias (e.g. "implement" -> "code"), otherwise returns role unchanged.
 * Never returns NULL.  The returned pointer is either role itself or a string
 * literal — do not free it. */
const char *delegate_role_canonicalize(const char *role);

/* Non-NULL when `role` was deleted by the persona-vs-role cull, giving an
 * operator-facing reason and the migration path. Callers should refuse the
 * invocation rather than route it: the name would otherwise still be accepted as
 * an arbitrary role string while its write classification, tool defaults and
 * built-in template no longer exist. */
const char *delegate_role_removed_reason(const char *role);

/* 1 when `role` (or its canonical alias) names a real delegate role: a built-in
 * role, or one an operator defined with a project/user role template.
 * `project_root` may be NULL to skip the project-level template lookup.
 *
 * Dispatch must refuse an unknown name rather than route it. An unrecognized
 * role has no prompt template, no write classification and no agent that can
 * declare it, so it runs as a read-only delegate with a generic prompt while the
 * caller believes it asked for something specific. */
int delegate_role_known(const char *project_root, const char *role);

/* Returns 1 if role is a write-capable role (code or refactor, including
 * aliases).  Write roles are subject to no-op edit detection. */
int delegate_role_is_write(const char *role);

#endif /* DEC_DELEGATE_ROLE_H */
