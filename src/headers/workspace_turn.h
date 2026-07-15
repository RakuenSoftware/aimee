#ifndef WORKSPACE_TURN_H
#define WORKSPACE_TURN_H 1

#include <stddef.h> /* size_t */

/* workspace_turn — bind the thread-active resource provider for an agent turn
 * (workspace-resource-plane §2-3, the final wiring).
 *
 * When a turn operates in `cwd`, if cwd lies inside a registered workspace
 * whose provider is `detached`, a thread-local detached provider — bound to
 * that workspace's runner queue (keyed by the workspace root path) — is made
 * the active provider, so the turn's file/exec tools marshal over the reverse
 * channel to the client serving that workspace (`aimee workspace serve <root>`).
 * For a `shared` (co-located) workspace, or an unregistered cwd, the turn stays
 * on the shared provider and nothing changes. */

/* Returns 1 if a non-shared provider was bound for this thread (the caller must
 * pair it with workspace_turn_unbind_active after the turn), 0 otherwise. */
int workspace_turn_bind_active(const char *cwd);

/* Bind this thread's file/exec tools to a DELEGATE'S OWN CONTAINER for the turn:
 * acquire a container from the `docker` backend keyed by `task_id`, and route
 * td_bash / read / write / list through it. `image` may be NULL for the backend's
 * default; `workspace` is the host directory to expose AS the container's
 * workspace — normally the tree the delegate already has server-side, so it gets
 * the entire current source tree by bind-mount rather than the backend's empty
 * scratch dir. NULL keeps that historical empty dir. `workspace_read_only` mounts
 * it :ro — required whenever the tree is not the delegate's own, because a
 * delegate's changes must not leave its container: a shared tree must be
 * unwritable at the MOUNT, not merely guarded above it. Returns 1 if bound (pair it
 * with workspace_turn_unbind_active, which also RELEASES the container), 0
 * otherwise.
 *
 * Returns 0 — leaving the turn in-process, exactly as today — when the
 * `delegate_sandbox` config dial is off (the default), or when the docker backend
 * is unavailable, or the container cannot be acquired. Those last two are LOGGED at
 * ERROR: falling back silently would run the delegate on the host while the
 * operator believes it is sandboxed.
 *
 * Unlike workspace_turn_bind_active this is not cwd-driven: a container provider
 * needs a live container handle, so the caller that owns the delegate decides. */
int workspace_turn_bind_container(const char *task_id, const char *image, const char *workspace,
                                  int workspace_read_only);

/* Clear any provider bound for this thread by workspace_turn_bind_active.
 * Safe to call unconditionally (no-op if nothing was bound). */
void workspace_turn_unbind_active(void);

/* For a `mirror` workspace, the turn does not bind a marshalling provider —
 * instead the file/exec tools run on the server-side reconstructed worktree via
 * the `shared` provider, with the turn's cwd remapped into that tree. This
 * returns the remapped cwd the caller should `run_cmd_set_cwd()` (in place of
 * the client-supplied cwd), or NULL when no remap applies (shared/detached).
 * Valid only between bind and unbind. */
const char *workspace_turn_active_cwd(void);

/* The drift summary line a `mirror` turn should surface to the user (client head
 * vs server mirror head; AC #5 — drift is surfaced, never merged), or NULL when
 * the workspace is in sync / not a mirror. Valid only between bind and unbind. */
const char *workspace_turn_drift_notice(void);

/* Resolve a cwd that lies in a registered `mirror` workspace to the equivalent
 * cwd inside its reconstructed server-side worktree, driving the mirror lifecycle
 * (ensure + reconstruct) as a side effect. Unlike workspace_turn_bind_active this
 * binds NO thread-local provider/cwd — it just returns the remapped path, for
 * non-turn callers that chdir server-side themselves (the mcp.call git tools, so
 * `/pr` works on a mirror workspace). Returns 1 and fills out[out_cap] when `cwd`
 * is in a mirror workspace; 0 otherwise (out emptied). */
int workspace_turn_resolve_mirror_cwd(const char *cwd, char *out, size_t out_cap);

/* AC #6 — the worktree_cwd-trust hole. A turn carries a client-supplied `cwd`.
 * For a co-located peer (trusted_local) it is a real server-side path; for a
 * detached workspace (detached_bound) it is acted on via the provider on the
 * client. But a REMOTE peer whose cwd is neither — a raw foreign absolute path
 * the server would otherwise open on its own filesystem — must be refused.
 *
 * Returns 1 iff the turn must be REJECTED: `cwd` is a non-empty absolute path
 * (no `..`), the turn did not bind a detached provider, and the peer is not
 * trusted-local. Pure — unit-testable, no globals. */
int workspace_turn_reject_foreign_cwd(int detached_bound, int trusted_local, const char *cwd);

#endif /* WORKSPACE_TURN_H */
