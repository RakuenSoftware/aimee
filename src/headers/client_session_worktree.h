/* client_session_worktree.h: thin-client per-session worktree bootstrap.
 *
 * Every new agent session, regardless of host client, must run on its OWN branch
 * cut from the repository's default branch, inside its OWN worktree. The generic
 * `aimee launch -- <client>` process boundary calls this before the client starts;
 * SessionStart and MCP children never try to move their already-running parent.
 * The server-side
 * implementation of that policy lives in modules/workspace (worktree_create_
 * sibling_at_ref); this is the thin-client twin of it.
 *
 * It exists separately because the client binary links none of workspace.o /
 * config.o / guardrails.o, so it cannot call those functions: everything here
 * goes through shell-free `git` subprocesses. Both produce:
 *
 *   worktree: <git_root>/.aimee/worktrees/<key>/main
 *   branch:   aimee/session/<key>
 *   base:     the repository's default branch (never the checkout's current
 *             branch, unless an operator opts in explicitly)
 *
 * The key itself comes from session_worktree_key() (session_worktree_key.h),
 * which the server links too — so a worktree placed by one side is found by the
 * other. That was NOT always true: the client used to hash the id while the
 * server truncated it, and the two disagreed about where a session lived.
 *
 * Delegates are deliberately NOT handled here: a delegate must inherit its
 * PARENT's branch and working-tree state, which is the server-side
 * worktree_create_sibling_from_anchor path (cmd_agent_delegate.c).
 */
#ifndef DEC_CLIENT_SESSION_WORKTREE_H
#define DEC_CLIENT_SESSION_WORKTREE_H 1

#include <stddef.h>

/* Ensure the per-session worktree for `sid` exists, creating it (and its
 * session branch, cut from the base branch) if needed. Idempotent: re-running
 * for the same session id reuses the same worktree, so startup/resume/compact
 * all land in one place. The launcher calls this before exec'ing the client.
 *
 * Writes the absolute worktree path into out[cap] on success.
 *
 * Returns:
 *    0  the worktree exists and out holds its path
 *   -1  not applicable — isolation is disabled, the caller is already inside a
 *       managed worktree, there is no session id, or the cwd is not a git repo
 *   -2  applicable but FAILED — the base branch could not be resolved or git
 *       refused to create the worktree. A diagnostic has been written to stderr.
 *
 * Never chdirs; the caller decides whether to enter the worktree. */
int client_session_worktree_ensure(const char *sid, char *out, size_t cap);

/* As above, but resolve the repository from `cwd` rather than the caller's
 * process cwd. Lifecycle and tool hooks must use the cwd in their payload: a
 * hook subprocess can have a different cwd, and it cannot chdir its parent.
 * An existing Aimee worktree is reused only when its key belongs to `sid`;
 * entering another session's worktree creates/routes to this session's own. */
int client_session_worktree_ensure_at(const char *sid, const char *cwd, char *out, size_t cap);

/* SessionEnd cleanup. Removes only a clean Aimee-owned checkout. Git refuses
 * dirty/untracked trees, and the session branch is deleted only when merged.
 * Returns 0 removed, 1 retained/not applicable, -1 invalid input. */
int client_session_worktree_release_at(const char *sid, const char *cwd);

/* Transparently route a tool path or shell command from the checkout named by
 * `cwd` into `sid`'s worktree. These are the client-neutral primitives used by
 * Claude, Codex, OpenCode, Hermes, and future lifecycle adapters.
 *
 * route_path writes an absolute path. A NULL/empty input means "the effective
 * cwd", which is useful for shell tools. route_command prefixes the command
 * with the routed cwd and remaps literal absolute source-root references.
 * A path outside the repository is returned unchanged without provisioning a
 * worktree; external reads must not depend on repository base resolution.
 *
 * Returns 0 when routed, 1 when isolation is not applicable, -2 when the
 * worktree could not be prepared, and -3 when the input explicitly targets a
 * different Aimee session's worktree. */
int client_session_worktree_route_path(const char *sid, const char *cwd, const char *input,
                                       char *out, size_t cap);
int client_session_worktree_route_command(const char *sid, const char *cwd, const char *command,
                                          char *out, size_t cap);
int client_session_worktree_route_patch(const char *sid, const char *cwd, const char *patch,
                                        char *out, size_t cap);

/* Resolve the exact ref/commit a fresh session worktree should be cut from,
 * mirroring the server policy. With origin configured, session start performs
 * a bounded, non-interactive fetch and pins the remote's current HEAD commit;
 * it never accepts stale tracking data after a failed fetch. An explicit
 * feature/release ref is preserved, but creation makes the fetched default tip
 * its ancestor inside the new session branch. `current` and `local_default`
 * are the explicit offline/stale overrides.
 * Returns 0 and fills buf[cap] on success, -1 when no base could be resolved. */
int client_session_worktree_base(const char *git_root, char *buf, size_t cap);

/* The collision-free worktree/branch key for a session id: a short alnum prefix
 * of the id for readability plus a 64-bit FNV-1a hash of the FULL id, so two
 * distinct ids never collide on a shared sanitized prefix. Pure; exposed for
 * tests and so both call sites derive the same key. */
void client_session_worktree_key(const char *sid, char *out, size_t cap);

/* Publish the HOST-assigned session id under <home>/session-ppid-<pid> so the
 * session's other processes resolve the same one, and therefore the same
 * worktree. Written for the caller's parent AND, on Linux, for the host process
 * found by walking up to it -- a hook whose command carries an env assignment
 * runs under a shell and so is a grandchild, while `aimee mcp serve` is a direct
 * child and reads the key named for the host. The walk stops at the host so no
 * shared ancestor (terminal, service manager) is ever named.
 *
 * Authoritative: truncates an existing file, because the host's id outranks one
 * a peer minted for itself when it could not find this. Rejects a sid
 * containing '/', a newline, or a control character.
 *
 * Returns 0 when at least one location was published, -1 otherwise (including
 * on Windows, where session-worktree isolation is not a feature). */
int client_session_id_publish(const char *sid, const char *home);

#endif /* DEC_CLIENT_SESSION_WORKTREE_H */
