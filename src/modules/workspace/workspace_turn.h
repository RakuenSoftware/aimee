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

/* Returns 1 if a registered provider or mirror was bound for this thread,
 * 0 for shared/unregistered cwd, or -1 when reconstruction fails. A negative
 * result must refuse the turn. Pair binding with workspace_turn_unbind_active. */
int workspace_turn_bind_active(const char *cwd);

/* Nested operations own separate provider storage and leave the enclosing turn's
 * resource leases intact. A non-NULL scope must be ended even when begin returns
 * 0 (shared/unresolved cwd). -1 leaves the caller untouched. End is LIFO and
 * returns -1 without changing state for an out-of-order scope. Join callbacks
 * borrowing a nested provider before ending its scope. */
typedef struct workspace_turn_scope workspace_turn_scope_t;
int workspace_turn_scope_begin(const char *cwd, workspace_turn_scope_t **out);
/* A background worker cannot depend on a client runner. Explicit mirror and
 * detached registrations use their recorded source/head under an owned local
 * scope; failed reconstruction refuses without exposing the foreign cwd. */
int workspace_turn_scope_begin_background(const char *cwd, workspace_turn_scope_t **out);
/* Host creation result only: call immediately after the worktree owner creates
 * this turn's dedicated child. Retains that exact directory under the held
 * reconstruction, never a prefix grant or a model-supplied mount path. */
int workspace_turn_scope_hold_created_worktree(workspace_turn_scope_t *scope, const char *cwd);
int workspace_turn_scope_end(workspace_turn_scope_t *scope);

/* An owned mirror directory capability for retained callbacks. Capture duplicates
 * the owner's held descriptor. Bind borrows an immutable capability until restore;
 * release only after every callback has joined. No path/JSON input creates one. */
typedef struct workspace_turn_mirror_lease workspace_turn_mirror_lease_t;
int workspace_turn_mirror_capture(const char *cwd, workspace_turn_mirror_lease_t **out);
int workspace_turn_mirror_valid(const workspace_turn_mirror_lease_t *lease, const char *cwd);
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *lease);
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *lease);

/* The host may supply a held directory from its authenticated, registered
 * project owner. A path alone cannot create this capability; missing callbacks
 * and refused ownership keep the existing explicit-registration requirements. */
typedef int (*workspace_turn_project_open_fn)(const char *cwd);
void workspace_turn_register_project_owner(workspace_turn_project_open_fn owner);

/* Identity-only attestation of the currently held tool resource. Shared/mirror
 * directories return an owned fd pinning the measured inode; container/detached
 * identities include this owner's live acquisition epoch, never a pointer.
 * The caller retains its turn lease and closes any returned fd. No access grant
 * or path/provider fallback is created. Failure leaves fd=-1 and digest empty. */
int workspace_turn_resource_identity(const char *cwd, int *directory_fd, char digest[65]);
/* Approved local source custody, independent of the active execution namespace.
 * A container turn returns a duplicate of the exact directory retained when its
 * approved source was bound; the pathname must still name that held inode.
 * Shared/mirror sources retain their existing local owner rules. Detached-only,
 * sibling, replaced, unbound and unapproved sources refuse. This API grants no
 * host tool execution or provider fallback; caller closes the returned fd. */
int workspace_turn_source_identity(const char *cwd, int *directory_fd, char digest[65]);

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
 * the entire current source tree by bind-mount. NULL or empty is refused.
 * `workspace_read_only` mounts it :ro — required whenever the tree is not the
 * delegate's own, because a delegate's changes must not leave its container: a
 * shared tree must be unwritable at the MOUNT, not merely guarded above it.
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
 * path) and returns 1 for a REGISTERED root or the exact actively held mirror
 * reconstruction of one. Refusals are
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
