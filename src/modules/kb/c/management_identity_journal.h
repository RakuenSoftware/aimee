/* management_identity_journal.h — the typed C seam a login mode calls to file a
 * data-plane identity intent (proposal per-user-remote-writes-authz.md §3/§4).
 *
 * This is the boundary between "kb authenticated somebody" and "the token
 * authority may mint for them". A login front-end (OIDC relying party, or the
 * PAM mediator) calls kb_store_identity_intent_start once it holds a VERIFIED
 * principal; the mint pipeline in management_token_authority.h then takes over
 * from the (correlation_id, jti) pair this returns.
 *
 * Two properties are structural, not conventional:
 *
 *   * There is no subject parameter. The subject recorded is the authenticated
 *     principal the caller passes as `principal`, resolved by the database from
 *     the tenant scope. A login front-end cannot file an intent for anyone but
 *     whoever it just authenticated.
 *
 *   * Filing an intent authorizes nothing. It records that a granted subject
 *     asked for a token; kb_management_identity_authority_snapshot re-reads the
 *     grant, the registry, the management instance and both enrollments under
 *     its own row locks at mint time. A grant revoked in between makes the mint
 *     refuse, not this call.
 *
 * The result taxonomy is deliberately the management-action one: both journals
 * refuse on the same SQLSTATEs for the same reasons, so a second identical enum
 * would be duplicated knowledge with a second place to drift. */
#ifndef AIMEE_KB_STORE_MANAGEMENT_IDENTITY_JOURNAL_H
#define AIMEE_KB_STORE_MANAGEMENT_IDENTITY_JOURNAL_H

#include "kb_identity.h"
#include "management_action_journal.h" /* kb_store_management_action_result_t */

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_IDENTITY_ID_HEX           64U
#define KB_STORE_IDENTITY_TOKEN_JTI_MAX    128U
#define KB_STORE_IDENTITY_SERVER_MAX       127U
#define KB_STORE_IDENTITY_TOKEN_ISSUER_MAX 255U
#define KB_STORE_IDENTITY_SUBJECT_MAX      576U
#define KB_STORE_IDENTITY_KID_MAX          64U
#define KB_STORE_IDENTITY_INSTALL_ID_HEX   32U

/* The server refuses a token whose lifetime exceeds this, and the intent CHECK
 * refuses to record one, so a caller that asks for more is rejected here rather
 * than minting something the verifier would throw away. */
#define KB_STORE_IDENTITY_TTL_MAX_SECONDS 3600

/* How the subject was authenticated. Recorded for audit, never for
 * authorization — the grant decides that. An extensible list rather than a
 * boolean so a future backend (Kerberos/SPNEGO, direct LDAP) is a new value
 * here and in the schema CHECK, not a change to the enforcement side. */
typedef enum
{
   KB_STORE_IDENTITY_AUTH_MODE_OIDC = 1,
   KB_STORE_IDENTITY_AUTH_MODE_PAM
} kb_store_identity_auth_mode_t;

/* Caller-owned and retained across an ambiguous start: on
 * KB_STORE_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS the identifiers must be reused
 * verbatim, never regenerated, or the retry files a second intent. Every char
 * array is a canonical NUL-terminated fixed record with an all-zero tail. */
typedef struct
{
   char correlation_id[KB_STORE_IDENTITY_ID_HEX + 1];
   char jti[KB_STORE_IDENTITY_ID_HEX + 1];              /* namespace handle, not the token claim */
   char token_jti[KB_STORE_IDENTITY_TOKEN_JTI_MAX + 1]; /* the token's own jti claim */
   int64_t team_id;
   char target_server_id[KB_STORE_IDENTITY_SERVER_MAX + 1];
   kb_store_identity_auth_mode_t auth_mode;
   char token_issuer[KB_STORE_IDENTITY_TOKEN_ISSUER_MAX + 1];
   char kid[KB_STORE_IDENTITY_KID_MAX + 1];
   int ttl_seconds;
   char installation_id[KB_STORE_IDENTITY_INSTALL_ID_HEX + 1];
} kb_store_identity_intent_operation_t;

typedef struct
{
   int replayed;
   char correlation_id[KB_STORE_IDENTITY_ID_HEX + 1];
   char jti[KB_STORE_IDENTITY_ID_HEX + 1];
   char token_jti[KB_STORE_IDENTITY_TOKEN_JTI_MAX + 1];
   int64_t team_id;
   /* Resolved by the database from the tenant scope, never supplied. */
   char subject[KB_STORE_IDENTITY_SUBJECT_MAX + 1];
   kb_store_identity_auth_mode_t auth_mode;
   char target_server_id[KB_STORE_IDENTITY_SERVER_MAX + 1];
   char token_issuer[KB_STORE_IDENTITY_TOKEN_ISSUER_MAX + 1];
   char audience[KB_STORE_IDENTITY_SERVER_MAX + 1];
   char kid[KB_STORE_IDENTITY_KID_MAX + 1];
   int64_t issued_at, expires_at;
   char installation_id[KB_STORE_IDENTITY_INSTALL_ID_HEX + 1];
   int64_t installation_generation, installation_enrollment_id;
   int64_t target_enrollment_id, revocation_generation, created_at_epoch;
} kb_store_identity_intent_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Generate the three identifiers (correlation, namespace jti, token jti) and
    * canonicalize the rest. `out` is cleared on entry and on failure. Call this
    * exactly once per login: on an ambiguous start, retry with the same `out`.
    * Returns KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE if the platform CSPRNG failed —
    * never a weaker identifier. */
   kb_store_management_action_result_t kb_store_identity_intent_operation_init(
       int64_t team_id, const char *target_server_id, kb_store_identity_auth_mode_t auth_mode,
       const char *token_issuer, const char *kid, int ttl_seconds, const char *installation_id,
       kb_store_identity_intent_operation_t *out);

   /* File the intent for `principal` (which must be authenticated — the tenant
    * scope refuses otherwise). DENIED covers both "no live grant" and "not a
    * member of the named team": the writer reads the grant as the principal
    * under FORCE RLS, so a grant planted for a non-member is invisible. */
   kb_store_management_action_result_t
   kb_store_identity_intent_start(const kb_principal_t *principal,
                                  const kb_store_identity_intent_operation_t *operation,
                                  kb_store_identity_intent_t *out);

   /* The two intent inputs a login front-end must READ rather than be told: the
    * signing kid of the live JWKS publication, and the installation_id of the
    * team's active management instance.
    *
    * Deliberately not parameters on the login routes. Both are re-checked by
    * kb_management_identity_authority_snapshot at mint time, so a caller-supplied
    * kid from a superseded publication or another team's installation would be
    * refused there — but only after the intent was already a durable WORM row, and
    * with a refusal far from its cause. Reading them means a login can only file
    * an intent against state that exists.
    *
    * DENIED when the principal is not a member of `team_id`; UNAVAILABLE when the
    * team has no single active instance or the current publication is outside its
    * validity window — both of which are real deployment states, not bugs, and
    * both of which must stop a login rather than produce an intent that cannot
    * mint. */
   kb_store_management_action_result_t
   kb_store_identity_login_context(const kb_principal_t *principal, int64_t team_id,
                                   char installation_id[33],
                                   char kid[KB_STORE_IDENTITY_KID_MAX + 1]);

   /* The wire string for an auth mode ("oidc"/"pam"), or NULL if out of range. */
   const char *kb_store_identity_auth_mode_str(kb_store_identity_auth_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_STORE_MANAGEMENT_IDENTITY_JOURNAL_H */
