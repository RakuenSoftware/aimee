/* client_session_worktree.h: thin-client per-session worktree bootstrap.
 *
 * Every new agent session — a Claude Code session via the SessionStart hook, an
 * MCP-hosted session via `aimee mcp serve` — must run on its OWN branch cut from
 * the repository's default branch, inside its OWN worktree. The server-side
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
 * all land in one place.
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

/* Resolve the branch a fresh session worktree should be cut from, mirroring the
 * server's worktree_detect_base_branch policy: AIMEE_SESSION_WORKTREE_BASE (or
 * the "remote_default" default) -> origin/HEAD -> the local default branch.
 * Deliberately has NO silent fall back to the current HEAD: inheriting whatever
 * branch the shared checkout happens to be on is what put sessions on another
 * session's branch. `current` is reachable only by explicit opt-in.
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
