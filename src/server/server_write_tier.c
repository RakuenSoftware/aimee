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
   case SERVER_WRITE_TIER_NO_TEAM_CONFIGURED:
      return "no_team_configured";
   }
   /* An out-of-range outcome is a bug, and a bug must not read as "ok". */
   return "invalid";
}

static int team_is_enrolled(const server_write_tier_config_t *config, int64_t team_id)
{
   for (size_t i = 0; i < config->enrolled_team_count; ++i)
      if (config->enrolled_teams[i] == team_id)
         return 1;
   return 0;
}

/* A server with no configured team authorizes no KB-issued identity token.
 * That is a deployment error, not a token error, and the two must not report identically. */
static int server_has_no_team(const server_write_tier_config_t *config)
{
   return !config->enrolled_teams || config->enrolled_team_count == 0;
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

int server_write_tier_verify(const char *token, size_t token_len,
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

   /* config->replay is deliberately NOT required here: this half has no side
    * effects, so a caller that only wants the tier need not own a store. */
   if (!config || !config->jwks_json || !config->expected_issuer || !config->expected_audience ||
       now < 0)
      return SERVER_REMOTE_WRITES_OFF;
   if (!token || token_len == 0 || token[0] == '\0')
   {
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

   /* §4: the grant lookup and the enforced team claim must agree. Checked before
    * any consumption so a token minted for another team can never burn a replay
    * slot on this server. */
   if (server_has_no_team(config) || !team_is_enrolled(config, claims.team_id))
   {
      *outcome = server_has_no_team(config) ? SERVER_WRITE_TIER_NO_TEAM_CONFIGURED
                                            : SERVER_WRITE_TIER_WRONG_TEAM;
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

int server_write_tier_consume(const server_identity_token_claims_t *claims,
                              const server_write_tier_config_t *config, int64_t now,
                              server_write_tier_outcome_t *outcome)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (!outcome)
      outcome = &local;
   *outcome = SERVER_WRITE_TIER_INVALID;
   if (!claims || !config || !config->replay || now < 0)
      return SERVER_REMOTE_WRITES_OFF;

   int consumed = config->replay(config->replay_ctx, claims, now);
   if (consumed > 0)
   {
      *outcome = SERVER_WRITE_TIER_REPLAY;
      return SERVER_REMOTE_WRITES_OFF;
   }
   if (consumed < 0)
   {
      /* The store could not answer. Denying is the only safe reading: treating
       * an unavailable replay check as "not replayed" would make an outage into
       * an unlimited-replay window. */
      *outcome = SERVER_WRITE_TIER_REPLAY_UNAVAILABLE;
      return SERVER_REMOTE_WRITES_OFF;
   }
   *outcome = SERVER_WRITE_TIER_OK;
   return tier_from_claims(claims->tier);
}

int server_write_tier_resolve(const char *token, size_t token_len,
                              const server_write_tier_config_t *config, int64_t now,
                              server_write_tier_outcome_t *outcome,
                              server_identity_token_claims_t *claims_out)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (!outcome)
      outcome = &local;
   /* resolve keeps requiring a replay hook: a caller asking for the whole
    * decision in one step is asking for the token to be spent. */
   if (!config || !config->replay)
   {
      if (claims_out)
         memset(claims_out, 0, sizeof(*claims_out));
      *outcome = SERVER_WRITE_TIER_INVALID;
      return SERVER_REMOTE_WRITES_OFF;
   }

   server_identity_token_claims_t claims;
   int tier = server_write_tier_verify(token, token_len, config, now, outcome, &claims);
   if (*outcome != SERVER_WRITE_TIER_OK)
   {
      if (claims_out)
         memset(claims_out, 0, sizeof(*claims_out));
      OPENSSL_cleanse(&claims, sizeof(claims));
      return tier;
   }
   int consumed_tier = server_write_tier_consume(&claims, config, now, outcome);
   if (*outcome == SERVER_WRITE_TIER_OK && claims_out)
      *claims_out = claims;
   else if (claims_out)
      memset(claims_out, 0, sizeof(*claims_out));
   OPENSSL_cleanse(&claims, sizeof(claims));
   return consumed_tier;
}
