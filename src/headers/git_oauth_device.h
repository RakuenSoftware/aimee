#ifndef GIT_OAUTH_DEVICE_H
#define GIT_OAUTH_DEVICE_H 1

#include <stddef.h>

/* git_oauth_device — provider-generic OAuth 2.0 Device Authorization Grant for
 * hosts OTHER than GitHub (GitHub keeps its dedicated git_oauth_github module).
 * Currently: GitLab and Gitea/Forgejo, both of which implement the standard device
 * grant. A completed flow stores the resulting access token in the per-host
 * credential store (git_host_cred) so clone/enumeration reuse it — exactly like the
 * GitHub flow. Bitbucket has no device grant and stays PAT-only.
 *
 * The client_id (public — device flow needs no secret) is registered per
 * (provider, host): a self-hosted GitLab/Gitea each has its own OAuth application.
 *
 * NOTE: the GitLab/Gitea endpoints follow each provider's documented device-flow
 * spec but have not been exercised against a live instance in this codebase; treat
 * the live handshake as validation-pending. */

typedef enum
{
   OAUTH_DEV_GITLAB = 0,
   OAUTH_DEV_GITEA,
} oauth_dev_provider_t;

/* Parse a provider name ("gitlab" | "gitea"/"forgejo") into *out. Returns 0 on
 * success, -1 if the name is not a device-flow provider handled here. */
int oauth_dev_provider_from_name(const char *name, oauth_dev_provider_t *out);

/* Canonical provider name ("gitlab" | "gitea"). */
const char *oauth_dev_provider_name(oauth_dev_provider_t p);

/* 1 iff a client_id is registered for (provider, host) — the flow is usable. */
int oauth_dev_available(oauth_dev_provider_t p, const char *host);

/* Store / read the OAuth App client_id for (provider, host) in the server vault.
 * set returns 0/-1; get returns 1 (found, out filled) or 0 (none). */
int oauth_dev_set_client_id(oauth_dev_provider_t p, const char *host, const char *client_id);
int oauth_dev_get_client_id(oauth_dev_provider_t p, const char *host, char *out, size_t cap);

/* Begin device flow for (provider, host). On success returns 0 and fills user_code
 * + verify_uri + *interval; the opaque device_code is held server-side for poll().
 * Returns -1 with err on failure. */
int oauth_dev_start(oauth_dev_provider_t p, const char *host, const char *principal,
                    char *user_code, size_t uc_len, char *verify_uri, size_t vu_len, int *interval,
                    char *err, size_t errlen);

/* Poll the pending flow for (provider, host). Returns 1 (done — token stored under
 * the host in git_host_cred), 0 (still pending), or -1 (error/expired, err filled). */
int oauth_dev_poll(oauth_dev_provider_t p, const char *host, const char *principal, char *err,
                   size_t errlen);

/* --- Exposed for unit tests (pure, no network) --------------------------------- */

/* Build the device-authorization + token endpoint URLs and the credential host for
 * (provider, host) into the caller's buffers. `host` may be NULL/empty for a
 * provider with a default (gitlab.com); Gitea requires a host. Returns 0 on
 * success, -1 on a missing required host. */
int oauth_dev_endpoints(oauth_dev_provider_t p, const char *host, char *device_url,
                        size_t device_cap, char *token_url, size_t token_cap, char *cred_host,
                        size_t cred_cap);

#endif /* GIT_OAUTH_DEVICE_H */
