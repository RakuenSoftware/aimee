/* server_write_tier_db1.c — bridges the pure write-tier policy to the durable
 * replay store.
 *
 * Deliberately a separate translation unit from server_write_tier.c: that file
 * must stay free of storage dependencies so its decision logic can be unit
 * tested without a database. This file is the only place the two meet. */

#include "server_write_tier_db1.h"

#include "server.h" /* SERVER_REMOTE_WRITES_* */
#include "server_identity_jti.h"
#include "server_mgmt_jwks_cache.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

static const char *tier_text(kb_identity_tier_t tier)
{
   switch (tier)
   {
   case KB_IDENTITY_TIER_OFF:
      return "off";
   case KB_IDENTITY_TIER_DATA:
      return "data";
   case KB_IDENTITY_TIER_FULL:
      return "full";
   }
   return NULL;
}

int server_write_tier_replay_db1(void *ctx, const server_identity_token_claims_t *claims,
                                 int64_t now)
{
   (void)ctx;
   if (!claims)
      return -1;
   const char *tier = tier_text(claims->tier);
   if (!tier)
      return -1; /* an unrecognized tier is corrupt, not consumable */

   server_identity_jti_t record;
   memset(&record, 0, sizeof(record));
   record.jti = claims->jti;
   record.issuer = claims->issuer;
   record.kid = claims->kid;
   record.audience = claims->audience;
   record.subject = claims->subject;
   record.team_id = claims->team_id;
   record.tier = tier;
   record.issued_at = claims->issued_at;
   record.expires_at = claims->expires_at;

   switch (server_identity_jti_consume(&record, now))
   {
   case SERVER_IDENTITY_JTI_OK:
      return 0; /* previously unseen, now durably recorded */
   case SERVER_IDENTITY_JTI_REPLAY:
      return 1;
   case SERVER_IDENTITY_JTI_SATURATED:
   case SERVER_IDENTITY_JTI_STORAGE:
   case SERVER_IDENTITY_JTI_INVALID:
      break;
   }
   /* ONLY an explicit OK counts as fresh. Saturation, a storage fault and a
    * malformed record all mean the store did not record this jti, so treating
    * any of them as "not replayed" would let the same token be presented again
    * for as long as the condition lasts. They deny. */
   return -1;
}

/* --- runtime bindings: environment + JWKS cache ------------------------- */

/* Parse a positive team id. Anything else - unset, empty, non-numeric, zero,
 * negative, trailing junk - is "not configured" rather than a silent 0, because
 * a 0 team would compare unequal to every real team and deny with the wrong
 * reason. */
static int env_team_id(int64_t *out)
{
   const char *raw = getenv("AIMEE_SERVER_TEAM_ID");
   if (!raw || !raw[0])
      return 0;
   char *end = NULL;
   errno = 0;
   long long value = strtoll(raw, &end, 10);
   if (errno || !end || *end || value <= 0)
      return 0;
   *out = (int64_t)value;
   return 1;
}

int server_write_tier_team_configured(void)
{
   int64_t team = 0;
   return env_team_id(&team);
}

int server_write_tier_for_request(const char *token, size_t token_len, int64_t now,
                                  server_write_tier_outcome_t *outcome,
                                  server_identity_token_claims_t *claims_out)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (!outcome)
      outcome = &local;
   *outcome = SERVER_WRITE_TIER_INVALID;
   if (claims_out)
      memset(claims_out, 0, sizeof(*claims_out));

   /* No credential presented: report that plainly before doing any work, so the
    * ordinary read-only caller is never mistaken for a misconfiguration. */
   if (!token || token_len == 0 || token[0] == '\0')
   {
      *outcome = SERVER_WRITE_TIER_ABSENT;
      return SERVER_REMOTE_WRITES_OFF;
   }

   int64_t team = 0;
   if (!env_team_id(&team))
   {
      *outcome = SERVER_WRITE_TIER_NO_TEAM_CONFIGURED;
      return SERVER_REMOTE_WRITES_OFF;
   }

   const char *server_id = getenv("AIMEE_SERVER_ID");
   const char *bundle_path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   if (!server_id || !server_id[0] || !bundle_path || !bundle_path[0])
      return SERVER_REMOTE_WRITES_OFF; /* INVALID: this server cannot verify anything */

   char bundle[SERVER_MGMT_JWKS_BUNDLE_MAX];
   size_t bundle_len = 0;
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_len = 0;
   int resolved = SERVER_REMOTE_WRITES_OFF;
   if (server_mgmt_jwks_trust_bundle_load(bundle_path, bundle, sizeof(bundle), &bundle_len) == 0 &&
       server_mgmt_jwks_cache_load(bundle, bundle_len, now, jwks, sizeof(jwks), &jwks_len) ==
           SERVER_MGMT_JWKS_CACHE_OK &&
       jwks_len > 0)
   {
      server_write_tier_config_t config;
      memset(&config, 0, sizeof(config));
      config.jwks_json = jwks;
      config.expected_issuer = "kb"; /* proposal §4 pins iss=kb */
      config.expected_audience = server_id;
      config.enrolled_teams = &team;
      config.enrolled_team_count = 1; /* a server belongs to exactly one team */
      config.replay = server_write_tier_replay_db1;
      resolved = server_write_tier_resolve(token, token_len, &config, now, outcome, claims_out);
   }
   /* A JWKS we cannot load leaves *outcome INVALID: we cannot tell a forged
    * token from a good one without keys, so we must not guess. */
   OPENSSL_cleanse(bundle, sizeof(bundle));
   OPENSSL_cleanse(jwks, sizeof(jwks));
   return resolved;
}
