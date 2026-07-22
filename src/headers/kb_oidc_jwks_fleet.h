/* kb_oidc_jwks_fleet.h: fleet-wide (Postgres-backed) JWKS source (P1 I10).
 *
 * At scale the set of trusted OIDC signing keys must be fleet-wide, not a
 * per-instance file: all stateless kb instances read the same keys from shared
 * Postgres (kb_oidc_jwks), so an IdP key rotation converges across the fleet
 * within a bounded refresh and no instance accepts a JWT another rejects. The env
 * JWKS file remains a dev-only fallback used only when no PG rows exist.
 *
 * Decoupling: auth_oidc.c holds an optional resolver hook (kb_oidc_set_fleet_
 * resolver) so the verifier core stays free of a DB dependency; kb_main registers
 * the db2-backed resolver below at startup, and unit tests leave it unset (file
 * fallback). */
#ifndef DEC_KB_OIDC_JWKS_FLEET_H
#define DEC_KB_OIDC_JWKS_FLEET_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Resolve the fleet JWKS for `issuer` into out[cap] as a JWKS document
    * ({"keys":[...]}). Returns 0 when at least one active key exists (out filled),
    * -1 when there are none / DB unavailable (caller falls back to the file). Uses
    * a bounded-refresh TTL cache (default 300s) so IdP rotation converges fleet-wide
    * within the TTL, which is <= the OIDC token-age ceiling (I9). */
   int kb_oidc_jwks_fleet_get(const char *issuer, char *out, size_t cap);

   /* Register kb_oidc_jwks_fleet_get as the verifier's fleet resolver (call once at
    * kb startup, after db2_init). */
   void kb_oidc_jwks_fleet_enable(void);

   /* Pure: assemble a JWKS document from `n` JWK object strings into out[cap].
    * Returns 0 on success, -1 on overflow/invalid args. Exposed for unit testing. */
   int kb_oidc_jwks_assemble(const char *const *jwk_objects, int n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_OIDC_JWKS_FLEET_H */
