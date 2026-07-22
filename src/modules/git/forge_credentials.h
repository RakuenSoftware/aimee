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

/* Install a token for `workspace_id`, with forge `scope` (one of "global",
 * "workspace", "project", "user" — the charter scope lattice), valid for
 * `ttl_seconds` from `now_epoch`. Replaces (zeroing) any existing token for the
 * workspace. The token is copied into a broker-owned heap buffer; the caller
 * should wipe its own copy after. Returns 0, or -1 on bad args / registry full. */
int forge_cred_install(const char *workspace_id, const char *token, const char *scope,
                       long ttl_seconds, long now_epoch);

/* Copy the live (unexpired) token for `workspace_id` into out[out_cap]. Returns
 * 0 on success, -1 if absent or expired (out is set to "" on failure). */
int forge_cred_get(const char *workspace_id, long now_epoch, char *out, size_t out_cap);

/* Copy the installed forge scope for `workspace_id` into out[out_cap] ("" if
 * absent). Returns 0 if a (non-expired) token exists, -1 otherwise. */
int forge_cred_scope(const char *workspace_id, long now_epoch, char *out, size_t out_cap);

/* 1 iff a token scoped `cred_scope` is broad enough to satisfy an operation
 * requiring `required_scope`, by lattice rank (global=0 broadest < workspace <
 * project < user). A broader credential satisfies a narrower requirement.
 * NULL/unknown either side => deny (0). Pure.
 *
 * NOTE: this gates the *forge token* against the operation. It is necessary but
 * not sufficient — the connection's *bearer* capabilities are checked
 * separately by the per-route capability matrix (server_auth.c /
 * server_http_route_caps), so a `project:read` bearer can never reach a push
 * path even if a broader forge token is installed. */
int forge_cred_scope_allows(const char *cred_scope, const char *required_scope);

/* Revoke (zero the buffer, then drop) the token for `workspace_id`. Idempotent.
 * Call on session/workspace close. */
void forge_cred_revoke(const char *workspace_id);

/* Revoke every installed token (server shutdown). */
void forge_cred_revoke_all(void);

/* Number of currently-installed (slot-occupied) tokens — for tests/introspection. */
int forge_cred_count(void);

/* Build a NULL-terminated envp for an execve() of a git/gh command acting on
 * `workspace_id`: a copy of the parent `parent_environ` plus GH_TOKEN=<token>
 * and (when `askpass_shim` is non-NULL) GIT_ASKPASS=<askpass_shim> and
 * GIT_TERMINAL_PROMPT=0. The token crosses only in the child's environment, so
 * it never touches disk and never appears in the command line (ps-safe). The
 * askpass shim is a tiny program the deployment ships that prints $GH_TOKEN.
 * Returns a malloc'd array (free with forge_cred_free_env), or NULL if there is
 * no live token for the workspace. `parent_environ` is usually `environ`. */
char **forge_cred_build_env(const char *workspace_id, long now_epoch, char *const *parent_environ,
                            const char *askpass_shim);

/* Free an envp returned by forge_cred_build_env (frees each entry, then the
 * array; zeroes the GH_TOKEN entry first so the secret does not linger). */
void forge_cred_free_env(char **envp);

/* Provision (once per process) the GIT_ASKPASS shim that git authenticates
 * through under a forge env: a tiny script that prints $GH_TOKEN (which the
 * built env carries) as the password and a conventional username. It holds NO
 * secret itself. Lives in the instance config dir; returns its stable path, or
 * NULL if it cannot be written. Pass it as the `askpass_shim` arg above. Shared
 * by every git call site that injects a forge env (mcp_git, the mirror tier). */
const char *forge_cred_askpass_shim(void);

/* --- Server-held forge identity (workspace-resource-plane §6) ----------------
 * A forge credential the SERVER itself holds, used for instance-held workspaces
 * when a filesystem-poor surface (e.g. telegram) drives a git op and supplies no
 * token of its own. The default source is the environment: AIMEE_FORGE_TOKEN
 * (the credential) and AIMEE_FORGE_SCOPE (default "workspace"). A production hub
 * points AIMEE_FORGE_TOKEN at a GitHub/GitLab App installation token that the
 * App machinery refreshes; this layer just consumes it. Like the per-workspace
 * tokens it is kept out of the command line and never logged. */

/* If a server identity is configured (AIMEE_FORGE_TOKEN non-empty), copy the
 * token into tok_out[tok_cap] and the scope into scope_out[scope_cap] and return
 * 1; otherwise clear the outputs and return 0. */
int forge_cred_server_identity(char *tok_out, size_t tok_cap, char *scope_out, size_t scope_cap);

/* Register an optional App installation-token provider (forge_app_token.c). When
 * registered AND `configured()` is true, forge_cred_server_identity sources the
 * token from `get()` instead of raw AIMEE_FORGE_TOKEN. The server registers this
 * at startup; leaving it unregistered (thin client / unit tests) keeps the raw
 * AIMEE_FORGE_TOKEN behavior unchanged. Decoupled via pointers so forge_credentials
 * (core, widely linked) carries no link dependency on the App-token module. */
void forge_cred_register_app_token_provider(int (*configured)(void), int (*get)(char *, size_t));

/* Build an exec envp carrying the SERVER identity (GH_TOKEN + the GIT_ASKPASS
 * shim), or NULL when no server identity is configured. Mirrors
 * forge_cred_build_env; the caller falls back to ambient creds on NULL. */
char **forge_cred_build_server_env(char *const *parent_environ, const char *askpass_shim);

#endif /* FORGE_CREDENTIALS_H */
