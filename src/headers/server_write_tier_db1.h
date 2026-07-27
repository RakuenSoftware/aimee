#ifndef AIMEE_SERVER_WRITE_TIER_DB1_H
#define AIMEE_SERVER_WRITE_TIER_DB1_H

/* The db1-backed replay hook for server_write_tier_resolve. Kept out of
 * server_write_tier.c so the policy stays storage-free and unit testable. */

#include <stdint.h>

#include "server_write_tier.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      SERVER_WRITE_TIER_CONFIG_READY = 0,
      SERVER_WRITE_TIER_CONFIG_NO_TEAM,
      SERVER_WRITE_TIER_CONFIG_NO_SERVER_ID,
      SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE
   } server_write_tier_config_state_t;

   /* Matches server_write_tier_replay_fn: 0 = fresh and now recorded,
    * 1 = already seen, negative = the store did not record it (deny). */
   int server_write_tier_replay_db1(void *ctx, const server_identity_token_claims_t *claims,
                                    int64_t now);

   /* Verify the caller's identity token against this server's environment and
    * JWKS cache, WITHOUT spending its single-use jti:
    *
    *   AIMEE_SERVER_ID                       -> expected audience
    *   AIMEE_SERVER_TEAM_ID                  -> the one team this server serves
    *   AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE   -> the JWKS to verify against
    *
    * The issuer is the fixed "kb" identity per proposal §4.
    *
    * Safe to call before the request is known to be servable, which is what lets
    * connection capabilities be derived from the caller's real tier. Returns
    * SERVER_REMOTE_WRITES_OFF and a specific outcome for every failure,
    * including this server's own misconfiguration: an unset AIMEE_SERVER_TEAM_ID
    * reports no_team_configured rather than blaming the token. */
   int server_write_tier_verify_for_request(const char *token, size_t token_len, int64_t now,
                                            server_write_tier_outcome_t *outcome,
                                            server_identity_token_claims_t *claims_out);

   /* Spend the jti for claims already returned by the verify call above. Call at
    * most once per request and ONLY once the request is going to be served, so a
    * request rejected for an unrelated reason does not burn the user's token. */
   int server_write_tier_consume_for_request(const server_identity_token_claims_t *claims,
                                             int64_t now, server_write_tier_outcome_t *outcome);

   /* 1 when this server has a usable AIMEE_SERVER_TEAM_ID. Startup uses this to
    * log the misconfiguration once and loudly, instead of leaving an operator to
    * infer it from a stream of denied writes. */
   int server_write_tier_team_configured(void);

   /* Syntactic startup preflight for the three deployment inputs. This does not
    * claim that the mounted bundle or cached JWKS is valid; those are checked by
    * server_mgmt_jwks_cache_startup after DB1 opens. It exists so partial
    * Compose configuration names the missing variable instead of collapsing to
    * the generic token outcome `invalid`. */
   server_write_tier_config_state_t server_write_tier_config_state(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_WRITE_TIER_DB1_H */
