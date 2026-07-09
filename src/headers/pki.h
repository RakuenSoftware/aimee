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

   /* Issue a client cert for `cn` (which must pass vault_principal_cert_sanitize),
    * valid for `validity_days`, signed by the CA. Writes the cert PEM and key PEM
    * into the caller's buffers and the hex serial into serial_out, and records the
    * cert in DB1. Returns 0 on success, -1 on failure. */
   int pki_issue(const char *cn, int validity_days, char *cert_pem, size_t cert_len, char *key_pem,
                 size_t key_len, char *serial_out, size_t serial_len);

   /* Revoke by hex serial (adds to the DB1 denylist + the in-memory snapshot).
    * Returns 0 on success (including already-revoked), -1 on error. */
   int pki_revoke(const char *serial);

   /* 1 if `serial` (hex) is revoked, else 0. Reads the in-memory snapshot only —
    * safe to call from the TLS verify callback. */
   int pki_is_revoked(const char *serial);

   /* Enumerate issued certs (newest first), invoking cb per row. Returns the row
    * count, or -1 on error. Keeps pki.c free of the JSON layer. */
   int pki_list(void (*cb)(void *ctx, const char *serial, const char *cn, long issued_at,
                           long expires_at, int revoked),
                void *ctx);

   /* Provision a self-signed EC P-256 server cert at (cert_path, key_path) when
    * neither exists yet — makes native TLS zero-config when a tls_port is set but
    * no operator cert is present. Never overwrites an existing cert/key. The key
    * is written 0600. Returns 0 if a usable cert is in place, -1 on failure. */
   int pki_ensure_self_signed_server_cert(const char *cert_path, const char *key_path);

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
