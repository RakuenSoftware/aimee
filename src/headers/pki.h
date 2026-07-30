/* pki.h: aimee's self-generated client-cert CA + issuance/revocation, for mTLS
 * client identity (mtls-client-identity slice 2).
 *
 * aimee owns a private CA whose key is sealed in the server vault and whose cert
 * is written to <config>/tls/client-ca.crt (the file server_tls verifies client
 * certs against). It issues short-lived client certs whose CN becomes a
 * cert:<CN> principal, and revokes them via an in-memory serial denylist
 * (snapshot-loaded from DB1) consulted in the TLS verify callback — no DB query
 * in the handshake hot path. All issuance/revocation is operator-gated upstream.
 */
#ifndef DEC_PKI_H
#define DEC_PKI_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Load-or-create the CA: if the sealed key + cert file exist, load them;
    * otherwise generate an EC P-256 CA, seal the key in the vault, and write the
    * cert to <config>/tls/client-ca.crt. Also (re)loads the revocation snapshot
    * from DB1. Idempotent. Returns 0 on success, -1 on failure (logged). */
   int pki_ca_ensure(void);

   /* Issue a client cert for `cn` (which must pass vault_principal_name_sanitize),
    * valid for `validity_days`, signed by the CA. Writes the cert PEM and key PEM
    * into the caller's buffers and the hex serial into serial_out, and records the
    * cert in DB1. Returns 0 on success, -1 on failure. */
   int pki_issue(const char *cn, int validity_days, char *cert_pem, size_t cert_len, char *key_pem,
                 size_t key_len, char *serial_out, size_t serial_len);

   int pki_sign_csr(const char *cn, int validity_days, const char *csr_pem, char *cert_pem,
                    size_t cert_len, char *serial_out, size_t serial_len);

   /* Revoke by hex serial (adds to the DB1 denylist + the in-memory snapshot).
    * Returns 0 on success (including already-revoked), -1 on error. */
   int pki_revoke(const char *serial);

   /* 1 if `serial` (hex) is revoked, else 0. Reads the in-memory snapshot only —
    * safe to call from the TLS verify callback. */
   int pki_is_revoked(const char *serial);

   /* Per-request durable cert status (P8a, invariant #5). Distinct from
    * pki_is_revoked: ERROR (an unavailable authority) is never conflated with
    * UNKNOWN (a cert this server's CA never issued) — both fail closed at the
    * caller, but are logged distinctly. */
   typedef enum
   {
      PKI_CERT_VALID = 0,
      PKI_CERT_REVOKED,
      PKI_CERT_EXPIRED,
      PKI_CERT_UNKNOWN,
      PKI_CERT_ERROR
   } pki_cert_status_t;

   /* Fresh, DURABLE per-request status for `serial` (BN_bn2hex form, as
    * pki_certs.serial stores it) at wall-clock `now`. Runs a single prepared
    * `SELECT revoked, expires_at FROM pki_certs WHERE serial=?` against DB1 on
    * every call — NOT the in-memory g_revoked snapshot — so a revocation written
    * to pki_certs is authoritative on the very next request, with no snapshot
    * staleness. This is the load-bearing check that closes the keep-alive-after-
    * revoke gap: mtls_verify_cb runs only at handshake and cannot be re-run on an
    * open connection. Precedence: a SQLite prepare/bind/step failure -> ERROR
    * (dominates); no matching row -> UNKNOWN; row with revoked!=0 -> REVOKED; row
    * with expires_at>0 AND expires_at<=now -> EXPIRED; else VALID. */
   pki_cert_status_t pki_cert_check(const char *serial, long now);

   /* Human-readable label for a pki_cert_status_t, for distinct-cause logging. */
   const char *pki_cert_status_str(pki_cert_status_t s);

   /* Durable optional->required mTLS ramp. Startup returns the effective mode
    * (never weaker than configured_mode). A valid per-request certificate check
    * may then record presentation; ready is a read-only preflight, and advance
    * repeats the same roster/hash test under BEGIN IMMEDIATE before committing
    * the monotonic state transition. */
   int pki_mtls_ramp_init(int configured_mode);
   int pki_mtls_note_presentation(const char *serial, long now);
   int pki_mtls_ramp_ready(long now);
   int pki_mtls_ramp_advance(long now);
   int pki_mtls_ramp_get(int *state_out, char *hash_out, size_t hash_len, long *advanced_at_out);

   /* Enumerate issued certs (newest first), invoking cb per row. Returns the row
    * count, or -1 on error. Keeps pki.c free of the JSON layer. */
   int pki_list(void (*cb)(void *ctx, const char *serial, const char *cn, long issued_at,
                           long expires_at, int revoked),
                void *ctx);

   /* Provision a self-signed EC P-256 server cert at cert_path and seal its
    * private key directly into Vault. key_path is accepted only as a one-way
    * migration source: it must match an existing cert, is sealed, verified, and
    * securely erased. Fresh installs never create a key file. An existing public
    * cert remains authoritative; delete it explicitly to rotate the identity. */
   int pki_ensure_self_signed_server_cert(const char *cert_path, const char *key_path);

   /* Load the native server TLS private key from its fixed Vault record into a
    * caller-owned transient buffer. The caller must cleanse it. */
   int pki_server_tls_key_load(char *out, size_t cap);

   /* Resolve the STABLE common name for the self-signed server cert, preferring an
    * operator-declared identity (AIMEE_TLS_CN, else the first non-IP token of
    * AIMEE_TLS_EXTRA_SAN) over the OS hostname — which in a container is the
    * volatile per-container ID and rotated the cert on every recreate, breaking
    * TOFU-pinned clients. Falls back to gethostname(), then "aimee-server". Writes
    * a NUL-terminated CN into |out|. Truncation-safe. Exposed for tests. */
   void pki_resolve_server_cn(char *out, size_t cap);

   /* Build the self-signed server cert SAN string into |out| (cap bytes): the
    * type-classified |cn| (IP: for an IP literal, else DNS:) + localhost + IPv4/IPv6
    * loopback, plus |extra| (the
    * AIMEE_TLS_EXTRA_SAN value: comma/space-separated additional names, each
    * pre-typed "IP:"/"DNS:"/… or a bare host/IP auto-classified). Lets a
    * NAT/DNAT deployment present a cert that verifies for its reachable address
    * without an operator-supplied cert. Truncation-safe. Exposed for tests. */
   void pki_build_server_san(const char *cn, const char *extra, char *out, size_t cap);

   /* For tests: drop cached CA + snapshot so a fresh AIMEE_HOME is picked up. */
   void pki_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PKI_H */
