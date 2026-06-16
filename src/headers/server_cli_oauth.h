/* server_cli_oauth.h: server-hosted OAuth CLI agents (claude / codex).
 *
 * Installs the vendor CLI on the aimee-server host (on demand, into the
 * home-volume npm prefix) and drives its OAuth login server-side in a tmux
 * session, surfacing the verification URL + code to the operator (no PTY
 * forwarding). See docs/proposals/pending/server-hosted-oauth-cli-agents.md for
 * the security contract this implements (argv-only, vendor allowlist, pinned +
 * --ignore-scripts install, 0700/0600 token custody, scrape URL+code only,
 * per-vendor lock, private tmux socket, hard timeout).
 */
#ifndef DEC_SERVER_CLI_OAUTH_H
#define DEC_SERVER_CLI_OAUTH_H 1

#include <stddef.h>

/* The closed set of supported vendors. Parsed from an untrusted string ONLY via
 * cli_oauth_vendor_parse before any process is spawned. */
typedef enum
{
   CLI_OAUTH_CLAUDE = 0,
   CLI_OAUTH_CODEX = 1
} cli_oauth_vendor_t;

/* Parse "claude" / "codex" (also accepts the "-oauth" suffix). Returns 0 and sets
 * *out on success, -1 for any other value. */
int cli_oauth_vendor_parse(const char *s, cli_oauth_vendor_t *out);

/* Canonical vendor name ("claude" / "codex"). Never NULL. */
const char *cli_oauth_vendor_name(cli_oauth_vendor_t v);

/* The agent name a successful setup registers ("claude" / "codex"). */
const char *cli_oauth_agent_name(cli_oauth_vendor_t v);

/* Login state surfaced to the client across polls. */
typedef enum
{
   CLI_OAUTH_PENDING = 0,   /* waiting on the user's browser step */
   CLI_OAUTH_AUTHENTICATED, /* token persisted; ready to register */
   CLI_OAUTH_FAILED         /* error / timeout / expired code */
} cli_oauth_state_t;

/* Result of starting a login: the verification URL and (codex) one-time code to
 * surface to the operator, plus the tmux session token for follow-up calls.
 * Buffers are caller-provided and NUL-terminated. */
typedef struct
{
   char url[1024];
   char code[64]; /* codex device code; empty for claude (it pastes a code BACK) */
   char session[64];
   int needs_code_back; /* 1 for claude (operator pastes a code back via _submit) */
} cli_oauth_start_t;

/* Ensure the vendor CLI is installed (idempotent: a version probe, then a pinned
 * `npm i -g <pkg>@<ver> --ignore-scripts` if absent). Returns 0 on ready, -1 with
 * a redacted message in err. */
int cli_oauth_install(cli_oauth_vendor_t v, char *err, size_t errn);

/* Start the login: take the per-vendor lock, launch the vendor login command in a
 * private-socket tmux session, scrape the URL (+ codex code). Returns 0 and fills
 * *out, -1 with err. The op holds the lock until _submit/_poll completes, fails,
 * or times out. */
int cli_oauth_start(cli_oauth_vendor_t v, cli_oauth_start_t *out, char *err, size_t errn);

/* claude only: send the operator-supplied code back into the tmux login. `code`
 * is validated (charset/length) before send-keys. Returns 0, -1 with err. */
int cli_oauth_submit_code(cli_oauth_vendor_t v, const char *session, const char *code, char *err,
                          size_t errn);

/* Poll the login: inspect the session's auth state. Sets *state. Returns 0, -1 on
 * a hard error. Releases the lock + tears down the session on a terminal state. */
int cli_oauth_poll(cli_oauth_vendor_t v, const char *session, cli_oauth_state_t *state, char *err,
                   size_t errn);

/* --- pure helpers (exposed for tests) ------------------------------------- */

/* Copy the first https URL at/after `anchor` from a tmux pane into out (up to
 * whitespace). Returns 0 on a plausible URL, -1 otherwise. */
int cli_oauth_scrape_url(const char *pane, const char *anchor, char *out, size_t n);

/* Extract a codex device code (shaped XXXX-XXXXX) from a pane. 0 / -1. */
int cli_oauth_scrape_codex_code(const char *pane, char *out, size_t n);

/* 1 if `code` is a safe paste-back token (bounded url-safe charset, no control
 * chars); 0 otherwise. */
int cli_oauth_code_is_safe(const char *code);

#endif /* DEC_SERVER_CLI_OAUTH_H */
