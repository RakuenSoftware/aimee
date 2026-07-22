#ifndef GIT_OAUTH_GITHUB_H
#define GIT_OAUTH_GITHUB_H 1

#include <stddef.h>

/* git_oauth_github — "Sign in with GitHub" via the OAuth device flow, to populate
 * the github.com entry of the per-host credential store (git_host_cred) without
 * the user creating a PAT by hand. Uses a GitHub OAuth App with device flow
 * enabled (the client_id is public, no secret needed): resolved from the UI-set
 * value, else AIMEE_GITHUB_OAUTH_CLIENT_ID, else the built-in default baked into
 * the build (oauth_defaults.h) so a distribution can ship one and every deployment
 * signs in with no setup. Other providers (Gitea/GitLab/...) use the token field
 * or the generic device flow (git_oauth_device). */

/* 1 iff a client ID is configured (stored from the UI, or the env) — flow usable. */
int git_oauth_github_available(void);

/* Store the GitHub OAuth App client ID (set from the web UI; persisted in the
 * server vault). Returns 0 on success, -1 on error. The client ID is public. */
int git_oauth_github_set_client_id(const char *client_id);

/* Read the configured client ID into out (the stored value, else the env).
 * Returns 1 (found) or 0 (none). */
int git_oauth_github_get_client_id(char *out, size_t out_len);

/* Begin device flow: request a user code. On success returns 0 and fills
 * user_code (shown to the user) + verify_uri (where they enter it) + *interval
 * (poll seconds). The opaque device_code is held server-side for poll(). Returns
 * -1 with err on failure (not configured / network / bad response). */
int git_oauth_github_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                           size_t vu_len, int *interval, char *err, size_t errlen);

/* Poll for completion. Returns 1 (done — token stored as host:github.com), 0
 * (still pending), or -1 (error/expired, err filled). */
int git_oauth_github_poll(const char *principal, char *err, size_t errlen);

/* ── Web (authorization-code) flow ──────────────────────────────────────────
 * The seamless "click → GitHub authorize → back, logged in" flow. Unlike the
 * device flow it needs a client SECRET (so it cannot ship in a public image; set
 * it per deployment via the vault or AIMEE_GITHUB_OAUTH_CLIENT_SECRET). When a
 * secret is present the UI uses this flow; otherwise it falls back to the device
 * flow above. */

/* 1 iff the web flow is usable (both a client ID and a client secret configured). */
int git_oauth_github_web_available(void);

/* Store the GitHub OAuth App client SECRET (write-only; sealed in the server
 * vault). Returns 0 on success, -1 on error. */
int git_oauth_github_set_client_secret(const char *client_secret);

/* Begin the web flow: stash a CSRF `state` bound to `principal` + `redirect_uri`
 * and write the GitHub authorize URL to out_url (the browser is sent there).
 * `redirect_uri` must match the OAuth App's registered callback URL. Returns 0, or
 * -1 with err. */
int git_oauth_github_web_start(const char *principal, const char *redirect_uri, char *out_url,
                               size_t url_len, char *err, size_t errlen);

/* Complete the web flow: validate `state` for `principal`, exchange `code` (+ the
 * client secret + the stashed redirect_uri) for an access token, and store it as
 * the github.com credential. Returns 0 (done, token stored) or -1 (err filled). */
int git_oauth_github_web_callback(const char *principal, const char *code, const char *state,
                                  char *err, size_t errlen);

#endif /* GIT_OAUTH_GITHUB_H */
