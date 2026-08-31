#ifndef AIMEE_SERVER_IDENTITY_TOKEN_H
#define AIMEE_SERVER_IDENTITY_TOKEN_H

/* server_identity_token.h — server-side verification of the kb-signed data-plane
 * identity token (proposal per-user-remote-writes-authz.md §4). Counterpart to
 * the kb-side builder in kb_identity_token.{h,c}. Implemented in
 * shared/auth_token_verify.c so it reuses that file's vetted JWS/JWKS primitives
 * (base64url decode, RS256 verify, JWKS key-selection by kid), and is kept a
 * strictly separate token type from the P5 management JWT: it requires the
 * `aimee-id+jwt` header `typ`, so a management token can never verify here and
 * vice versa.
 *
 * The verifier authenticates the token itself — signature (by `kid`), `typ`,
 * `iss`/`aud` equality, the P1 composite-identity `sub` form, a valid `tier`,
 * and the `iat`/`exp` window — and returns the typed claims. The CALLER still
 * enforces the two server-state checks that are not token-intrinsic:
 * `team ∈ this server's enrolled teams` and `jti` replay rejection. Every check
 * is fail-closed. */

#include <stddef.h>
#include <stdint.h>

#include "kb_identity_token.h" /* kb_identity_tier_t */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Maximum accepted lifetime (seconds) for a data-plane identity token; a
    * token whose exp-iat exceeds this is rejected regardless of the clock. */
#define SERVER_IDENTITY_TOKEN_MAX_LIFETIME 3600

   /* 1 if `s` is a well-formed data-plane subject: `owner`, `oidc:<iss>:<sub>`,
    * `cert:<issuer>:<serial>`, or a bare host-account `<username>`.
    *
    * Exposed so the grammar can be cross-checked against its other two copies —
    * the subject CHECK in db2/schema.sql and db2_intent_canonical_actor in
    * db2/management_intent_fields.h. Those three cannot share an implementation
    * (the server links neither DB2_OBJS nor libpq), so the only thing that keeps
    * them from drifting is testing them against one shared corpus, and that needs
    * this predicate reachable. See tests/subject_corpus.h.
    *
    * Deliberately NOT the management token's actor grammar, which is stricter and
    * excludes the bare form. */
   int server_identity_subject_valid(const char *s);

   typedef struct
   {
      char issuer[256];
      char audience[128];
      char subject[577];
      int64_t team_id;
      kb_identity_tier_t tier;
      char jti[129];
      char kid[65];
      int64_t issued_at;
      int64_t expires_at;
   } server_identity_token_claims_t;

   typedef enum
   {
      SERVER_IDENTITY_TOKEN_INVALID = 0,
      SERVER_IDENTITY_TOKEN_OK = 1,
      SERVER_IDENTITY_TOKEN_UNKNOWN_KID = 2,
   } server_identity_token_result_t;

   /* Verify a compact-serialization identity token against the supplied JWKS.
    * `now` is the current unix time (seconds). On OK, `*out` holds the typed
    * claims; on any failure `*out` is zeroed. The caller supplies the JWKS
    * (e.g. from the server's authenticated JWKS cache). */
   server_identity_token_result_t
   server_identity_token_verify(const char *jwt, size_t jwt_len, const char *jwks_json,
                                const char *expected_issuer, const char *expected_audience,
                                int64_t now, server_identity_token_claims_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_IDENTITY_TOKEN_H */
