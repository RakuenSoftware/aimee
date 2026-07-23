/* P7-witness-e3 §2: tamper detection proven end to end, from bytes alone.
 *
 * The umbrella's claim is deliberately conditional: locally-inconsistent tampering
 * is caught unconditionally, and coherent rewrite or rollback is caught only by
 * comparison against externally retained copies. These tests pin BOTH halves —
 * including the cases the checkpoint stream alone CANNOT catch, because a suite
 * that only demonstrates the easy detections would overstate the property.
 *
 * Everything here runs on captured bytes with no database, which is the posture
 * during a real incident. The scenarios needing a live store (locally inconsistent
 * rows, coherent local rewrite) live in test_witness_tamper_scenarios_pg.c.
 */
#include <assert.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_export.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_offline.h"
#include "modules/vault/vault_witness_record.h"

static uint8_t g_stream[1 << 16];
static size_t g_len;

static void reset_stream(void)
{
   g_len = 0;
}

static void frame_payload(vault_witness_export_kind_t kind, const uint8_t *payload, size_t plen)
{
   uint8_t frame[8192];
   size_t flen = 0;
   assert(vault_witness_export_frame(kind, payload, plen, frame, sizeof frame, &flen) == 0);
   assert(g_len + flen <= sizeof g_stream);
   memcpy(g_stream + g_len, frame, flen);
   g_len += flen;
}

static void gen_keypair(uint8_t priv[32], uint8_t pub[32])
{
   EVP_PKEY *pkey = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1);
   size_t l = 32;
   assert(EVP_PKEY_get_raw_private_key(pkey, priv, &l) == 1);
   l = 32;
   assert(EVP_PKEY_get_raw_public_key(pkey, pub, &l) == 1);
   EVP_PKEY_free(pkey);
   EVP_PKEY_CTX_free(ctx);
}

/* Build one record in a shard. `variant` perturbs the content so two records can
 * share a shard_seq while differing — which is what a fork looks like. */
static void make_record(vault_witness_record_t *r, uint64_t seq, int variant,
                        const uint8_t *pred)
{
   memset(r, 0, sizeof *r);
   r->source = VAULT_WITNESS_SRC_REWRAP;
   r->shard_seq = seq;
   r->is_first_in_shard = (seq == 1);
   r->seal_epoch = 1;
   r->fencing_token = 1;
   memset(r->source_hash, (int)(0x30 + variant), 32);
   snprintf(r->source_id, sizeof r->source_id, "s%llu-v%d", (unsigned long long)seq, variant);
   snprintf(r->tenant, sizeof r->tenant, "!kb");
   snprintf(r->provider, sizeof r->provider, "!audit");
   snprintf(r->timestamp, sizeof r->timestamp, "2026-07-23T00:00:%02lluZ",
            (unsigned long long)(seq % 60));
   if (seq == 1)
      assert(vault_witness_genesis_sentinel("!kb", "!audit", r->witness_pred_hash) == 0);
   else
      memcpy(r->witness_pred_hash, pred, 32);
}

static void emit_record(const vault_witness_record_t *r)
{
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t wl = 0;
   assert(vault_witness_record_encode(r, wire, sizeof wire, &wl) == 0);
   frame_payload(VAULT_WITNESS_EXPORT_RECORD, wire, wl);
}

/* Emit a chained run of `n` records and return the final head digest. */
static void emit_chain(size_t n, int variant, uint8_t head[32])
{
   uint8_t prev[32];
   memset(prev, 0, 32);
   for (size_t i = 1; i <= n; i++)
   {
      vault_witness_record_t r;
      make_record(&r, i, variant, prev);
      assert(vault_witness_record_digest(&r, prev) == 0);
      emit_record(&r);
   }
   memcpy(head, prev, 32);
}

/* Sign and emit a checkpoint over a single shard head. `pred` is the previous
 * checkpoint's digest, or NULL for the first. Returns this checkpoint's digest. */
static void emit_checkpoint(uint64_t seq, const uint8_t head[32], const uint8_t *pred,
                            const uint8_t priv[32], const uint8_t key_id[16], uint8_t out_dig[32],
                            int emit_it)
{
   vault_witness_leaf_t leaf;
   assert(vault_witness_shard_key_hash("!kb", "!audit", leaf.key) == 0);
   assert(vault_witness_leaf_hash("!kb", "!audit", seq, head, leaf.hash) == 0);
   uint8_t root[32];
   assert(vault_witness_merkle_root(&leaf, 1, root) == 0);

   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = seq;
   cp.has_predecessor = (pred != NULL);
   if (pred)
      memcpy(cp.predecessor_digest, pred, 32);
   cp.shard_count = 1;
   cp.sig_alg = VAULT_WITNESS_SIG_ED25519;
   cp.sig_version = 1;
   memcpy(cp.root, root, 32);
   memset(cp.leaf_snapshot_digest, 0x5A, 32);
   memcpy(cp.signer_key_id, key_id, 16);
   snprintf(cp.created_at, sizeof cp.created_at, "2026-07-23T00:0%llu:00Z",
            (unsigned long long)(seq % 10));
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);
   assert(vault_witness_checkpoint_digest(&cp, out_dig) == 0);

   if (emit_it)
   {
      uint8_t cw[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
      size_t cl = 0;
      assert(vault_witness_checkpoint_encode(&cp, cw, sizeof cw, &cl) == 0);
      frame_payload(VAULT_WITNESS_EXPORT_CHECKPOINT, cw, cl);
   }
}

/* ---------------------------------------------------------------------------
 * Scenario 3: a fork hidden behind a SUPPRESSED checkpoint.
 *
 * The attacker emits checkpoints A, B and E and withholds C and D, forking the
 * record interval in between. From bytes alone a withheld checkpoint and a fork
 * are indistinguishable, so the verifier must report continuity UNPROVEN — a work
 * item that stops the operator from concluding "clean" — and must NOT silently
 * pass. This is the affordance that makes suppression visible at all.
 */
static void test_fork_behind_suppressed_checkpoint(void)
{
   reset_stream();
   uint8_t priv[32], pub[32], key_id[16];
   gen_keypair(priv, pub);
   memset(key_id, 0xD0, 16);

   uint8_t head[32];
   emit_chain(3, 0, head);

   uint8_t dA[32], dB[32], dC[32], dD[32], dE[32];
   emit_checkpoint(1, head, NULL, priv, key_id, dA, 1);
   emit_checkpoint(2, head, dA, priv, key_id, dB, 1);
   /* C and D are produced (so the chain links) but deliberately NOT emitted. */
   emit_checkpoint(3, head, dB, priv, key_id, dC, 0);
   emit_checkpoint(4, head, dC, priv, key_id, dD, 0);
   emit_checkpoint(5, head, dD, priv, key_id, dE, 1);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   /* Every emitted checkpoint is genuinely signed — the suppression is not a
    * signature failure, which is exactly why continuity has to carry the signal. */
   assert(rep.checkpoints == 3 && rep.checkpoints_ok == 3);
   assert(rep.checkpoints_bad_sig == 0);
   assert(rep.continuity == VAULT_WITNESS_CONTINUITY_UNPROVEN);
   /* UNPROVEN is a work item, not proof of tampering: it must not be reported as a
    * hard tamper, and it must not be reported as clean either. The tool's exit code
    * is 0 with an explicit "continuity unproven" line for exactly this reason. */
   assert(rep.any_tamper == 0);
}

/* ---------------------------------------------------------------------------
 * Scenario 4: a fork BETWEEN two emitted checkpoints — the mandatory hard case.
 *
 * There is no gap at all. Both checkpoints are emitted, correctly signed, and
 * mutually consistent, because the attacker recomputed the interval coherently.
 * The continuity affordance does NOT fire here. Detection can only come from the
 * retained RECORD stream, where the same shard_seq carries two different records.
 * This is the scenario that proves the checkpoint stream alone is insufficient.
 */
static void test_fork_between_emitted_checkpoints(void)
{
   reset_stream();
   uint8_t priv[32], pub[32], key_id[16];
   gen_keypair(priv, pub);
   memset(key_id, 0xD1, 16);

   /* The honest run, as retained by a consumer that was listening at the time. */
   uint8_t head_true[32];
   emit_chain(3, 0, head_true);

   /* The attacker's rewritten interval: same positions, different content. Records
    * 2 and 3 are replaced; record 1 is byte-identical and must NOT be flagged. */
   uint8_t prev[32];
   {
      vault_witness_record_t r1;
      make_record(&r1, 1, 0, NULL);
      assert(vault_witness_record_digest(&r1, prev) == 0);
      emit_record(&r1); /* identical repeat: a benign duplicate, not a fork */
   }
   for (uint64_t seq = 2; seq <= 3; seq++)
   {
      vault_witness_record_t r;
      make_record(&r, seq, 9 /* different content */, prev);
      assert(vault_witness_record_digest(&r, prev) == 0);
      emit_record(&r);
   }
   uint8_t head_forged[32];
   memcpy(head_forged, prev, 32);

   /* Two properly signed, properly linked checkpoints over the FORGED head. No
    * gap, no signature problem, no continuity complaint. */
   uint8_t dA[32], dB[32];
   emit_checkpoint(1, head_forged, NULL, priv, key_id, dA, 1);
   emit_checkpoint(2, head_forged, dA, priv, key_id, dB, 1);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);

   /* The checkpoint stream is entirely happy — this is the point of the scenario. */
   assert(rep.checkpoints_ok == 2 && rep.checkpoints_bad_sig == 0);
   assert(rep.continuity == VAULT_WITNESS_CONTINUITY_OK);

   /* Detection comes from the record stream alone: two positions carry conflicting
    * records, while the untouched position is correctly treated as a duplicate. */
   assert(rep.records_conflict == 2);
   assert(rep.records_duplicate == 1);
   assert(rep.any_tamper == 1);
}

/* A rollback — the attacker replays an older, genuinely signed checkpoint as if it
 * were current — is caught the same way: the retained record stream still holds the
 * newer records, and the shorter re-emission cannot erase them. What a consumer
 * retained is the anchor; the local store is not consulted. */
static void test_rollback_caught_by_retained_records(void)
{
   reset_stream();
   uint8_t priv[32], pub[32], key_id[16];
   gen_keypair(priv, pub);
   memset(key_id, 0xD2, 16);

   uint8_t head5[32];
   emit_chain(5, 0, head5); /* what the consumer retained: five records */

   /* The attacker rolls the store back to three records and re-emits a rewritten
    * tail. Positions 4 and 5 now disagree with what was retained. */
   uint8_t prev[32];
   memset(prev, 0, 32);
   for (uint64_t seq = 1; seq <= 3; seq++)
   {
      vault_witness_record_t r;
      make_record(&r, seq, 0, prev);
      assert(vault_witness_record_digest(&r, prev) == 0);
      emit_record(&r);
   }
   for (uint64_t seq = 4; seq <= 5; seq++)
   {
      vault_witness_record_t r;
      make_record(&r, seq, 7, prev);
      assert(vault_witness_record_digest(&r, prev) == 0);
      emit_record(&r);
   }

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.records_duplicate == 3); /* 1..3 replayed identically */
   assert(rep.records_conflict == 2);  /* 4..5 rewritten */
   assert(rep.any_tamper == 1);
}

/* The honest baseline. Without this the suite proves only that the verifier is
 * eager to cry tamper; a detector that always fires detects nothing. */
static void test_honest_run_is_clean(void)
{
   reset_stream();
   uint8_t priv[32], pub[32], key_id[16];
   gen_keypair(priv, pub);
   memset(key_id, 0xD3, 16);

   uint8_t head[32];
   emit_chain(4, 0, head);
   uint8_t dA[32], dB[32];
   emit_checkpoint(1, head, NULL, priv, key_id, dA, 1);
   emit_checkpoint(2, head, dA, priv, key_id, dB, 1);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.any_tamper == 0 && rep.malformed == 0);
   assert(rep.records_conflict == 0 && rep.shards_broken == 0);
   assert(rep.continuity == VAULT_WITNESS_CONTINUITY_OK);
   assert(rep.checkpoints_ok == 2);
}

int main(void)
{
   test_honest_run_is_clean();
   test_fork_behind_suppressed_checkpoint();
   test_fork_between_emitted_checkpoints();
   test_rollback_caught_by_retained_records();
   printf("test_witness_tamper_scenarios: all passed\n");
   return 0;
}
