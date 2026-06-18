#ifndef GIT_OAUTH_GITHUB_H
#define GIT_OAUTH_GITHUB_H 1

#include <stddef.h>

/* git_oauth_github — "Sign in with GitHub" via the OAuth device flow, to populate
 * the github.com entry of the per-host credential store (git_host_cred) without
 * the user creating a PAT by hand. Requires AIMEE_GITHUB_OAUTH_CLIENT_ID (a
 * registered GitHub OAuth App with device flow enabled — the client_id is public,
 * no secret needed for device flow). Other providers (Gitea/GitLab/...) use the
 * token field instead. */

/* 1 iff AIMEE_GITHUB_OAUTH_CLIENT_ID is configured (the flow is usable). */
int git_oauth_github_available(void);

/* Begin device flow: request a user code. On success returns 0 and fills
 * user_code (shown to the user) + verify_uri (where they enter it) + *interval
 * (poll seconds). The opaque device_code is held server-side for poll(). Returns
 * -1 with err on failure (not configured / network / bad response). */
int git_oauth_github_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                           size_t vu_len, int *interval, char *err, size_t errlen);

/* Poll for completion. Returns 1 (done — token stored as host:github.com), 0
 * (still pending), or -1 (error/expired, err filled). */
int git_oauth_github_poll(const char *principal, char *err, size_t errlen);

#endif /* GIT_OAUTH_GITHUB_H */
