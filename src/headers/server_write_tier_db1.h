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

   /* Matches server_write_tier_replay_fn: 0 = fresh and now recorded,
    * 1 = already seen, negative = the store did not record it (deny). */
   int server_write_tier_replay_db1(void *ctx, const server_identity_token_claims_t *claims,
                                    int64_t now);

   /* Resolve the write tier for one live request, assembling the resolver's
    * config from this server's environment and JWKS cache:
    *
    *   AIMEE_SERVER_ID                       -> expected audience
    *   AIMEE_SERVER_TEAM_ID                  -> the one team this server serves
    *   AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE   -> the JWKS to verify against
    *
    * The issuer is the fixed "kb" identity per proposal §4.
    *
    * Returns SERVER_REMOTE_WRITES_OFF and a specific outcome for every failure,
    * including its own misconfiguration: an unset AIMEE_SERVER_TEAM_ID reports
    * no_team_configured rather than pretending the token was at fault. */
   int server_write_tier_for_request(const char *token, size_t token_len, int64_t now,
                                     server_write_tier_outcome_t *outcome,
                                     server_identity_token_claims_t *claims_out);

   /* 1 when this server has a usable AIMEE_SERVER_TEAM_ID. Startup uses this to
    * log the misconfiguration once and loudly, instead of leaving an operator to
    * infer it from a stream of denied writes. */
   int server_write_tier_team_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_WRITE_TIER_DB1_H */
