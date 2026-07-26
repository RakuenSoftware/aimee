/* server_write_tier_db1.c — bridges the pure write-tier policy to the durable
 * replay store.
 *
 * Deliberately a separate translation unit from server_write_tier.c: that file
 * must stay free of storage dependencies so its decision logic can be unit
 * tested without a database. This file is the only place the two meet. */

#include "server_write_tier_db1.h"

#include "server_identity_jti.h"

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
