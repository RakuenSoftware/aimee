/* kb_enroll.h: zero-config enrollment primitives for aimee-kb's remote
 * (distributed-mode) auth — the opaque single-use enrollment token and the
 * `aimee://` connection string an operator pastes once.
 *
 * Phase 1 of the distributed-mode-auth proposal: the token lifecycle and the
 * connection-string codec. The internal CA / mTLS client-cert issuance
 * (`pki.c`), the `POST /v1/enroll` endpoint, and the JWKS/OIDC verifier are
 * later phases. The registry here is in-memory (token hashes only); DB2
 * persistence (proposal invariant 3) is a follow-up.
 *
 * Security model: tokens are 256-bit opaque random values, base64url-encoded;
 * aimee-kb stores ONLY sha256(token) + metadata in Vault, never the cleartext. Token
 * checks are constant-time. Enrollment tokens are single-use (replay-rejected). */
#ifndef DEC_KB_ENROLL_H
#define DEC_KB_ENROLL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define KB_ENROLL_TOKEN_RAW_BYTES 32 /* 256-bit opaque token */
#define KB_ENROLL_TOKEN_MAX       64 /* base64url(32B)=43 chars + slack/NUL */
#define KB_ENROLL_HASH_HEX        65 /* sha256 lowercase hex (64) + NUL */
#define KB_ENROLL_SCOPE_MAX       128

   /* Generate a fresh 256-bit opaque enrollment token, base64url-encoded
    * (no padding), into out[cap] (cap >= KB_ENROLL_TOKEN_MAX). Returns the token
    * length (excluding NUL), or -1 on error (small buffer / RNG failure). */
   int kb_enroll_token_generate(char *out, size_t cap);

   /* Write sha256(token) as lowercase hex into hex_out[cap]
    * (cap >= KB_ENROLL_HASH_HEX). aimee-kb persists ONLY this hash, never the
    * cleartext token. Returns 0 on success, -1 on error. */
   int kb_enroll_token_hash(const char *token, char *hex_out, size_t cap);

   /* Constant-time check that `presented` hashes (sha256, lowercase hex) to
    * `stored_hex`. Returns 1 on match, 0 otherwise. Safe against timing
    * side-channels (the compare does not short-circuit on the first mismatch). */
   int kb_enroll_token_verify(const char *presented, const char *stored_hex);

   /* Parsed `aimee://` connection string. */
   typedef struct
   {
      char host[256]; /* host part (no port) */
      int port;
      char ca_sha256[80]; /* CA fingerprint hex (the value after "sha256:") */
      char enroll_token[KB_ENROLL_TOKEN_MAX];
   } kb_enroll_conn_t;

   /* Build `aimee://<host>:<port>?ca=sha256:<fp>&enroll=<token>` into out[cap].
    * Returns the string length (excluding NUL), or -1 if it does not fit or an
    * argument is invalid (empty host, non-positive port, empty fp/token). */
   int kb_enroll_conn_string_build(const char *host, int port, const char *ca_sha256,
                                   const char *enroll_token, char *out, size_t cap);

   /* Parse `aimee://host:port?ca=sha256:fp&enroll=token` into *out. Returns 0 on
    * success, -1 on malformed input (wrong scheme, missing host/port/ca/enroll,
    * or a non-`sha256:` ca prefix). Query params may appear in any order. */
   int kb_enroll_conn_string_parse(const char *s, kb_enroll_conn_t *out);

   /* --- Single-use enrollment-token registry (in-memory; DB2 persistence is a
    *     later phase). Stores only token hashes + scope, never cleartext. --- */

   /* Issue a fresh enrollment token bound to `scope` (e.g. "global",
    * "project:X"): generate the token, record sha256(token)+scope as unconsumed,
    * and return the cleartext token (the only time it is available) in out[cap]
    * (cap >= KB_ENROLL_TOKEN_MAX). Returns 0 on success, -1 on error. */
   int kb_enroll_registry_issue(const char *scope, char *out, size_t cap);

   /* Verify and atomically consume a presented enrollment token. On the first
    * presentation of a known, unconsumed token: returns 1 and copies its scope
    * into scope_out[scope_cap]. On replay / unknown / already-consumed token:
    * returns 0. Single-use is enforced under the registry lock. */
   int kb_enroll_registry_consume(const char *token, char *scope_out, size_t scope_cap);

   /* Drop all registry entries (test helper / shutdown). */
   void kb_enroll_registry_reset(void);

   /* --- Vault-backed single-use enrollment-token store (persistent + process-
    *     shared). `path` identifies an empty flock inode; its hash namespaces an
    *     encrypted server-Vault record, so a token minted by `aimee-kb enroll`
    *     is redeemable by the daemon and survives restart without leaving its
    *     verifier outside Vault. Legacy text records are migrated in place. --- */

   /* Ingest an existing legacy hash registry into Vault and securely truncate
    * its file. Missing path is a successful no-op. */
   int kb_enroll_store_migrate(const char *path);

   /* Issue a fresh enrollment token bound to `scope`, appending
    * sha256(token)+scope (unconsumed) to the Vault record coordinated by `path`
    * if absent). Returns the cleartext token in out[cap] (cap >=
    * KB_ENROLL_TOKEN_MAX). Returns 0 on success, -1 on error (RNG / I/O / a
    * scope containing a tab or newline / oversized scope). */
   int kb_enroll_store_issue(const char *path, const char *scope, char *out, size_t cap);

   /* Verify + atomically consume a presented token against the store at `path`.
    * On the first presentation of a known, unconsumed token: returns 1 and
    * copies its scope into scope_out[scope_cap]. On replay / unknown / missing
    * store: returns 0. The read-mark-rewrite runs under an exclusive flock so
    * single-use holds even across concurrent processes. */
   int kb_enroll_store_consume(const char *path, const char *token, char *scope_out,
                               size_t scope_cap);

   /* --- one-shot enrollment mint (composes the CA + token store + codec) --- */

   /* Mint an enrollment under `data_dir`: load-or-create the internal CA in
    * <data_dir>/kb-ca (so it persists across restarts), issue a fresh single-use
    * enrollment token bound to `scope` into the Vault-backed registry coordinated
    * by the empty <data_dir>/kb-enroll-tokens lock inode, and
    * build the `aimee://<host>:<port>?ca=sha256:<fp>&enroll=<token>` connection
    * string (the value an operator hands a client) into out[cap]. The CA
    * fingerprint pins the CA for TOFU. Returns 0 on success, -1 on error (bad
    * args, CA / store / codec failure). This is the core of `aimee-kb enroll`. */
   int kb_enroll_mint(const char *data_dir, const char *host, int port, const char *scope,
                      char *out, size_t cap);

   /* Redeem an enrollment token (the client side of `kb_enroll_mint`). Consume
    * the single-use token from the store under `data_dir`, and — if it was valid
    * and unconsumed — issue a client certificate signed by the persistent CA in
    * <data_dir>/kb-ca, with the token's scope as the certificate subject (so the
    * mTLS layer can later derive scope from the verified cert). Writes the
    * granted scope into scope_out, the client cert PEM into cert_pem_out, and its
    * private key PEM into key_pem_out (the client keeps both). `valid_secs` is the
    * client-cert lifetime. Returns 0 on success, -1 on an invalid / replayed /
    * unknown token or any CA / issuance error. Single-use: a token redeems once. */
   int kb_enroll_redeem(const char *data_dir, const char *token, long valid_secs, char *scope_out,
                        size_t scope_cap, char *cert_pem_out, size_t cert_cap, char *key_pem_out,
                        size_t key_cap);

   /* CSR-based redemption (the secure variant: the client's private key NEVER
    * leaves it). Consume the single-use token, then sign the client-supplied
    * PEM CSR with the persistent CA, binding the CSR's public key to the token's
    * scope (server-controlled subject; the CSR's own subject is ignored). Writes
    * the granted scope into scope_out and the client cert PEM into cert_pem_out
    * (NO private key — the client already holds it). The CSR is validated BEFORE
    * the token is consumed, so a malformed CSR does not burn the token. Returns 0
    * on success, -1 on a bad CSR / invalid-replayed token / CA / issuance error. */
   int kb_enroll_redeem_csr(const char *data_dir, const char *token, const char *csr_pem,
                            long valid_secs, char *scope_out, size_t scope_cap, char *cert_pem_out,
                            size_t cert_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_ENROLL_H */
