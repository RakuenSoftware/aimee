#ifndef WEBUSER_EDITOR_H
#define WEBUSER_EDITOR_H 1

#include <stddef.h>

/* webuser_editor — per-webuser code-server supervisor (webchat-git WP-I).
 *
 * aimee-server owns the editor lifecycle because it owns the workspace files AND
 * the sealed-vault credential-injection environment (WP-C/WP-C2). Each webuser
 * gets at most one code-server, bound to 127.0.0.1:<ephemeral-port>, rooted at
 * their scoped workspace (ws_scope_user_root), launched with the vault-backed
 * git env so the integrated terminal + Git view authenticate without ever
 * exposing a credential to the browser. webchat reverse-proxies the /vscode
 * path to the returned port (WP-J); the loopback port is never sent to the
 * browser.
 *
 * On by default, fail-closed: ensure() serves the editor unless
 * AIMEE_WEBCHAT_EDITOR=0 disables it, and returns 0 (unavailable) when no
 * code-server binary is present (a WITH_VSCODE=0 / dev build), so enabling it
 * never breaks an image that does not ship the editor. It binds only to loopback
 * and runs each child hardened (no core dump, no new privs, detached session,
 * stdio to /dev/null). */

#define WEBUSER_EDITOR_MAX 64 /* max concurrent per-user editors */

/* Ensure a live code-server for `principal` (must be "webuser:<name>"): spawn
 * one if none is running, otherwise reuse the existing one and refresh its
 * activity timestamp. On success writes the loopback port to *out_port and
 * returns 1. Returns 0 when the editor feature is disabled or unavailable, or -1
 * on error (err filled, NUL-terminated). Thread-safe. */
int webuser_editor_ensure(const char *principal, int *out_port, char *err, size_t errlen);

/* Refresh the activity timestamp for `principal`'s editor so the idle reaper
 * leaves an in-use session alone. Called by the proxy on each request. No-op if
 * the principal has no running editor. Thread-safe. */
void webuser_editor_touch(const char *principal);

/* Stop and reap `principal`'s editor (e.g. on logout). No-op if none. */
void webuser_editor_stop(const char *principal);

/* Kill every editor idle longer than idle_secs; returns the number reaped.
 * idle_secs <= 0 reaps none. Safe to call frequently. */
int webuser_editor_reap_idle(long idle_secs);

/* Stop every running editor (server shutdown). */
void webuser_editor_shutdown(void);

/* Whether the editor feature is enabled (on unless AIMEE_WEBCHAT_EDITOR=0) AND a
 * code-server binary is resolvable. Exposed for the route layer / tests. */
int webuser_editor_available(void);

/* Build the sanitized, fully-owned environment a webuser's code-server is
 * launched with (webchat-git WP-K): a minimal curated base (PATH/LANG/TERM) +
 * HOME=<userroot> + the user's vault-backed git env (GH_TOKEN/GIT_ASKPASS/
 * SSH_AUTH_SOCK when present) + git's on-disk credential cache disabled. It NEVER
 * contains the server's own environment, so the editor's terminal/extensions
 * cannot read server secrets (AIMEE_DB2_URL, AIMEE_SERVER_TOKEN, provider keys).
 * Returns a malloc'd NULL-terminated array (free with webuser_editor_free_env),
 * or NULL on error. Exposed for the WP-K editor-env leak tests. */
char **webuser_editor_build_env(const char *principal, const char *userroot);

/* Free an envp from webuser_editor_build_env (zeroes any GH_TOKEN first). */
void webuser_editor_free_env(char **envp);

#endif /* WEBUSER_EDITOR_H */
