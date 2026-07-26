#ifndef AIMEE_SERVER_WRITE_TIER_H
#define AIMEE_SERVER_WRITE_TIER_H

/* server_write_tier.h — resolve the per-request /v1 write tier from a kb-signed
 * identity token (proposal per-user-remote-writes-authz.md §4-§5).
 *
 * This replaces the process-global `aimee.api.remote_writes` as the authorizer.
 * It is deliberately a PURE policy function: the caller supplies the JWKS, the
 * clock, this server's enrolled teams, and a replay-check callback, so the
 * decision can be tested exhaustively without a socket, a database, or a clock.
 *
 * Fail-closed is total. Every failure path — absent header, malformed token, bad
 * signature, unknown kid, wrong issuer or audience, a team this server is not
 * enrolled for, a replayed jti, or an internal error — resolves to
 * SERVER_REMOTE_WRITES_OFF. There is no path that returns a permissive tier by
 * default, and the outcome is reported separately from the tier so an operator
 * can tell "denied because no token" from "denied because replay" without the
 * two ever collapsing into one value. */

#include <stddef.h>
#include <stdint.h>

#include "server_identity_token.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      /* No Authorization token was presented at all. Writes are denied, but this
       * is the ordinary state for a read-only caller, not an error. */
      SERVER_WRITE_TIER_ABSENT = 0,
      SERVER_WRITE_TIER_OK = 1,
      /* Presented but unusable: malformed, bad signature, wrong iss/aud, expired. */
      SERVER_WRITE_TIER_INVALID = 2,
      /* Verified, but the kid is not in this server's JWKS — usually a key
       * rotation this server has not fetched yet, which is worth distinguishing
       * from a forged token. */
      SERVER_WRITE_TIER_UNKNOWN_KID = 3,
      /* Verified, but the token's team is not one this server is enrolled for.
       * This is the §4 cross-team replay defence. */
      SERVER_WRITE_TIER_WRONG_TEAM = 4,
      /* This server has NO team configured at all, so it can authorize nobody.
       * Deliberately distinct from WRONG_TEAM: that is a token problem an
       * operator cannot fix, this is a server misconfiguration they can fix in
       * seconds — and reporting the two identically is what makes a misconfigured
       * appliance look like a fleet of bad tokens. */
      SERVER_WRITE_TIER_NO_TEAM_CONFIGURED = 7,
      /* Verified, but this jti has already been consumed. */
      SERVER_WRITE_TIER_REPLAY = 5,
      /* The replay store could not answer. Denied: an unavailable replay check
       * must never be treated as "not replayed". */
      SERVER_WRITE_TIER_REPLAY_UNAVAILABLE = 6
   } server_write_tier_outcome_t;

   /* Consume `jti` exactly once. Must return 0 when the jti was previously
    * unseen and is now durably recorded, 1 when it was already seen, and a
    * negative value when the store could not answer. A negative answer denies. */
   typedef int (*server_write_tier_replay_fn)(void *ctx,
                                              const server_identity_token_claims_t *claims,
                                              int64_t now);

   typedef struct
   {
      const char *jwks_json;
      const char *expected_issuer;   /* "kb" */
      const char *expected_audience; /* this server_id */
      const int64_t *enrolled_teams;
      size_t enrolled_team_count;
      server_write_tier_replay_fn replay;
      void *replay_ctx;
   } server_write_tier_config_t;

   /* Resolve the write tier for one request.
    *
    * `token` is the raw bearer credential (already stripped of any "Bearer "
    * prefix), or NULL/empty when none was presented. On success writes the
    * verified claims to `claims_out` when it is non-NULL.
    *
    * ALWAYS returns a valid SERVER_REMOTE_WRITES_* value, and returns
    * SERVER_REMOTE_WRITES_OFF for every non-OK outcome. `*outcome` says why. */
   int server_write_tier_resolve(const char *token, size_t token_len,
                                 const server_write_tier_config_t *config, int64_t now,
                                 server_write_tier_outcome_t *outcome,
                                 server_identity_token_claims_t *claims_out);

   /* Stable lowercase label for logs and the deny metric. Never NULL. */
   const char *server_write_tier_outcome_str(server_write_tier_outcome_t outcome);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_SERVER_WRITE_TIER_H */
