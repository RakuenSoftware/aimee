#ifndef GIT_CRED_INJECT_H
#define GIT_CRED_INJECT_H 1

#include <stddef.h> /* size_t */

/* git_cred_inject — assemble the execve() environment for a webchat user's git
 * operation, injecting their HTTPS forge token from the sealed vault (webchat-git
 * WP-C). The token is read autonomously (server wrap) from the `webuser:`
 * principal's vault and crosses ONLY in the git child's environment (GH_TOKEN) +
 * a GIT_ASKPASS shim — never on disk, never on the command line, never logged,
 * vault-only at rest.
 *
 * Cross-tenant safety: the git child runs as the owning user's own OS UID
 * (WP-I), and the kernel restricts /proc/<pid>/environ to that UID, so another
 * webuser cannot read the token from the environment. Per-principal isolation +
 * vault-at-rest is the security boundary.
 *
 * SSH-key injection (in-memory ssh-agent) is WP-C2. */

/* Build a NULL-terminated envp for an execve() of a git command, with the HTTPS
 * forge token resolved vault-first and injected via GH_TOKEN + the GIT_ASKPASS
 * shim (GIT_TERMINAL_PROMPT=0), plus SSH_AUTH_SOCK when a vaulted key is loaded.
 * This is the ONE credential-resolution policy every git network op routes
 * through, so the precedence can never drift between call sites.
 *
 * Token precedence (first hit wins):
 *   1. preferred_token — a live caller-supplied token that must win: an inline
 *      clone token, or the in-memory workspace broker token (§4). NULL = none.
 *   2. per-host vault  — git_host_cred for the repo's remote HOST, resolved from
 *      `remote_url` (if given) else from the `origin` remote of `repo_dir`. This
 *      is the built-in server vault (server principal, server-wrap), keyed by
 *      host so one server can talk to many forges (github/gitlab/gitea/...).
 *   3. webuser vault   — the `principal`'s own vaulted personal forge token.
 *   4. server identity — a GitHub App installation token, else AIMEE_FORGE_TOKEN.
 * The `principal`'s vaulted SSH key is also loaded into the in-memory ssh-agent
 * (SSH_AUTH_SOCK) when present, independent of the token above.
 *
 * Any of principal/remote_url/repo_dir/preferred_token may be NULL. `repo_dir` is
 * consulted only when a host token is needed and `remote_url` is NULL — a single
 * local `git config --get remote.origin.url` (no network, no creds). Returns a
 * malloc'd array (free with git_cred_inject_free_env), or NULL when no credential
 * of any kind is available (caller falls back to ambient creds) or on error.
 *
 * `out_token_fd` selects how an HTTPS token reaches git:
 *   - non-NULL  → FD MODE. The token is written to a CLOEXEC memfd (anonymous,
 *     never a named path) whose fd is returned in *out_token_fd, and the env
 *     carries AIMEE_GIT_TOKEN_FD=GIT_CRED_TOKEN_TARGET_FD (a number, not the
 *     secret) instead of GH_TOKEN. The caller must run git with
 *     safe_exec_capture_cwd_env_fd_timeout(inherit_fd=*out_token_fd,
 *     target_fd=GIT_CRED_TOKEN_TARGET_FD) and close *out_token_fd afterwards. The
 *     token then never appears in the child's /proc/<pid>/environ. *out_token_fd
 *     is set to -1 when there is no HTTPS token (ssh-only / no creds).
 *   - NULL      → LEGACY ENV MODE. The token is injected as GH_TOKEN in the env
 *     (used by the long-lived editor path until it migrates). */
#define GIT_CRED_TOKEN_TARGET_FD 21 /* fd number the askpass reads in the git child */
char **git_cred_inject_build_env_for_repo(const char *principal, const char *remote_url,
                                          const char *repo_dir, const char *preferred_token,
                                          char *const *parent_environ, int *out_token_fd);

/* Back-compat shim: build the env for `principal` with no repo context — webuser
 * vault → server identity → ssh-agent (no per-host token). Equivalent to
 * git_cred_inject_build_env_for_repo(principal, NULL, NULL, NULL, parent). */
char **git_cred_inject_build_env(const char *principal, char *const *parent_environ);

/* Resolve just the HTTPS token (no env assembly) under the SAME precedence as
 * the env builders above: preferred → per-host vault → principal vault → server
 * identity. For the one caller that needs the raw token rather than an exec env
 * — the in-process GitHub REST open-PR, which puts it in an Authorization header
 * (git_pr_api.c). Writes the token to `out` and returns 1, or 0 (no token, out
 * empty). Centralizing here keeps the precedence from drifting and keeps the
 * credential ladder out of downstream callers. */
int git_cred_inject_resolve_token(const char *principal, const char *remote_url,
                                  const char *repo_dir, const char *preferred_token, char *out,
                                  size_t cap);

/* Free an envp from git_cred_inject_build_env (zeroes the GH_TOKEN entry first). */
void git_cred_inject_free_env(char **envp);

#endif /* GIT_CRED_INJECT_H */
