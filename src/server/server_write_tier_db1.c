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
#include "server_runtime_identity.h"

#include <openssl/crypto.h>
#include <string.h>
#include <unistd.h> /* access — startup preflight only */

/* One definition for every server-id buffer in this file. The audience the
 * verifier compares against lives in one of these, so its size is a contract,
 * not a local choice. */
#define SERVER_WRITE_TIER_SERVER_ID_MAX 128

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

int server_write_tier_team_configured(void)
{
   int64_t team = 0;
   char server_id[SERVER_WRITE_TIER_SERVER_ID_MAX];
   return server_runtime_identity_load(server_id, sizeof(server_id), &team) ==
          SERVER_RUNTIME_IDENTITY_READY;
}

server_write_tier_config_state_t server_write_tier_config_state(void)
{
   int64_t team = 0;
   char server_id[SERVER_WRITE_TIER_SERVER_ID_MAX];
   server_runtime_identity_state_t state =
       server_runtime_identity_load(server_id, sizeof(server_id), &team);
   if (state == SERVER_RUNTIME_IDENTITY_NO_TEAM)
      return SERVER_WRITE_TIER_CONFIG_NO_TEAM;
   if (state == SERVER_RUNTIME_IDENTITY_NO_SERVER_ID)
      return SERVER_WRITE_TIER_CONFIG_NO_SERVER_ID;
   const char *bundle_path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   if (!bundle_path || !bundle_path[0])
      return SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE;
   /* The variable being SET is not the input; the bundle being THERE is. The
    * shipped standalone compose defaults this to a conventional path nothing
    * mounts, so a deployment that never provisioned an authority reported READY
    * while every KB-issued token was in fact denied — and the precise startup
    * error naming this variable was suppressed in favour of a vague recurring
    * "management JWKS authorization unavailable" warning.
    *
    * Readability only: this is a startup preflight answering "did the operator
    * supply the inputs". Structural validation stays in build_config, which
    * fails closed per request. Called once at startup, so the stat is free. */
   if (access(bundle_path, R_OK) != 0)
      return SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE;
   return SERVER_WRITE_TIER_CONFIG_READY;
}

/* Assemble the resolver config from this server's environment and JWKS cache.
 * Returns 1 on success. On failure sets *outcome to the specific reason so the
 * caller never has to guess whether the problem was the token or the server. */
/* server_id is a CALLER-OWNED buffer for the same reason jwks and bundle are:
 * config->expected_audience points at it, and the config outlives this function.
 * It used to be a local here, so the audience the verifier compared against was
 * a pointer into a dead stack frame -- undefined behaviour that happened to hold
 * the right bytes until an unrelated change to the caller's code disturbed the
 * frame, at which point every identity token was rejected as INVALID. */
static int build_config(server_write_tier_config_t *config, char *jwks, size_t jwks_cap,
                        int64_t *team, char *bundle, size_t bundle_cap, char *server_id,
                        size_t server_id_cap, int64_t now, server_write_tier_outcome_t *outcome)
{
   memset(config, 0, sizeof(*config));
   if (server_runtime_identity_load(server_id, server_id_cap, team) !=
       SERVER_RUNTIME_IDENTITY_READY)
   {
      *outcome = SERVER_WRITE_TIER_NO_TEAM_CONFIGURED;
      return 0;
   }
   const char *bundle_path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   if (!bundle_path || !bundle_path[0])
   {
      *outcome = SERVER_WRITE_TIER_INVALID;
      return 0;
   }
   size_t bundle_len = 0, jwks_len = 0;
   if (server_mgmt_jwks_trust_bundle_load(bundle_path, bundle, bundle_cap, &bundle_len) != 0 ||
       server_mgmt_jwks_cache_load(bundle, bundle_len, now, jwks, jwks_cap, &jwks_len) !=
           SERVER_MGMT_JWKS_CACHE_OK ||
       jwks_len == 0)
   {
      /* Without keys we cannot tell a forged token from a good one, so we must
       * not guess. */
      *outcome = SERVER_WRITE_TIER_INVALID;
      return 0;
   }
   config->jwks_json = jwks;
   config->expected_issuer = "kb"; /* proposal §4 pins iss=kb */
   config->expected_audience = server_id;
   config->enrolled_teams = team;
   config->enrolled_team_count = 1; /* a server belongs to exactly one team */
   config->replay = server_write_tier_replay_db1;
   return 1;
}

int server_write_tier_verify_for_request(const char *token, size_t token_len, int64_t now,
                                         server_write_tier_outcome_t *outcome,
                                         server_identity_token_claims_t *claims_out)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (!outcome)
      outcome = &local;
   *outcome = SERVER_WRITE_TIER_INVALID;
   if (claims_out)
      memset(claims_out, 0, sizeof(*claims_out));

   /* Report an absent credential before doing any work, so the ordinary
    * read-only caller is never mistaken for a misconfiguration. */
   if (!token || token_len == 0 || token[0] == '\0')
   {
      *outcome = SERVER_WRITE_TIER_ABSENT;
      return SERVER_REMOTE_WRITES_OFF;
   }

   server_write_tier_config_t config;
   char bundle[SERVER_MGMT_JWKS_BUNDLE_MAX];
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   /* Outlives the config it is pointed into; see build_config. */
   char server_id[SERVER_WRITE_TIER_SERVER_ID_MAX];
   int64_t team = 0;
   int tier = SERVER_REMOTE_WRITES_OFF;
   if (build_config(&config, jwks, sizeof(jwks), &team, bundle, sizeof(bundle), server_id,
                    sizeof(server_id), now, outcome))
      tier = server_write_tier_verify(token, token_len, &config, now, outcome, claims_out);
   OPENSSL_cleanse(bundle, sizeof(bundle));
   OPENSSL_cleanse(jwks, sizeof(jwks));
   return tier;
}

int server_write_tier_consume_for_request(const server_identity_token_claims_t *claims, int64_t now,
                                          server_write_tier_outcome_t *outcome)
{
   server_write_tier_outcome_t local = SERVER_WRITE_TIER_INVALID;
   if (!outcome)
      outcome = &local;
   *outcome = SERVER_WRITE_TIER_INVALID;
   if (!claims)
      return SERVER_REMOTE_WRITES_OFF;

   /* Consumption needs only the replay hook, but it goes through the same
    * builder so a server that cannot verify also cannot spend. */
   server_write_tier_config_t config;
   char bundle[SERVER_MGMT_JWKS_BUNDLE_MAX];
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   /* Outlives the config it is pointed into; see build_config. */
   char server_id[SERVER_WRITE_TIER_SERVER_ID_MAX];
   int64_t team = 0;
   int tier = SERVER_REMOTE_WRITES_OFF;
   if (build_config(&config, jwks, sizeof(jwks), &team, bundle, sizeof(bundle), server_id,
                    sizeof(server_id), now, outcome))
      tier = server_write_tier_consume(claims, &config, now, outcome);
   OPENSSL_cleanse(bundle, sizeof(bundle));
   OPENSSL_cleanse(jwks, sizeof(jwks));
   return tier;
}
