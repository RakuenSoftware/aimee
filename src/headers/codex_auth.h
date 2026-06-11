/* codex_auth.h: read the local Codex CLI OAuth credentials.
 *
 * The Codex CLI stores its ChatGPT-OAuth tokens in ~/.codex/auth.json (and
 * refreshes them in place). For the thin-client model the engine runs on a
 * remote server that has no such file, so the client reads the live token here
 * and supplies it per request. Pure (file + cJSON), no DB/agent dependency, so
 * it links into both the thin client and the server.
 */
#ifndef CODEX_AUTH_H
#define CODEX_AUTH_H

#include <stddef.h>

/* Read this machine's Codex OAuth creds from ~/.codex/auth.json: `token` <-
 * tokens.access_token (top-level access_token fallback), `account_id` <-
 * tokens.account_id (top-level fallback). Either out pointer may be NULL.
 * Returns 0 when an access token was found (account_id may be empty), -1
 * otherwise. */
int codex_local_auth(char *token, size_t token_len, char *account_id, size_t account_id_len);

#endif /* CODEX_AUTH_H */
