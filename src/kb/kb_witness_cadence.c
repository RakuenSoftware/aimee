#include "kb_witness_cadence.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>

#include "db2/db2_witness_checkpoint.h"
#include "db2/db2_witness_emit.h"
#include "kb/kb_vault_policy.h"
#include "log.h"
#include "modules/vault/vault_witness_signer.h"
#include "util.h"

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

   /* Anchor coverage: every retained checkpoint must name a key this kb can
    * actually verify with. Holding evidence it cannot check is exactly the state a
    * key-holding kb must refuse to start in, so both "a foreign key id is present"
    * and "the check could not be run" fail closed — "could not verify" is not
    * "verified". */
   int64_t unknown = 0;
   char sample[64] = "";
   int cov = db2_witness_checkpoint_anchor_coverage(key_id, sizeof key_id, &unknown, sample,
                                                    sizeof sample);
   OPENSSL_cleanse(key_id, sizeof key_id);
   if (cov != 0)
   {
      if (err && errlen)
         snprintf(err, errlen,
                  "witness anchor coverage could not be checked; a key-holding kb refuses to "
                  "start unable to confirm it can verify its own retained checkpoints");
      return -1;
   }
   if (unknown > 0)
   {
      if (err && errlen)
         snprintf(err, errlen,
                  "%lld retained witness checkpoint(s) are signed by a key this kb cannot derive "
                  "(e.g. signer_key_id %s); it cannot verify its own evidence and refuses to "
                  "start. This means a database from another install, or tampering.",
                  (long long)unknown, sample[0] ? sample : "unknown");
      return -1;
   }
   return 0;
}

/* The log sink. Evidence rides the log path as base64 of the exact export frame —
 * bytes, not a re-rendering — so a consumer that retains these lines can feed them
 * straight to aimee-witness-verify after decoding. The kind tag is redundant with
 * the frame header and is present only so an operator can grep one stream apart
 * from another; the verifier reads the header, never the tag. */
static const char *kind_tag(vault_witness_export_kind_t k)
{
   switch (k)
   {
   case VAULT_WITNESS_EXPORT_RECORD:
      return "record";
   case VAULT_WITNESS_EXPORT_CHECKPOINT:
      return "checkpoint";
   case VAULT_WITNESS_EXPORT_PROOF:
      return "proof";
   case VAULT_WITNESS_EXPORT_SNAPSHOT:
      return "snapshot";
   default:
      return "unknown";
   }
}

static int log_sink(void *ctx, vault_witness_export_kind_t kind, const uint8_t *frame, size_t len)
{
   (void)ctx;
   size_t need = aimee_base64_encoded_len(len);
   char *b64 = malloc(need);
   if (!b64)
      return -1; /* not emitted; the cursor stops here and the next tick retries */
   if (aimee_base64_encode(frame, len, b64, need) == 0)
   {
      free(b64);
      return -1;
   }
   LOG_INFO("kb.witness.evidence", "kind=%s b64=%s", kind_tag(kind), b64);
   free(b64);
   return 0;
}

/* Per-stream drain budget for one tick. Emission reads committed state and frames
 * bytes, so the per-item cost is small; the budget exists to bound the tail, not
 * the common case. Sized so a burst of several thousand events reaches an off-host
 * consumer on the next tick rather than trickling out over an hour, while a
 * pathological backlog still cannot monopolise the periodic loop. When it bites,
 * the backlog gauge reports what is still outstanding. */
#define KB_WITNESS_EMIT_MAX_PER_STREAM 8192

static void emit_once(void)
{
   db2_witness_emit_stats_t s;
   db2_witness_emit_result_t r = db2_witness_emit_run(log_sink, NULL, KB_WITNESS_EMIT_MAX_PER_STREAM, &s);
   switch (r)
   {
   case DB2_WITNESS_EMIT_OK:
      if (s.records_emitted || s.checkpoints_emitted)
         LOG_DEBUG("kb.witness",
                   "emitted records=%llu checkpoints=%llu snapshots=%llu backlog=%llu/%llu",
                   (unsigned long long)s.records_emitted,
                   (unsigned long long)s.checkpoints_emitted,
                   (unsigned long long)s.snapshots_emitted,
                   (unsigned long long)s.backlog_records,
                   (unsigned long long)s.backlog_checkpoints);
      break;
   case DB2_WITNESS_EMIT_PARITY_MISMATCH:
      /* The stored row and its canonical encoding disagree. Emitting past this
       * would publish evidence that can never match the store, so emission stops
       * here and stays stopped until an operator resolves it. Admission is
       * untouched: the durable log is still the system of record. */
      LOG_ERROR("kb.witness",
                "INTEGRITY: witness record digest parity failed; emission halted at the "
                "offending record (stored hash does not match its canonical encoding)");
      break;
   case DB2_WITNESS_EMIT_SINK_FAILED:
      LOG_WARN("kb.witness", "evidence emission sink rejected a frame; backlog will retry");
      break;
   case DB2_WITNESS_EMIT_TRANSIENT:
      break; /* no connection or a retryable read failure; next tick retries */
   case DB2_WITNESS_EMIT_ERROR:
   default:
      LOG_WARN("kb.witness", "evidence emission failed; will retry");
      break;
   }
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

   /* Emission runs after the checkpoint attempt so a checkpoint signed this tick
    * goes out on the same tick, and runs regardless of the checkpoint result:
    * records accrue whether or not a new root was signed, and a stalled checkpoint
    * is exactly when getting the record stream off-host matters most. */
   emit_once();
}
