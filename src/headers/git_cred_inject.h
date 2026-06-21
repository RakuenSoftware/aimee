#ifndef GIT_CRED_INJECT_H
#define GIT_CRED_INJECT_H 1

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
 * of any kind is available (caller falls back to ambient creds) or on error. */
char **git_cred_inject_build_env_for_repo(const char *principal, const char *remote_url,
                                          const char *repo_dir, const char *preferred_token,
                                          char *const *parent_environ);

/* Back-compat shim: build the env for `principal` with no repo context — webuser
 * vault → server identity → ssh-agent (no per-host token). Equivalent to
 * git_cred_inject_build_env_for_repo(principal, NULL, NULL, NULL, parent). */
char **git_cred_inject_build_env(const char *principal, char *const *parent_environ);

/* Free an envp from git_cred_inject_build_env (zeroes the GH_TOKEN entry first). */
void git_cred_inject_free_env(char **envp);

#endif /* GIT_CRED_INJECT_H */
