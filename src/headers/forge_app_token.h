/* forge_app_token.h: GitHub/GitLab App installation-token provider.
 *
 * A long-running aimee hub needs a continuously-valid forge identity. When the
 * App env is configured, this layer mints a GitHub App *installation token*
 * from an App ID + RSA private key, caches it with its expiry, and refreshes it
 * transparently before expiry — feeding the existing AIMEE_FORGE_TOKEN consume
 * path (see forge_cred_server_identity in forge_credentials.c).
 *
 * Additive + env-gated: when AIMEE_FORGE_APP_* is unset, this layer is inert
 * and the raw AIMEE_FORGE_TOKEN behavior is unchanged.
 *
 * First boot:   AIMEE_FORGE_APP_PRIVATE_KEY contains PEM text. Startup seals it
 *               into Vault and removes it from the environment before serving;
 *               filesystem paths are rejected.
 * Runtime env:  AIMEE_FORGE_APP_ID, AIMEE_FORGE_APP_INSTALLATION_ID.
 * Optional env:  AIMEE_FORGE_API_BASE (default https://api.github.com; set to a
 *                GHE base or, in tests, a local mock endpoint).
 *
 * See docs/proposals/pending/aimee-workspace-forge-app-identity-and-remote-validation.md
 */
#ifndef DEC_FORGE_APP_TOKEN_H
#define DEC_FORGE_APP_TOKEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* True when all required AIMEE_FORGE_APP_* vars are present. */
   int forge_app_token_configured(void);

   /* Fetch a currently-valid installation token into tok_out (minting on first
    * use, returning the cache otherwise, refreshing when within the skew
    * window). Returns:
    *    1  -> tok_out holds a valid token
    *    0  -> App env not configured (caller falls back to raw AIMEE_FORGE_TOKEN)
    *   -1  -> App env set but mint/refresh failed (fails closed; error logged) */
   int forge_app_token_get(char *tok_out, size_t tok_cap);

   /* Drop the cached token (e.g. for tests / forced refresh). */
   void forge_app_token_reset_cache(void);

   /* --- Testable internals (pure; no env / global state) --- */

   /* Build a compact RS256 App JWT: header {"alg":"RS256","typ":"JWT"}, claims
    * {"iat":iat,"exp":iat+ttl_secs,"iss":app_id}, signed with the RSA private
    * key `pem`. Returns a malloc'd "<h>.<p>.<sig>" string (caller frees) or
    * NULL on a load/sign error. */
   char *forge_app_build_jwt(const char *app_id, const char *pem, long iat, long ttl_secs);

   /* Parse a GitHub installation-token response
    * ({"token":"ghs_...","expires_at":"2024-01-01T00:00:00Z", ...}) into
    * tok_out + *expires_at (unix seconds). Returns 0 on success, -1 if either
    * field is missing/malformed. */
   int forge_app_parse_token_response(const char *json, char *tok_out, size_t tok_cap,
                                      long *expires_at);

   /* Refresh decision: returns 1 when now >= expires_at - skew_secs. */
   int forge_app_token_needs_refresh(long expires_at, long now, long skew_secs);

#ifdef __cplusplus
}
#endif

#endif /* DEC_FORGE_APP_TOKEN_H */
