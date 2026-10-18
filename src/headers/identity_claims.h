#ifndef AIMEE_IDENTITY_CLAIMS_H
#define AIMEE_IDENTITY_CLAIMS_H
#include <stdint.h>

/* Owned claim storage shared by the identity-token builder and verifier.
 * Replay-store input remains a borrowed view with a textual tier. */
typedef enum
{
   KB_IDENTITY_TIER_OFF = 0,
   KB_IDENTITY_TIER_DATA = 1,
   KB_IDENTITY_TIER_FULL = 2
} aimee_identity_tier_t;

typedef aimee_identity_tier_t kb_identity_tier_t;

typedef struct
{
   char issuer[256];   /* iss — always the kb identity ("kb")               */
   char audience[128]; /* aud — the target server_id                        */
   char subject[577];  /* sub — OIDC (iss,sub) composite or PAM username    */
   int64_t team_id;    /* team — the tenant team the grant is scoped to     */
   aimee_identity_tier_t tier;
   char jti[129];      /* unique token id (server-local replay rejection)   */
   char kid[65];       /* signing-key id (server selects the JWKS key)      */
   int64_t issued_at;  /* iat (unix seconds)                                */
   int64_t expires_at; /* exp (unix seconds); must be > issued_at           */
} aimee_identity_claims_t;
#endif
