/* cli_agent_keys.h: thin-client local agent credential store + session push.
 *
 * Agent API keys live HERE, on the client machine (~/.config/aimee/
 * agent-keys.json), not on aimee-server. The client pushes them once per
 * "credential session" to the server's RAM-only keyring (POST
 * /v1/session/credentials); the server never persists them. Each chat/delegate
 * request carries a `cred_session_id` so the server resolves the right keyring
 * entry, decoupled from the functional chat session id.
 */
#ifndef CLI_AGENT_KEYS_H
#define CLI_AGENT_KEYS_H

#include "cJSON.h"
#include <stddef.h>

/* Store/replace (NULL/empty removes) an agent's API key in the local keyring
 * (~/.config/aimee/agent-keys.json, mode 0600). Returns 0 on success. */
int cli_agent_key_set(const char *agent_name, const char *api_key);

/* Load the local keyring as a cJSON object {agent_name: api_key, ...} (empty
 * object if none). Caller owns the result (cJSON_Delete). Used by `agent key
 * import` to migrate client-held keys into the server vault (P3). */
cJSON *cli_agent_keys_load(void);

/* `aimee agent key import [--scrub]` (P3): push each local keyring entry into the
 * server vault under the server principal via vault.set_server, reporting per
 * agent. --scrub removes an entry only after a confirmed store. Returns 0 unless
 * a non-refusal error occurred. */
int cli_agent_key_import(int argc, char **argv, int json_output);

/* For `aimee agent add <name> ... --key <K>` against a remote server: store K in
 * the LOCAL keyring (keyed by <name> = argv[1]) and strip `--key <K>` from argv
 * in place (decrementing *argc) so the key is never forwarded to / stored on the
 * server. argv[0] is "add". Returns 1 if a key was localized, 0 otherwise. */
int cli_agent_add_localize_key(int *argc, char **argv);

/* Stable per-client credential-session id (generated once into
 * ~/.config/aimee/cred-session.id, then reused). Fills `out`; returns 0. */
int cli_cred_session_id(char *out, size_t out_len);

/* Prime a chat/delegate request for client-held credentials when talking to a
 * remote tcp server: stamp `cred_session_id` on `req`, and (deduped, ~once per
 * session) push the local keyring + Codex creds to /v1/session/credentials so
 * the server can authenticate the agents without storing any key. No-op for a
 * co-located/unset endpoint. */
void cli_session_creds_prime(cJSON *req);

#endif /* CLI_AGENT_KEYS_H */
