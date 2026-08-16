#include "kb_witness_cadence.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>

#include "modules/db2/c/db2_witness_checkpoint.h"
#include "modules/db2/c/db2_witness_emit.h"
#include "kb/kb_vault_policy.h"
#include "kb/kb_witness_gate_state.h"
#include "log.h"
#include "modules/vault/vault_witness_signer.h"
#include "util.h"

/* The cross-thread verification-result cell lives in kb_witness_gate_state.c so the
 * concurrency contract (a C11-atomic store/load between the cadence thread and the
 * egress-gate thread) is isolated and testable in one small unit. This accessor is
 * the public read the release gate calls. */
int kb_witness_verification_last_clean(void)
{
   return kb_witness_gate_state_get();
}

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
   /* Render the public trust anchor in the exact `<32-hex key_id>:<64-hex pubkey>`
    * form aimee-witness-verify's anchor file uses, so an operator can capture it from
    * the boot log of a key-holding kb (there is no separate export command yet). The
    * pubkey and key_id are PUBLIC; only the private seed is secret. */
   char
       anchor_line[VAULT_WITNESS_SIGNER_KEY_ID_LEN * 2 + 1 + VAULT_WITNESS_ED25519_PUB_LEN * 2 + 1];
   {
      size_t o = 0;
      for (size_t i = 0; i < VAULT_WITNESS_SIGNER_KEY_ID_LEN; i++)
         o += (size_t)snprintf(anchor_line + o, sizeof anchor_line - o, "%02x", key_id[i]);
      anchor_line[o++] = ':';
      for (size_t i = 0; i < VAULT_WITNESS_ED25519_PUB_LEN; i++)
         o += (size_t)snprintf(anchor_line + o, sizeof anchor_line - o, "%02x", pub[i]);
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
   /* Publish the trust anchor for operators to pin with retained copies. Logged once
    * at boot on the key-holding path; the values are public. */
   LOG_INFO("kb.witness", "trust anchor (pin this with retained copies): %s", anchor_line);
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

/* Continuous verification cadence and window. Re-verifying a bounded window keeps
 * the cost flat as the chain grows; older checkpoints are already covered by the
 * offline verifier over retained copies, which is the authority for history. */
#define KB_WITNESS_VERIFY_INTERVAL_S 300
#define KB_WITNESS_VERIFY_WINDOW     256

/* Test-only cadence override. A real-daemon kill/liveness harness needs the cadence
 * to fire in seconds, not minutes; production intervals would make such a test take
 * minutes per checkpoint. AIMEE_WITNESS_CADENCE_TEST_S, when set to a small integer
 * (1..60), shortens BOTH the checkpoint and verify intervals. It is a strict lower
 * bound only — it can never lengthen an interval or disable the cadence — so an
 * accidental production setting can at worst make checkpoints slightly more
 * frequent, never suppress them. Unset or malformed leaves the production values. */
static time_t witness_interval(time_t production_s)
{
   const char *v = getenv("AIMEE_WITNESS_CADENCE_TEST_S");
   if (!v || !v[0])
      return production_s;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (end && *end == '\0' && n >= 1 && n <= 60 && (time_t)n < production_s)
      return (time_t)n;
   return production_s;
}

static void emit_once(void)
{
   db2_witness_emit_stats_t s;
   db2_witness_emit_result_t r =
       db2_witness_emit_run(log_sink, NULL, KB_WITNESS_EMIT_MAX_PER_STREAM, &s);
   switch (r)
   {
   case DB2_WITNESS_EMIT_OK:
      if (s.records_emitted || s.checkpoints_emitted)
         LOG_DEBUG("kb.witness",
                   "emitted records=%llu checkpoints=%llu snapshots=%llu backlog=%llu/%llu",
                   (unsigned long long)s.records_emitted, (unsigned long long)s.checkpoints_emitted,
                   (unsigned long long)s.snapshots_emitted, (unsigned long long)s.backlog_records,
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
   time_t interval = witness_interval(KB_WITNESS_CHECKPOINT_INTERVAL_S);
   if (next == 0)
      next = now + interval;
   if (now < next)
      return;
   next = now + interval;

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

   /* Continuous verification of the retained checkpoint run. The producer already
    * re-verifies every shard head against its evidence log on each tick (the leaf
    * scan raises head_log_mismatch), so chain verification is continuous by
    * construction; what that does NOT cover is whether the signed roots over those
    * heads are themselves authentic and correctly linked. This closes that half.
    *
    * It runs on a slower cadence than checkpointing because it re-verifies a window
    * of history rather than one new root, and a compromise it would catch does not
    * become undetectable in the interim — the evidence is durable and the offline
    * verifier sees the same thing. */
   {
      /* The first verify runs on the first tick after boot (next_verify seeded to
       * `now`), so the release gate's "last verification clean" term becomes
       * available promptly rather than staying fail-closed for a full interval. */
      static time_t next_verify = 0;
      time_t verify_interval = witness_interval(KB_WITNESS_VERIFY_INTERVAL_S);
      if (next_verify == 0)
         next_verify = now;
      if (now >= next_verify)
      {
         next_verify = now + verify_interval;
         db2_witness_verify_report_t vr;
         if (db2_witness_checkpoint_verify_run(KB_WITNESS_VERIFY_WINDOW, &vr) != 0)
         {
            /* Could not verify is not verified. It is not proof of tampering
             * either, so this is a warning rather than an integrity alert — but it
             * must never be silent. The release gate reads this as NOT clean. */
            kb_witness_gate_state_set(0);
            LOG_WARN("kb.witness", "checkpoint verification could not run; evidence is unverified "
                                   "until it succeeds");
         }
         else if (vr.bad_signature > 0 || vr.unknown_key > 0 || vr.continuity_broken)
         {
            kb_witness_gate_state_set(0);
            LOG_ERROR("kb.witness",
                      "INTEGRITY: retained checkpoints failed verification "
                      "(checked=%lld bad_signature=%lld unknown_key=%lld continuity_broken=%d)",
                      (long long)vr.checked, (long long)vr.bad_signature, (long long)vr.unknown_key,
                      vr.continuity_broken);
         }
         else if (vr.continuity_unproven)
         {
            /* A gap and a fork are indistinguishable from the stored run alone, so
             * this is a work item for an operator to resolve by comparing retained
             * copies — deliberately not reported as clean, and not as tampering. The
             * release gate treats UNPROVEN as NOT clean (fail-closed): egress stays
             * closed until an operator resolves the gap. */
            kb_witness_gate_state_set(0);
            LOG_WARN("kb.witness",
                     "checkpoint continuity UNPROVEN over the retained window (checked=%lld); "
                     "compare cross-gap leaf sets against retained copies before concluding clean",
                     (long long)vr.checked);
         }
         else
         {
            /* Clean: signatures valid, keys known, continuity ok (including the
             * vacuous checked==0 case on a fresh kb — nothing to disagree with). */
            kb_witness_gate_state_set(1);
            LOG_DEBUG("kb.witness", "checkpoint run verified: %lld checkpoints",
                      (long long)vr.checked);
         }
      }
   }

   /* Emission runs after the checkpoint attempt so a checkpoint signed this tick
    * goes out on the same tick, and runs regardless of the checkpoint result:
    * records accrue whether or not a new root was signed, and a stalled checkpoint
    * is exactly when getting the record stream off-host matters most. */
   emit_once();
}
