/* kb_pki.h: aimee-kb's internal certificate authority for distributed-mode
 * mTLS. (distributed-mode-auth proposal, Phase: PKI.)
 *
 * aimee-kb is its own CA: on first start it generates a self-signed CA, and
 * every enrolled client is issued a short-lived client certificate signed by
 * that CA. A peer is trusted iff its certificate chains to the CA. The CA
 * fingerprint (sha256 of the CA cert DER) is the value pinned in the `aimee://`
 * connection string (`ca=sha256:<fp>`, see kb_enroll.h) — a paste-once TOFU
 * anchor so a client can detect a swapped CA.
 *
 * This module is the crypto core: CA generation, client-cert issuance, the CA
 * fingerprint, and chain verification. All values are PEM strings so the caller
 * owns persistence (proposal invariant 3 — CA + issued certs live on the DB2 /
 * data volume; wiring that persistence is a follow-up).
 *
 * Security model: keys are RSA-2048; the CA private key never leaves the
 * struct, and issued client keys are returned once to the enrolling client.
 * Verification is full X509 chain validation (signature + validity + CA basic
 * constraints), not a bare signature check. */
#ifndef DEC_KB_PKI_H
#define DEC_KB_PKI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define KB_PKI_CERT_PEM_MAX 4096 /* PEM cert (RSA-2048) ~1.2 KB + slack */
#define KB_PKI_KEY_PEM_MAX  4096 /* PEM PKCS#8 key (RSA-2048) ~1.7 KB + slack */
#define KB_PKI_FP_HEX       65   /* sha256 lowercase hex (64) + NUL */

   /* An internal CA: its self-signed certificate and private key, both PEM. */
   typedef struct
   {
      char cert_pem[KB_PKI_CERT_PEM_MAX];
      char key_pem[KB_PKI_KEY_PEM_MAX]; /* sensitive — persist with care */
   } kb_pki_ca_t;

   /* Generate a fresh self-signed CA (RSA-2048, CN "aimee-kb-ca", ~10 year
    * validity, basicConstraints CA:TRUE) into *out. Returns 0 on success, -1 on
    * error (RNG / OpenSSL failure or PEM overflow). */
   int kb_pki_ca_generate(kb_pki_ca_t *out);

   /* Compute the CA fingerprint: sha256 of the certificate's DER encoding, as
    * lowercase hex, into hex_out[cap] (cap >= KB_PKI_FP_HEX). This is the value
    * that appears after "sha256:" in the connection string. Accepts any cert
    * PEM (the CA's). Returns 0 on success, -1 on error. */
   int kb_pki_ca_fingerprint(const char *ca_cert_pem, char *hex_out, size_t cap);

   /* Extract the immutable identity fields exactly as the TLS peer helpers do:
    * issuer in OpenSSL's one-line DN form and serial as uppercase hex. Any
    * output may be NULL when the caller does not need it. */
   int kb_pki_cert_metadata(const char *cert_pem, char *issuer_out, size_t issuer_cap,
                            char *serial_out, size_t serial_cap);

   /* Issue a client certificate (RSA-2048) for `subject_cn`, signed by `ca`,
    * valid for `valid_secs` seconds from now (keyUsage digitalSignature,
    * extKeyUsage clientAuth). Writes the client cert PEM into cert_pem_out[cert_cap]
    * and its private key PEM into key_pem_out[key_cap]. Returns 0 on success,
    * -1 on error. */
   int kb_pki_issue_client_cert(const kb_pki_ca_t *ca, const char *subject_cn, long valid_secs,
                                char *cert_pem_out, size_t cert_cap, char *key_pem_out,
                                size_t key_cap);

   /* Issue a SERVER certificate (RSA-2048) for `host`, signed by `ca`. `host` is
    * the CN and a subjectAltName (DNS:<host>, or IP:<host> when it parses as an
    * IP) so TLS hostname verification passes. keyUsage
    * digitalSignature,keyEncipherment; extKeyUsage serverAuth. The kb presents
    * this on the mTLS listener; a client that pinned the CA fingerprint trusts
    * it. Writes the cert + key PEM. Returns 0 on success, -1 on error. */
   int kb_pki_issue_server_cert(const kb_pki_ca_t *ca, const char *host, long valid_secs,
                                char *cert_pem_out, size_t cert_cap, char *key_pem_out,
                                size_t key_cap);

   /* Verify that `client_cert_pem` chains to `ca_cert_pem` (signature, validity
    * window, and CA constraints). Returns 1 if the certificate is trusted, 0 if
    * not (untrusted issuer, expired, malformed). */
   int kb_pki_verify_client_cert(const char *ca_cert_pem, const char *client_cert_pem);

   /* Sign a PEM-encoded PKCS#10 CSR with `ca`, issuing a client certificate that
    * carries the CSR's PUBLIC KEY but a SERVER-CONTROLLED subject (`subject_cn`,
    * e.g. the enrollment token's scope). The CSR's own subject is IGNORED for
    * authz — never trust a client-asserted identity (verify-then-trust). The
    * CSR's self-signature is verified first, proving the requester holds the
    * matching private key; that key never leaves the client. The issued cert is
    * CA:FALSE, keyUsage digitalSignature, extKeyUsage clientAuth, valid for
    * `valid_secs`. Writes the cert PEM into cert_pem_out[cert_cap]. Returns 0 on
    * success, -1 on a malformed / unverifiable CSR or any issuance error. */
   int kb_pki_sign_csr(const kb_pki_ca_t *ca, const char *csr_pem, const char *subject_cn,
                       long valid_secs, char *cert_pem_out, size_t cert_cap);

   /* Validate a PEM PKCS#10 CSR WITHOUT issuing anything: it must parse, its
    * self-signature must verify (proof of private-key possession), and its key
    * must be acceptable (RSA >= 2048). Returns 0 if the CSR is acceptable, -1
    * otherwise. Lets callers reject a bad CSR before spending a single-use token
    * (kb_pki_sign_csr applies the same checks at issue time). */
   int kb_pki_csr_validate(const char *csr_pem);

   /* --- CA persistence (proposal invariant 3: trust material must survive a
    *     restart, else the CA re-rolls and every enrolled client breaks). The CA
    *     lives in <dir>/ca.pem (certificate) + <dir>/ca-key.pem (private key). */

   /* Persist `ca` to <dir>/ca.pem and <dir>/ca-key.pem, creating <dir> (mode
    * 0700) if absent. The private-key file is written mode 0600 (owner-only).
    * Returns 0 on success, -1 on error (bad args, mkdir/write failure). */
   int kb_pki_ca_save(const char *dir, const kb_pki_ca_t *ca);

   /* Load a CA from <dir>/ca.pem + <dir>/ca-key.pem into *out. Returns 0 on
    * success, -1 if either file is missing, unreadable, empty, or too large. */
   int kb_pki_ca_load(const char *dir, kb_pki_ca_t *out);

   /* Load the CA from <dir> if present; otherwise generate a fresh CA and
    * persist it there. On success *out holds the CA and, when `created` is
    * non-NULL, *created is 1 if newly generated or 0 if loaded from disk.
    * Returns 0 on success, -1 on error. Idempotent: a second call loads the
    * same CA (same fingerprint) rather than re-rolling it. */
   int kb_pki_ca_load_or_create(const char *dir, kb_pki_ca_t *out, int *created);

   /* Vault-custodied persistence: the private key is encrypted under the
    * configured vault server KEK and stored as ca-key.vault; no PEM key file is
    * emitted. These APIs are used by production enrollment/TLS paths. */
   int kb_pki_ca_save_custodied(const char *dir, const kb_pki_ca_t *ca);
   int kb_pki_ca_load_custodied(const char *dir, kb_pki_ca_t *out);
   int kb_pki_ca_load_or_create_custodied(const char *dir, kb_pki_ca_t *out, int *created);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_PKI_H */
