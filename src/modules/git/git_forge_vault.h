#ifndef GIT_FORGE_VAULT_H
#define GIT_FORGE_VAULT_H 1

#include <stddef.h>

/* git_forge_vault — the convention + autonomous accessor for a webchat user's
 * git credentials in the sealed per-principal vault (webchat-git WP-B).
 *
 * Credential INTAKE already exists end-to-end: a browser user stores a git
 * credential via webchat `/api/vault/credentials` (POST {agent,cred,secret}) →
 * `/v1/vault/set` → vault_service_set(`webuser:<name>`, agent, cred, secret),
 * which DUAL-wraps the secret under the user KEK and the server KEK. The browser
 * never holds or receives the secret.
 *
 * This module pins the canonical (agent, cred) NAMES git uses, and reads them
 * back via the SERVER wrap — i.e. autonomously, with no client unlock / cached
 * user KEK — so credential injection (WP-C: GIT_ASKPASS + ssh-agent) and git
 * operations (WP-D/E) work for background clones and for code-server sessions
 * after the user's login KEK has expired. The credential stays isolated to the
 * owning `webuser:` principal's vault namespace. */

/* Canonical vault names. A git HTTPS token (PAT / forge App token / `gh` token)
 * and an SSH private key live under the "git" agent. */
#define GIT_FORGE_VAULT_AGENT "git"
#define GIT_FORGE_TOKEN_CRED  "forge_token"
#define GIT_FORGE_SSHKEY_CRED "ssh_key"

/* Read `principal`'s git HTTPS token into out[out_len] via the server wrap.
 * Returns 1 if a token was written, 0 if none is stored (caller falls back to
 * ambient creds), -1 on a fail-closed crypto/IO error. `out` is empty on a
 * non-1 return. */
int git_forge_vault_token(const char *principal, char *out, size_t out_len);

/* As above for the SSH private key (PEM). Same return contract. */
int git_forge_vault_sshkey(const char *principal, char *out, size_t out_len);

/* Read the server's own static forge token from the server-principal vault.
 * Same 1/0/-1 contract as the per-principal accessors. */
int git_forge_vault_server_token(char *out, size_t out_len);

/* The commit identity, under the same "git" agent, in the SERVER principal's
 * vault. Supplied at installation as AIMEE_GIT_AUTHOR_NAME / _EMAIL and sealed by
 * the first-boot env bootstrap, like every other install-time secret.
 *
 * git_ops.c points GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM at /dev/null on
 * purpose, so a commit carries the identity aimee supplies or it carries none
 * and fails. This is that identity; git_identity_resolve below falls back to the
 * checkout's own user.name/user.email when it is unset. */
#define GIT_AUTHOR_NAME_CRED  "author_name"
#define GIT_AUTHOR_EMAIL_CRED "author_email"

/* Read the configured commit identity. Returns 1 when BOTH a name and an email
 * were written, 0 when the identity is not configured, -1 on a fail-closed
 * crypto/IO error. Both buffers are empty on a non-1 return: a half-configured
 * identity is not an identity, so it reports 0 rather than committing as half a
 * person. */
int git_identity_get(char *name_out, size_t name_len, char *email_out, size_t email_len);

/* Resolve the commit identity for a checkout: the sealed vault identity above
 * when present, otherwise the identity the OPERATOR has already configured for
 * that repository (`user.name` / `user.email`).
 *
 * The vault seal is an install-time step, so requiring it as the only source
 * stranded any already-running agent that reached a commit with a clean vault:
 * it could only stop and ask a human to re-run installation. The operator's own
 * git identity is not a persona aimee invented — it is the exact identity a
 * plain `git commit` in that checkout would use — so preferring the seal and
 * falling back to it keeps the "commit as the configured operator, or not at
 * all" rule while letting the work proceed.
 *
 * Only `user.name` and `user.email` are read, and only as data to be passed
 * back through `git -c`. That does not reintroduce what git_ops.c's
 * GIT_CONFIG_GLOBAL/SYSTEM nulling exists to prevent: ambient config changing
 * how git BEHAVES (insteadOf rewrites, hooks, credential helpers).
 *
 * Same 1/0/-1 contract as git_identity_get, and the same all-or-nothing rule:
 * a name without an email is not an identity. |repo_dir| may be NULL to use the
 * current directory. */
int git_identity_resolve(const char *repo_dir, char *name_out, size_t name_len, char *email_out,
                         size_t email_len);

/* Read one git config |key| into |out|; return 1 when a non-empty value was
 * read, 0 otherwise. |ud| is the caller's context. */
typedef int (*git_config_reader_fn)(const char *key, char *out, size_t out_len, void *ud);

/* Full resolution, in precedence order:
 *
 *   1. |principal|'s OWN sealed identity, if a principal is given. Distinct
 *      users sharing one server must commit as themselves, so a per-principal
 *      identity wins — the same layering the forge token already uses
 *      (per-webuser credential first, server's own identity second).
 *   2. the server's sealed identity (the single-operator install case).
 *   3. |read_cfg|, the caller-supplied config lookup.
 *
 * The config lookup must run WHERE THE COMMIT WILL RUN. A caller whose git
 * commands go through a workspace provider (the MCP git tools) does NOT execute
 * in the server process's own working directory, so resolving config in-process
 * would consult a directory that is not the checkout — and silently find
 * nothing. Such a caller passes a reader routed through the same runner it
 * commits with; a caller holding a real on-disk path can use
 * git_identity_resolve above. Pass NULL for either to skip that tier.
 *
 * Same 1/0/-1 contract, and every tier is all-or-nothing: a name without an
 * email does not resolve, and does not borrow the next tier's email. */
int git_identity_resolve_with(const char *principal, git_config_reader_fn read_cfg, void *ud,
                              char *name_out, size_t name_len, char *email_out, size_t email_len);

#endif /* GIT_FORGE_VAULT_H */
