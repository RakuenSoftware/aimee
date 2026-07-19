/* kb_auth_oidc.h: the BYO (bring-your-own) OIDC / JWT Verifier for aimee-kb.
 *
 * Phase 3 of the distributed-mode-auth proposal. A bearer that is a compact JWS
 * (a JWT) is verified against an operator-configured JWKS: the RS256 signature
 * is checked with the matching public key, the iss / aud / exp claims are
 * validated, and claims are mapped to a scope. This plugs into the Verifier
 * seam (kb_verifier.h) via kb_oidc_verifier_register().
 *
 * Security model — the load-bearing properties:
 *   - Asymmetric only. Verification uses the JWKS PUBLIC key; aimee-kb never
 *     holds a key that could mint another tenant's tokens.
 *   - alg pinning. Only "RS256" is accepted; the "none" alg and HMAC algs are
 *     rejected, closing the classic JWT alg-confusion downgrade.
 *   - Verify-then-trust. subject / scope are taken from the verified payload
 *     only, after the signature and iss/aud/exp checks pass.
 * This verifier is ADDITIVE: it is registered after the built-in kb-token
 * (owner) verifier, so a bad JWKS / issuer config can never lock the owner out. */
#ifndef DEC_KB_AUTH_OIDC_H
#define DEC_KB_AUTH_OIDC_H

#include "kb_verifier.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Operator configuration for the OIDC verifier. Empty issuer/audience mean
    * "do not check that claim" (an explicit operator choice). */
   typedef struct
   {
      char issuer[256];        /* expected "iss"; "" = unchecked */
      char audience[256];      /* expected "aud" (string, or one element of the
                                * "aud" array); "" = unchecked */
      char scope_claim[64];    /* payload claim mapped to scope_id (e.g. "project").
                                * "" = the JWT yields an unscoped (owner-equivalent)
                                * identity. */
      char scope_kind[32];     /* scope kind paired with scope_claim (e.g. "project") */
      char jwks_json[8192];    /* JWKS document: {"keys":[{"kty":"RSA","kid":..,
                                * "n":..,"e":..}, ...]} */
      long max_token_age_secs; /* hard server-side ceiling on accepted token age
                                * (now - iat); 0 = default (900s / 15 min). P1 I9. */
   } kb_oidc_config_t;

   /* Verify a compact JWS `jwt` (header.payload.signature) against cfg.
    * Validates: alg == RS256, signature against the JWKS key selected by the
    * header "kid" (or the sole RSA key when no kid), exp > now, and — when
    * configured — iss and aud. On success returns 1 and fills *out (subject from
    * "sub", scope from cfg->scope_claim, expiry from "exp"). Returns 0 on any
    * failure. `now` is the current unix time (injectable for testing). */
   int kb_oidc_verify_jwt(const char *jwt, const kb_oidc_config_t *cfg, long now,
                          kb_verify_result_t *out);

   /* Register the OIDC verifier into the Verifier seam, retaining a copy of cfg.
    * Tried after the built-in kb-token verifier. Returns 0 on success, -1 on
    * invalid args / registry full. A second call replaces the retained config. */
   int kb_oidc_verifier_register(const kb_oidc_config_t *cfg);

   /* Build the OIDC config from a JWKS file path plus claim policy and register
    * it (see kb_oidc_verifier_register). issuer/audience/scope_claim/scope_kind
    * may be NULL (unchecked / unscoped). Returns 0 on success, -1 on a config
    * error (missing path, unreadable / empty / oversized JWKS file). */
   int kb_oidc_register_from_file(const char *jwks_file, const char *issuer, const char *audience,
                                  const char *scope_claim, const char *scope_kind);

   /* Configure + register the OIDC verifier from the environment:
    *   AIMEE_KB_OIDC_JWKS_FILE   path to a JWKS JSON document (enables OIDC)
    *   AIMEE_KB_OIDC_ISSUER      expected "iss" (optional)
    *   AIMEE_KB_OIDC_AUDIENCE    expected "aud" (optional)
    *   AIMEE_KB_OIDC_SCOPE_CLAIM payload claim mapped to scope_id (optional)
    *   AIMEE_KB_OIDC_SCOPE_KIND  scope kind paired with the claim (optional)
    * A no-op returning 0 when AIMEE_KB_OIDC_JWKS_FILE is unset (OIDC stays off).
    * Returns -1 on a configuration error. */
   int kb_oidc_register_from_env(void);

   /* Optional fleet-JWKS resolver hook (P1 I10). When set, the verifier resolves
    * the trusted JWKS for the configured issuer from this callback (the shared
    * Postgres source) instead of the per-instance file; the file is used only when
    * the resolver returns non-zero (no fleet keys). Keeps this core free of a DB
    * dependency — kb registers the db2-backed resolver at startup; tests leave it
    * unset. Returns 0 + fills out[cap] on success, non-zero to fall back. */
   typedef int (*kb_oidc_fleet_resolver_fn)(const char *issuer, char *out, size_t cap);
   void kb_oidc_set_fleet_resolver(kb_oidc_fleet_resolver_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_AUTH_OIDC_H */
