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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_WRITE_TIER_DB1_H */
