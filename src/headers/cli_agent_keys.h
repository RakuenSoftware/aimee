/* cli_agent_keys.h: thin-client reader for the local agent-keys.json keyring,
 * used solely to MIGRATE any leftover client-held keys into the server vault.
 *
 * The credential vault on aimee-server is the single, permanent credential store
 * (the legacy per-session client push to /v1/session/credentials was retired in
 * P4b). `aimee agent key import` reads the local keyring here and pushes each
 * entry into the vault; nothing in the live request path consults this file.
 */
#ifndef CLI_AGENT_KEYS_H
#define CLI_AGENT_KEYS_H

#include "cJSON.h"
#include <stddef.h>

/* Store/replace (NULL/empty removes) an agent's API key in the local keyring
 * (~/.config/aimee/agent-keys.json, mode 0600). Used by `agent key import
 * --scrub` to drop an entry after it is confirmed stored in the vault. Returns 0
 * on success. */
int cli_agent_key_set(const char *agent_name, const char *api_key);

/* Load the local keyring as a cJSON object {agent_name: api_key, ...} (empty
 * object if none). Caller owns the result (cJSON_Delete). Used by `agent key
 * import` to migrate client-held keys into the server vault. */
cJSON *cli_agent_keys_load(void);

/* `aimee agent key import [--scrub]`: push each local keyring entry into the
 * server vault under the server principal via vault.set_server, reporting per
 * agent. --scrub removes an entry only after a confirmed store. Returns 0 unless
 * a non-refusal error occurred. */
int cli_agent_key_import(int argc, char **argv, int json_output);

#endif /* CLI_AGENT_KEYS_H */
