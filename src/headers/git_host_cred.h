#ifndef GIT_HOST_CRED_H
#define GIT_HOST_CRED_H 1

#include <stddef.h>

/* git_host_cred — per-git-host credential store for the single-user server.
 *
 * aimee-server is one person's agent, but it talks to MANY git hosts/providers
 * (github.com, gitea.example.com, gitlab.com, self-hosted, ...). Credentials are
 * therefore keyed by HOST, not by user, and persisted in aimee-server's OWN
 * vault (server principal, server-wrap → auto-decryptable, no unlock). The token
 * is provider-agnostic: an HTTPS access token authenticates GitHub, Gitea,
 * GitLab and Bitbucket alike. A clone/op resolves the token for its URL's host. */

#define GIT_HOST_MAX 256

/* Store (or replace) the access token for `host` (e.g. "github.com"). Returns 0
 * on success, -1 on error (bad args / vault failure). */
int git_host_cred_set(const char *host, const char *token);

/* Look up the token for `host`. Returns 1 (found, out filled + NUL-terminated),
 * 0 (none configured), -1 (error). */
int git_host_cred_get(const char *host, char *out, size_t out_len);

/* Resolve the token for any git URL form by its host (git@h:..,, ssh://, https://
 * ...). Returns 1/0/-1 as git_host_cred_get; 0 if the host can't be parsed. */
int git_host_cred_for_url(const char *url, char *out, size_t out_len);

/* Delete the stored token for `host`. Returns 0 (deleted or absent), -1 on error. */
int git_host_cred_delete(const char *host);

/* Extract the lowercased host from any git URL form into `out`. Returns 1 on
 * success, 0 on failure. Exposed for the set-from-URL path + tests. */
int git_host_from_url(const char *url, char *out, size_t out_len);

#endif /* GIT_HOST_CRED_H */
