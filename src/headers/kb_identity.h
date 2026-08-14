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
#include <stdint.h>
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
      /* A local host account, authenticated by PAM. Its identity key is the BARE
       * username — the fourth form the subject grammar already admits, alongside
       * owner / oidc: / cert: — so there is no prefix to invent. `owner` is
       * reserved within this form and the login route refuses it.
       *
       * A distinct kind rather than reusing KB_PRIN_OWNER because the two are not
       * the same authority: owner is the single unscoped bearer, while a host
       * account is one of many named subjects a grant can be written against. */
      KB_PRIN_HOST,
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

   /* Build a principal for a local host account whose password PAM has just
    * accepted. `username` must match the bare-username form of the subject
    * grammar (db2_intent_bare_username) and must not be the reserved name `owner`;
    * both are refused here as well as at the route, because a principal that
    * cannot be a legal subject must never reach a tenant scope. Sets
    * authenticated = 1. Returns 0 on success, -1 on invalid args. */
   int kb_principal_from_host_account(const char *username, kb_principal_t *out);

   /* Build a machine principal from a verified mTLS peer certificate. `serial` is
    * normalized via kb_cert_serial_normalize(). Sets authenticated = 1. */
   int kb_principal_from_cert(const char *cert_issuer, const char *cert_serial, const char *cn,
                              kb_principal_t *out);

   /* Derive the canonical immutable identity key into out[cap]. Returns 0 on
    * success, -1 if the principal is unauthenticated or args invalid. */
   int kb_identity_key(const kb_principal_t *p, char *out, size_t cap);

   /* Parse one existing canonical identity key back into its existing principal
    * kind. Used only for caller context asserted over the fully authenticated
    * aimee-server service channel; this validates and decodes, it does not
    * authenticate an arbitrary network header. */
   int kb_principal_from_identity_key(const char *identity_key, kb_principal_t *out);

   /* Normalize a certificate serial to a stable revocation key: strip colons and
    * 0x, lowercase hex, drop leading zeros (but keep a single "0"). Returns 0 on
    * success. Used for the immutable (cert_issuer, cert_serial_norm) key (I5). */
   int kb_cert_serial_normalize(const char *serial, char *out, size_t cap);

   /* ---- Composite identity resolution (slice 2, I7) ---------------------------
    * A request carries up to two authenticated principals: the mTLS transport
    * (cert:CN) and the actor (OIDC/owner). Resolution combines them FAIL-CLOSED:
    * the billing team must be valid for EVERY principal present (the intersection
    * of their team sets), a named team must lie in that set, and a composite
    * default is auto-selected only when both principals' defaults agree. */

#define KB_MAX_TEAMS 64

   typedef enum
   {
      KB_RESOLVE_OK = 0,
      KB_RESOLVE_NO_PRINCIPAL,      /* neither transport nor actor present */
      KB_RESOLVE_CONFLICT,          /* empty intersection, or named team not in it */
      KB_RESOLVE_AMBIGUOUS_DEFAULT, /* both present, no named team, defaults differ */
   } kb_resolve_status_t;

   /* Pure combination step (no DB): given each present principal's team set + its
    * default team (0 = none) and a named team (0 = none named), compute the
    * resolved team set (the intersection when both principals are present) and the
    * billing team, fail-closed. `out_teams` must hold KB_MAX_TEAMS. A single
    * principal with an empty team set resolves OK with 0 teams (deny downstream),
    * NOT a conflict; only a non-empty-vs-non-empty EMPTY intersection, or a named
    * team outside the resolved set, is KB_RESOLVE_CONFLICT. */
   kb_resolve_status_t kb_identity_combine(const int64_t *tteams, int n_t, int64_t tdefault,
                                           int has_transport, const int64_t *ateams, int n_a,
                                           int64_t adefault, int has_actor, int64_t named_team,
                                           int64_t *out_teams, int *out_n, int64_t *out_billing);

   /* The authenticated request context kb_identity_resolve returns — never a
    * single collapsed principal. transport/actor are copied in with their
    * has_* flag; teams[] is the resolved (intersection) set and billing_team the
    * resolved attribution team (0 = none resolved). */
   typedef struct
   {
      kb_principal_t transport;
      kb_principal_t actor;
      int has_transport;
      int has_actor;
      int64_t teams[KB_MAX_TEAMS];
      int n_teams;
      int64_t billing_team;
   } kb_request_context_t;

   /* DB-backed resolution (slice 2). For each present principal it runs an
    * identity-bootstrap scope (sets aimee.principal, reads that principal's own
    * memberships under RLS), then applies kb_identity_combine. `named_team` = 0
    * when the request names none. Requires the Postgres backend (hard-fails on the
    * SQLite shim like every tenant op). Returns the combine status; *out is filled
    * on KB_RESOLVE_OK (and carries the principals/teams on the reject stats too for
    * auditing). */
   kb_resolve_status_t kb_identity_resolve(const kb_principal_t *transport,
                                           const kb_principal_t *actor, int64_t named_team,
                                           kb_request_context_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_IDENTITY_H */
