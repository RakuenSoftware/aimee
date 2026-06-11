/* session_credentials.h: in-memory, session-scoped agent credential keyring.
 *
 * Security posture (see the client-held-credentials design): agent/delegate API
 * keys must NOT live long-term on aimee-server. The thin client pushes its keys
 * once per session; the server caches them HERE — in RAM only, keyed by session
 * id — and NEVER writes them to disk. Entries are evicted on session close or
 * after an idle TTL, so a compromised server holds no durable secret store.
 *
 * All functions are thread-safe.
 */
#ifndef SESSION_CREDENTIALS_H
#define SESSION_CREDENTIALS_H

#include <stddef.h>

/* Store/replace one agent's API key for a session. A NULL/empty key removes it.
 * No-op if session_id or agent_name is empty. */
void session_creds_set(const char *session_id, const char *agent_name, const char *api_key);

/* Store/replace the Codex OAuth creds (access token + ChatGPT account id) for a
 * session. NULL/empty token clears them. */
void session_creds_set_codex(const char *session_id, const char *token, const char *account_id);

/* Ingest a client-pushed JSON blob for a session:
 *   {"agents": {"<name>": "<key>", ...},
 *    "codex_oauth_token": "...", "codex_account_id": "..."}
 * Returns the number of credentials stored (>=0), or -1 on a parse error. */
int session_creds_ingest_json(const char *session_id, const char *json);

/* Look up an agent's key for a session. Returns 1 and fills `out` when found,
 * 0 otherwise. */
int session_creds_get(const char *session_id, const char *agent_name, char *out, size_t out_len);

/* Look up the session's Codex creds. Either out pointer may be NULL. Returns 1
 * when a token was found. */
int session_creds_get_codex(const char *session_id, char *token, size_t token_len, char *account_id,
                            size_t account_id_len);

/* Drop and zero all creds for a session (call on session close). */
void session_creds_clear(const char *session_id);

#endif /* SESSION_CREDENTIALS_H */
