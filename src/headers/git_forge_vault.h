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

#endif /* GIT_FORGE_VAULT_H */
