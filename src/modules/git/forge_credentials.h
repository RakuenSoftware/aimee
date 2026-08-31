#ifndef FORGE_CREDENTIALS_H
#define FORGE_CREDENTIALS_H 1

#include <stddef.h>

/* forge_credentials — the per-workspace short-lived forge-token broker
 * (workspace-resource-plane §4). The ONLY path by which a detached aimee-server
 * gets push/PR rights: a filesystem-rich client hands a short-lived, narrowly
 * scoped forge token (a GitHub/GitLab App installation token, a fine-grained
 * PAT, or `gh auth token`) over the authenticated /v1 channel; the server holds
 * it for the lifetime of the workspace and injects it into the git/gh exec
 * environment (GH_TOKEN + a GIT_ASKPASS shim that echoes it).
 *
 * Invariant: the token lives IN MEMORY ONLY — never written to disk, never
 * logged — and its buffer is explicitly zeroed on revoke / session close.
 *
 * This is deliberately distinct from:
 *   - delegate_credentials.c — leases *LLM-provider API keys* per agent.
 *   - the distributed-mode-auth bearer/mTLS — authenticates the
 *     client<->server / server<->kb channel, not the server<->forge channel.
 *
 * The broker is process-global and thread-safe. Time is passed in (now_epoch)
 * so the core is pure/testable; callers pass time(NULL). */


/* Provision (once per process) the GIT_ASKPASS shim that git authenticates
 * through under a forge env: a tiny script that prints $GH_TOKEN (which the
 * built env carries) as the password and a conventional username. It holds NO
 * secret itself. Lives in the instance config dir; returns its stable path, or
 * NULL if it cannot be written. Pass it as the `askpass_shim` arg above. Shared
 * by every git call site that injects a forge env (mcp_git, the mirror tier). */
const char *forge_cred_askpass_shim(void);

/* Absolute path to a helper that prints the session's git token on stdout, for
 * a user who needs it for something other than git (the editor terminal no
 * longer carries GH_TOKEN). Reads the same inherited descriptor the askpass
 * does, so the secret never enters an environment block. NULL if unavailable. */
const char *forge_cred_token_helper(void);

/* --- Server-held forge identity (workspace-resource-plane §6) ----------------
 * A forge credential the SERVER itself holds, used for instance-held workspaces
 * when a filesystem-poor surface (e.g. telegram) drives a git op and supplies no
 * token of its own. A static token is read through the registered server-Vault
 * provider; AIMEE_FORGE_TOKEN is accepted only by the first-boot Vault bootstrap
 * and is never a runtime source. AIMEE_FORGE_SCOPE is non-secret metadata. */

/* If a server identity is configured, copy the token into tok_out[tok_cap] and
 * scope into scope_out[scope_cap], returning 1; otherwise clear outputs and
 * return 0. */
int forge_cred_server_identity(char *tok_out, size_t tok_cap, char *scope_out, size_t scope_cap);

/* Register an optional App installation-token provider (forge_app_token.c). When
 * registered AND `configured()` is true, forge_cred_server_identity sources the
 * token from `get()` instead of raw AIMEE_FORGE_TOKEN. The server registers this
 * at startup; leaving it unregistered (thin client / unit tests) keeps the raw
 * static server-Vault provider. Decoupled via pointers so forge_credentials
 * (core, widely linked) carries no link dependency on either provider module. */
void forge_cred_register_app_token_provider(int (*configured)(void), int (*get)(char *, size_t));

/* Register the server-vault reader for the static forge identity. The server
 * installs this at boot; thin clients have no server identity. */
void forge_cred_register_static_token_provider(int (*get)(char *, size_t));


#endif /* FORGE_CREDENTIALS_H */
