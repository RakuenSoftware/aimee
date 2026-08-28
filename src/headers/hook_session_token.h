#ifndef AIMEE_HOOK_SESSION_TOKEN_H
#define AIMEE_HOOK_SESSION_TOKEN_H 1

#include <stddef.h>
#include <time.h>

#define HOOK_SESSION_TOKEN_HEX_LEN 64
#define HOOK_SESSION_TOKEN_CAP     (HOOK_SESSION_TOKEN_HEX_LEN + 1)

/* Server-side, process-local authority. Tokens are random bearer secrets bound
 * to all three identity dimensions; only their owning server process can
 * validate them and a restart invalidates every outstanding token. */
int hook_session_token_mint(const char *session_id, const char *client, const char *principal,
                            char out[HOOK_SESSION_TOKEN_CAP], time_t *expires_at);
int hook_session_token_verify(const char *session_id, const char *client, const char *principal,
                              const char *token);
void hook_session_token_revoke(const char *session_id, const char *client, const char *principal);
void hook_session_token_registry_reset(void); /* test/server-shutdown hygiene */

/* Thin-client persistence. The token is never printed or placed in the host
 * hook payload; independent hook processes rendezvous through this private
 * per-session file. */
int hook_session_token_store(const char *home, const char *session_id, const char *client,
                             const char *token);
int hook_session_token_load(const char *home, const char *session_id, const char *client,
                            char out[HOOK_SESSION_TOKEN_CAP]);
int hook_session_token_delete(const char *home, const char *session_id, const char *client);

#endif
