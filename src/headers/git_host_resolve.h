#ifndef GIT_HOST_RESOLVE_H
#define GIT_HOST_RESOLVE_H 1

#include <stddef.h>

/* git_host_resolve — resolve the per-host vault token for a git operation.
 *
 * The per-host credential store (git_host_cred, server vault) is reached through
 * a REGISTERED seam rather than a hard link dependency, so the credential paths
 * that need a per-host token (the webuser resolver in git_cred_inject, and the
 * operator MCP / mirror git runners) all share ONE lookup policy without dragging
 * the vault stack into every test binary. The server registers the real lookup
 * at startup; unregistered (thin client / unit tests) the per-host step is simply
 * skipped and resolution falls through to the next credential source. */

/* Register the per-host vault lookup (git_host_cred_for_url). Called once by the
 * server at startup. A second call replaces the first; NULL disables it. */
void git_host_resolve_register(int (*host_cred_for_url)(const char *url, char *out,
                                                        size_t out_len));

/* Resolve the per-host vault token for the repo, keyed by its remote host. The
 * host comes from `remote_url` when given, else from the `origin` remote of
 * `repo_dir` (a single local `git config --get remote.origin.url` — no network,
 * no creds). Returns 1 + out (token, NUL-terminated) on a hit, 0 otherwise (no
 * lookup registered, no remote resolvable, or no stored token). `out` is always
 * either a full token or empty — never a partial value. */
int git_host_resolve_token(const char *remote_url, const char *repo_dir, char *out, size_t out_len);

#endif /* GIT_HOST_RESOLVE_H */
