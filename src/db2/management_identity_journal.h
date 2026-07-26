/* management_identity_journal.h — the typed C seam a login mode calls to file a
 * data-plane identity intent (proposal per-user-remote-writes-authz.md §3/§4).
 *
 * This is the boundary between "kb authenticated somebody" and "the token
 * authority may mint for them". A login front-end (OIDC relying party, or the
 * PAM mediator) calls db2_identity_intent_start once it holds a VERIFIED
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
#ifndef AIMEE_DB2_MANAGEMENT_IDENTITY_JOURNAL_H
#define AIMEE_DB2_MANAGEMENT_IDENTITY_JOURNAL_H

#include "kb_identity.h"
#include "management_action_journal.h" /* db2_management_action_result_t */

#include <stddef.h>
#include <stdint.h>

#define DB2_IDENTITY_ID_HEX           64U
#define DB2_IDENTITY_TOKEN_JTI_MAX    128U
#define DB2_IDENTITY_SERVER_MAX       127U
#define DB2_IDENTITY_TOKEN_ISSUER_MAX 255U
#define DB2_IDENTITY_SUBJECT_MAX      576U
#define DB2_IDENTITY_KID_MAX          64U
#define DB2_IDENTITY_INSTALL_ID_HEX   32U

/* The server refuses a token whose lifetime exceeds this, and the intent CHECK
 * refuses to record one, so a caller that asks for more is rejected here rather
 * than minting something the verifier would throw away. */
#define DB2_IDENTITY_TTL_MAX_SECONDS 3600

/* How the subject was authenticated. Recorded for audit, never for
 * authorization — the grant decides that. An extensible list rather than a
 * boolean so a future backend (Kerberos/SPNEGO, direct LDAP) is a new value
 * here and in the schema CHECK, not a change to the enforcement side. */
typedef enum
{
   DB2_IDENTITY_AUTH_MODE_OIDC = 1,
   DB2_IDENTITY_AUTH_MODE_PAM
} db2_identity_auth_mode_t;

/* Caller-owned and retained across an ambiguous start: on
 * DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS the identifiers must be reused
 * verbatim, never regenerated, or the retry files a second intent. Every char
 * array is a canonical NUL-terminated fixed record with an all-zero tail. */
typedef struct
{
   char correlation_id[DB2_IDENTITY_ID_HEX + 1];
   char jti[DB2_IDENTITY_ID_HEX + 1];              /* namespace handle, not the token claim */
   char token_jti[DB2_IDENTITY_TOKEN_JTI_MAX + 1]; /* the token's own jti claim */
   int64_t team_id;
   char target_server_id[DB2_IDENTITY_SERVER_MAX + 1];
   db2_identity_auth_mode_t auth_mode;
   char token_issuer[DB2_IDENTITY_TOKEN_ISSUER_MAX + 1];
   char kid[DB2_IDENTITY_KID_MAX + 1];
   int ttl_seconds;
   char installation_id[DB2_IDENTITY_INSTALL_ID_HEX + 1];
} db2_identity_intent_operation_t;

typedef struct
{
   int replayed;
   char correlation_id[DB2_IDENTITY_ID_HEX + 1];
   char jti[DB2_IDENTITY_ID_HEX + 1];
   char token_jti[DB2_IDENTITY_TOKEN_JTI_MAX + 1];
   int64_t team_id;
   /* Resolved by the database from the tenant scope, never supplied. */
   char subject[DB2_IDENTITY_SUBJECT_MAX + 1];
   db2_identity_auth_mode_t auth_mode;
   char target_server_id[DB2_IDENTITY_SERVER_MAX + 1];
   char token_issuer[DB2_IDENTITY_TOKEN_ISSUER_MAX + 1];
   char audience[DB2_IDENTITY_SERVER_MAX + 1];
   char kid[DB2_IDENTITY_KID_MAX + 1];
   int64_t issued_at, expires_at;
   char installation_id[DB2_IDENTITY_INSTALL_ID_HEX + 1];
   int64_t installation_generation, installation_enrollment_id;
   int64_t target_enrollment_id, revocation_generation, created_at_epoch;
} db2_identity_intent_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Generate the three identifiers (correlation, namespace jti, token jti) and
    * canonicalize the rest. `out` is cleared on entry and on failure. Call this
    * exactly once per login: on an ambiguous start, retry with the same `out`.
    * Returns DB2_MANAGEMENT_ACTION_UNAVAILABLE if the platform CSPRNG failed —
    * never a weaker identifier. */
   db2_management_action_result_t
   db2_identity_intent_operation_init(int64_t team_id, const char *target_server_id,
                                      db2_identity_auth_mode_t auth_mode, const char *token_issuer,
                                      const char *kid, int ttl_seconds, const char *installation_id,
                                      db2_identity_intent_operation_t *out);

   /* File the intent for `principal` (which must be authenticated — the tenant
    * scope refuses otherwise). DENIED covers both "no live grant" and "not a
    * member of the named team": the writer reads the grant as the principal
    * under FORCE RLS, so a grant planted for a non-member is invisible. */
   db2_management_action_result_t
   db2_identity_intent_start(const kb_principal_t *principal,
                             const db2_identity_intent_operation_t *operation,
                             db2_identity_intent_t *out);

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
   db2_management_action_result_t db2_identity_login_context(const kb_principal_t *principal,
                                                             int64_t team_id,
                                                             char installation_id[33],
                                                             char kid[DB2_IDENTITY_KID_MAX + 1]);

   /* The wire string for an auth mode ("oidc"/"pam"), or NULL if out of range. */
   const char *db2_identity_auth_mode_str(db2_identity_auth_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_MANAGEMENT_IDENTITY_JOURNAL_H */
