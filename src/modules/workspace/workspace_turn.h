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

/* Select the client-side repository location that a git MCP call names.
 * `path` is authoritative for every operation except clone, where it is the
 * destination rather than an existing repository. `cwd` remains the fallback.
 * Pure policy seam so dispatch and tests cannot drift on path-vs-cwd priority. */
const char *workspace_turn_git_target(const char *tool, const char *path, const char *cwd);

/* Bind this thread's file/exec tools to a DELEGATE'S OWN CONTAINER for the turn:
 * acquire a container from the `docker` backend keyed by `task_id`, and route
 * td_bash / read / write / list through it. `image` may be NULL for the backend's
 * default; `workspace` is the host directory to expose AS the container's
 * workspace — normally the tree the delegate already has server-side, so it gets
 * the entire current source tree by bind-mount rather than the backend's empty
 * scratch dir. NULL keeps that historical empty dir. `workspace_read_only` mounts
 * it :ro — required whenever the tree is not the delegate's own, because a
 * delegate's changes must not leave its container: a shared tree must be
 * unwritable at the MOUNT, not merely guarded above it.
 *
 * Returns 1 if bound (pair it with workspace_turn_unbind_active, which also
 * RELEASES the container), 0 only when `task_id` is empty and there is therefore
 * no delegate to bind, or -1 for a refusal: the caller MUST abort the delegation.
 *
 * There is no in-process fallback and no dial that selects one. A delegate runs in
 * its own container or not at all, so an unavailable backend, an unacquirable
 * container, an unauthorized workspace, and a runtime that would not honour
 * --network none all return -1. Every one is logged at ERROR where it is detected.
 *
 * Unlike workspace_turn_bind_active this is not cwd-driven: a container provider
 * needs a live container handle, so the caller that owns the delegate decides. */
/* Is `workspace` a tree aimee may hand a delegate? Canonicalizes into `out` (which
 * is what the caller must then mount — the check and the mount must be the same
 * path) and returns 1 if it lives inside a REGISTERED workspace root. Refusals are
 * logged. Repository-ness is not authorization; the registered roots are the bound.
 * Exposed so every path that can hand a workspace to the backend shares ONE bound —
 * a second copy is a second thing to forget. */
int workspace_turn_workspace_authorized(const char *workspace, char *out, size_t out_cap);

int workspace_turn_bind_container(const char *task_id, const char *image, const char *workspace,
                                  int workspace_read_only);

/* Returns 1 when this thread's turn is bound to a delegate's OWN container
 * (workspace_turn_bind_container succeeded and has not yet been unbound), 0
 * otherwise. Callers use it to relax host-oriented restrictions that do not
 * apply once file/exec run inside the delegate's isolated sandbox — e.g. the
 * shell-git gate, whose purpose is to keep raw git off aimee-server's own
 * filesystem, is moot when git runs against the container's mounted worktree.
 * Valid only between bind and unbind, on the binding thread. */
int workspace_turn_container_bound(void);

/* Test seam: force workspace_turn_container_bound()'s result. -1 restores the real
 * thread-bound state; 0/1 pin it. For tests exercising the container-delegate exemption. */
void workspace_turn_set_container_bound_for_test(int bound);

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

/* Same resolution, for a `detached` workspace that also carries mirror inputs.
 *
 * A detached workspace is served by its client, so a job running with no client
 * attached (a background delegate) cannot reach it at all. When such a workspace
 * has recorded a `remote` and a `head` — a client ran `workspace mirror-sync` —
 * the server can still reconstruct an equivalent tree from its own bare mirror
 * and run there instead of having nowhere to go.
 *
 * That tree is the LAST SYNCED state, not the client's current one, so this is
 * deliberately a separate entry point rather than a widening of the mirror
 * resolver: a caller must choose the stale-but-real tree knowingly, and say so.
 * Returns 1 and fills out[out_cap] when `cwd` is in a detached workspace with
 * both inputs recorded; 0 otherwise (out emptied). */
int workspace_turn_resolve_detached_mirror_cwd(const char *cwd, char *out, size_t out_cap);

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
