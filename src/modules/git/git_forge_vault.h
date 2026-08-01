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
 * aimee cannot fall back to the machine's git config: git_ops.c points
 * GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM at /dev/null on purpose, so a commit
 * carries the identity aimee supplies or it carries none and fails. This is that
 * identity. */
#define GIT_AUTHOR_NAME_CRED  "author_name"
#define GIT_AUTHOR_EMAIL_CRED "author_email"

/* Read the configured commit identity. Returns 1 when BOTH a name and an email
 * were written, 0 when the identity is not configured, -1 on a fail-closed
 * crypto/IO error. Both buffers are empty on a non-1 return: a half-configured
 * identity is not an identity, so it reports 0 rather than committing as half a
 * person. */
int git_identity_get(char *name_out, size_t name_len, char *email_out, size_t email_len);

#endif /* GIT_FORGE_VAULT_H */
