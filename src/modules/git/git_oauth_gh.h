#ifndef GIT_OAUTH_GH_H
#define GIT_OAUTH_GH_H 1

#include <stddef.h>

/* git_oauth_gh — "Sign in with GitHub" through the bundled GitHub CLI (`gh`),
 * the zero-config fallback used when no OAuth App client ID is configured
 * (git_oauth_github). `gh auth login` runs GitHub's device flow under gh's own
 * public app identity, so sign-in works out of the box with no app
 * registration; the GitHub consent screen names "GitHub CLI".
 *
 * The login runs on a server-side PTY (gh insists on a terminal) with an
 * ephemeral GH_CONFIG_DIR. On success the token is moved into the per-host
 * credential store (git_host_cred) and gh's on-disk state is deleted, so the
 * vault stays the only place the token lives. Single-user server: one pending
 * sign-in at a time, mirroring git_oauth_github. */

/* 1 iff the gh binary is on PATH — the fallback is usable. */
int git_oauth_gh_available(void);

/* 1 iff a gh sign-in session is in flight (start()ed, not yet consumed by
 * poll()). Route dispatch uses this to steer poll traffic to the right flow. */
int git_oauth_gh_pending(void);

/* Begin a sign-in: spawn gh, wait for it to print the one-time code, and fill
 * user_code + verify_uri + *interval (poll seconds). Same contract as
 * git_oauth_github_start. Idempotent while a session is pending: re-issues the
 * same still-valid code. Returns 0, or -1 with err filled. */
int git_oauth_gh_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                       size_t vu_len, int *interval, char *err, size_t errlen);

/* Poll for completion. Returns 1 (done — token stored as host:github.com), 0
 * (still pending), or -1 (error/expired, err filled). */
int git_oauth_gh_poll(const char *principal, char *err, size_t errlen);

/* ── Test seams (the pure parsing helpers the PTY pump depends on) ──────────── */

/* Strip ANSI escape sequences (CSI + OSC) and carriage returns in place. */
void git_oauth_gh_strip_term_noise(char *s);

/* Find the XXXX-XXXX one-time code after gh's "one-time code" marker. Returns
 * 1 + fills out (cap must be >= 10), or 0. */
int git_oauth_gh_parse_code(const char *text, char *out, size_t cap);

#endif /* GIT_OAUTH_GH_H */
