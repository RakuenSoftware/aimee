/* server_write_tier.c — per-request /v1 write-tier resolution. See
 * server_write_tier.h. Pure policy: no sockets, no database, no clock. */

#include "server_write_tier.h"

#include "server.h" /* SERVER_REMOTE_WRITES_* */

#include <openssl/crypto.h>
#include <string.h>

const char *server_write_tier_outcome_str(server_write_tier_outcome_t outcome)
{
   switch (outcome)
   {
   case SERVER_WRITE_TIER_ABSENT:
      return "absent";
   case SERVER_WRITE_TIER_OK:
      return "ok";
   case SERVER_WRITE_TIER_INVALID:
      return "invalid";
   case SERVER_WRITE_TIER_UNKNOWN_KID:
      return "unknown_kid";
   case SERVER_WRITE_TIER_WRONG_TEAM:
      return "wrong_team";
   case SERVER_WRITE_TIER_REPLAY:
      return "replay";
   case SERVER_WRITE_TIER_REPLAY_UNAVAILABLE:
      return "replay_unavailable";
   }
   /* An out-of-range outcome is a bug, and a bug must not read as "ok". */
   return "invalid";
}

static int team_is_enrolled(const server_write_tier_config_t *config, int64_t team_id)
{
   if (!config->enrolled_teams || config->enrolled_team_count == 0)
      return 0; /* a server enrolled for nothing authorizes nothing */
   for (size_t i = 0; i < config->enrolled_team_count; ++i)
      if (config->enrolled_teams[i] == team_id)
         return 1;
   return 0;
}

static int tier_from_claims(kb_identity_tier_t tier)
{
   switch (tier)
   {
   case KB_IDENTITY_TIER_OFF:
      return SERVER_REMOTE_WRITES_OFF;
   case KB_IDENTITY_TIER_DATA:
      return SERVER_REMOTE_WRITES_DATA;
   case KB_IDENTITY_TIER_FULL:
      return SERVER_REMOTE_WRITES_FULL;
   }
   return SERVER_REMOTE_WRITES_OFF;
}

int server_write_tier_resolve(const char *token, size_t token_len,
                              const server_write_tier_config_t *config, int64_t now,
                              server_write_tier_outcome_t *outcome,
                              server_identity_token_claims_t *claims_out)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (claims_out)
      memset(claims_out, 0, sizeof(*claims_out));
   if (!outcome)
      outcome = &local;
   *outcome = SERVER_WRITE_TIER_INVALID;

   if (!config || !config->jwks_json || !config->expected_issuer || !config->expected_audience ||
       !config->replay || now < 0)
      return SERVER_REMOTE_WRITES_OFF;
   if (!token || token_len == 0 || token[0] == '\0')
   {
      /* No credential presented. Denied, but not an error: this is every
       * read-only caller, and conflating it with a forged token would drown the
       * signal an operator actually needs. */
      *outcome = SERVER_WRITE_TIER_ABSENT;
      return SERVER_REMOTE_WRITES_OFF;
   }

   server_identity_token_claims_t claims;
   server_identity_token_result_t verified =
       server_identity_token_verify(token, token_len, config->jwks_json, config->expected_issuer,
                                    config->expected_audience, now, &claims);
   if (verified == SERVER_IDENTITY_TOKEN_UNKNOWN_KID)
   {
      *outcome = SERVER_WRITE_TIER_UNKNOWN_KID;
      return SERVER_REMOTE_WRITES_OFF;
   }
   if (verified != SERVER_IDENTITY_TOKEN_OK)
   {
      *outcome = SERVER_WRITE_TIER_INVALID;
      return SERVER_REMOTE_WRITES_OFF;
   }

   /* §4: the grant lookup and the enforced team claim must agree. A subject
    * granted at team X must not be replayable against a server serving team Y,
    * so the team is checked against THIS server's enrollment before the token is
    * consumed — a token for a foreign team must not burn a replay slot here. */
   if (!team_is_enrolled(config, claims.team_id))
   {
      *outcome = SERVER_WRITE_TIER_WRONG_TEAM;
      OPENSSL_cleanse(&claims, sizeof(claims));
      return SERVER_REMOTE_WRITES_OFF;
   }

   int consumed = config->replay(config->replay_ctx, &claims, now);
   if (consumed > 0)
   {
      *outcome = SERVER_WRITE_TIER_REPLAY;
      OPENSSL_cleanse(&claims, sizeof(claims));
      return SERVER_REMOTE_WRITES_OFF;
   }
   if (consumed < 0)
   {
      /* The store could not answer. Denying is the only safe reading: treating
       * an unavailable replay check as "not replayed" would make an outage into
       * an unlimited-replay window. */
      *outcome = SERVER_WRITE_TIER_REPLAY_UNAVAILABLE;
      OPENSSL_cleanse(&claims, sizeof(claims));
      return SERVER_REMOTE_WRITES_OFF;
   }

   int tier = tier_from_claims(claims.tier);
   *outcome = SERVER_WRITE_TIER_OK;
   if (claims_out)
      *claims_out = claims;
   OPENSSL_cleanse(&claims, sizeof(claims));
   return tier;
}
