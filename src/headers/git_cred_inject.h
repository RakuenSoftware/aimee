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

/* Build a NULL-terminated envp for an execve() of a git command run as
 * `principal` (a `webuser:<name>`): a copy of `parent_environ` with any
 * inherited GH_TOKEN/GIT_ASKPASS dropped, plus GH_TOKEN=<vault token>,
 * GIT_ASKPASS=<shim>, GIT_TERMINAL_PROMPT=0. Returns a malloc'd array (free with
 * git_cred_inject_free_env), or NULL if the principal has no vaulted git token
 * (caller falls back to ambient creds) or on error. */
char **git_cred_inject_build_env(const char *principal, char *const *parent_environ);

/* Free an envp from git_cred_inject_build_env (zeroes the GH_TOKEN entry first). */
void git_cred_inject_free_env(char **envp);

#endif /* GIT_CRED_INJECT_H */
