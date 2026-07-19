/* kb_identity.h: P1 tenancy — the authenticated principal handle.
 *
 * kb_principal_t is the VERIFIED identity of a request's actor or transport. It
 * is produced ONLY by verifier / mTLS-parse code (kb_principal_from_verify,
 * kb_principal_from_cert) — never constructed from a caller-supplied string. The
 * `authenticated` flag is the type-level guarantee behind the tenancy trust
 * model: db2_tenant_scope_begin() refuses a principal whose authenticated flag is
 * unset, so no raw request string can ever drive the tenant GUCs (B2/N2).
 *
 * The canonical, immutable identity_key derived here is what binds to teams,
 * memberships, audit, and policy throughout — never a bare sub or email:
 *   OIDC    -> "oidc:<iss>:<sub>"
 *   machine -> "cert:<issuer>:<serial>"   (serial normalized; CN is a label only)
 *   owner   -> "owner"
 *
 * Composite identity resolution (transport + actor, intersection rule) lands in
 * slice 2 (kb_identity_resolve); this header defines the principal and its key. */
#ifndef DEC_KB_IDENTITY_H
#define DEC_KB_IDENTITY_H 1

#include <stddef.h>
#include "kb_verifier.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      KB_PRIN_NONE = 0,
      KB_PRIN_OIDC,  /* issuer-scoped OIDC subject (iss, sub) */
      KB_PRIN_CERT,  /* mTLS machine identity (cert_issuer, cert_serial) */
      KB_PRIN_OWNER, /* the owner/bearer principal (no-IdP single-org case) */
   } kb_principal_kind_t;

   /* An authenticated principal. Opaque by convention: only the constructors
    * below set `authenticated = 1`; a zero-initialized struct is unauthenticated
    * and is rejected by every tenant-scoped entry. `issuer`/`subject` are kept as
    * separate immutable fields (never collapsed to user:<sub>). */
   typedef struct
   {
      kb_principal_kind_t kind;
      char issuer[256];  /* OIDC iss, or cert issuer DN */
      char subject[256]; /* OIDC sub, or normalized cert serial */
      char label[128];   /* human label only (e.g. cert CN); never an identity key */
      int authenticated; /* 1 iff produced by a verifier / mTLS parse */
   } kb_principal_t;

   /* Build a principal from a verified OIDC/kb-token result (verify-then-trust).
    * `issuer` is the JWT iss (may be "" for the owner/kb-token). Sets
    * authenticated = 1. Returns 0 on success, -1 on invalid args. */
   int kb_principal_from_verify(const kb_verify_result_t *v, const char *issuer,
                                kb_principal_t *out);

   /* Build a machine principal from a verified mTLS peer certificate. `serial` is
    * normalized via kb_cert_serial_normalize(). Sets authenticated = 1. */
   int kb_principal_from_cert(const char *cert_issuer, const char *cert_serial, const char *cn,
                              kb_principal_t *out);

   /* Derive the canonical immutable identity key into out[cap]. Returns 0 on
    * success, -1 if the principal is unauthenticated or args invalid. */
   int kb_identity_key(const kb_principal_t *p, char *out, size_t cap);

   /* Normalize a certificate serial to a stable revocation key: strip colons and
    * 0x, lowercase hex, drop leading zeros (but keep a single "0"). Returns 0 on
    * success. Used for the immutable (cert_issuer, cert_serial_norm) key (I5). */
   int kb_cert_serial_normalize(const char *serial, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_IDENTITY_H */
