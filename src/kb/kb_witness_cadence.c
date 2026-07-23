#include "kb_witness_cadence.h"

#include <openssl/crypto.h>
#include <stdio.h>

#include "db2/db2_witness_checkpoint.h"
#include "kb/kb_vault_policy.h"
#include "log.h"
#include "modules/vault/vault_witness_signer.h"

int kb_witness_boot_check(char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   /* Only a key-holding kb (real external anchor, live keys allowed) must be able
    * to sign witness evidence. A dev/no-live-key kb witnesses nothing that gates a
    * real key, so the signer is not required at boot. */
   if (!kb_vault_live_keys_allowed())
      return 0;
   uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN], key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   if (vault_witness_signer_identity(pub, key_id) != 0)
   {
      if (err && errlen)
         snprintf(err, errlen,
                  "witness signing key not derivable; a key-holding kb refuses to start without a "
                  "working checkpoint signer");
      return -1;
   }
   OPENSSL_cleanse(pub, sizeof pub);
   OPENSSL_cleanse(key_id, sizeof key_id);
   return 0;
}

void kb_witness_cadence_tick(time_t now)
{
   static time_t next = 0;
   if (next == 0)
      next = now + KB_WITNESS_CHECKPOINT_INTERVAL_S;
   if (now < next)
      return;
   next = now + KB_WITNESS_CHECKPOINT_INTERVAL_S;

   int64_t seq = -1;
   db2_witness_checkpoint_result_t r = db2_witness_checkpoint_produce(&seq);
   switch (r)
   {
   case DB2_WITNESS_CP_OK:
      LOG_DEBUG("kb.witness", "checkpoint signed: seq=%lld", (long long)seq);
      break;
   case DB2_WITNESS_CP_EMPTY:
   case DB2_WITNESS_CP_TRANSIENT:
   case DB2_WITNESS_CP_FENCE_STALE:
      /* Benign: no evidence yet, a retryable serialization loss, or a fence race.
       * The next tick retries; none of these is a tamper signal. */
      break;
   case DB2_WITNESS_CP_HEAD_MISMATCH:
      /* A shard head diverged from its log: the checkpoint cross-check refused to
       * sign. This is an integrity alert, not a crash — appends and egress
       * continue, but new signed roots stop until an operator resolves it. */
      LOG_ERROR("kb.witness",
                "INTEGRITY: checkpoint refused, shard head does not match evidence log "
                "(head_log_mismatch); latest signed root is stale until resolved");
      break;
   case DB2_WITNESS_CP_CEILING:
      LOG_ERROR("kb.witness",
                "INTEGRITY: checkpoint refused, shard count exceeds ceiling; latest signed "
                "root is stale until resolved");
      break;
   case DB2_WITNESS_CP_ERROR:
   default:
      LOG_WARN("kb.witness", "checkpoint attempt failed (transient/config); will retry");
      break;
   }
}
