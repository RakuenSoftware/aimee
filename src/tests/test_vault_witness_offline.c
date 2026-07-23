#include "modules/vault/vault_witness_offline.h"

#include <assert.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_export.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_proof.h"
#include "modules/vault/vault_witness_record.h"

/* A growable stream builder. */
static uint8_t g_stream[65536];
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

/* Emit a chained record run for one shard. */
static void emit_shard_records(const char *tenant, const char *provider, size_t n)
{
   uint8_t prev[32];
   for (size_t i = 0; i < n; i++)
   {
      vault_witness_record_t r;
      memset(&r, 0, sizeof r);
      r.source = VAULT_WITNESS_SRC_REWRAP;
      r.shard_seq = i + 1;
      r.is_first_in_shard = (i == 0);
      r.seal_epoch = 1;
      r.fencing_token = 1;
      memset(r.source_hash, (int)(0x20 + i), 32);
      snprintf(r.source_id, sizeof r.source_id, "s%zu", i);
      snprintf(r.tenant, sizeof r.tenant, "%s", tenant);
      snprintf(r.provider, sizeof r.provider, "%s", provider);
      snprintf(r.timestamp, sizeof r.timestamp, "2026-07-23T00:00:%02zuZ", i % 60);
      if (i == 0)
         assert(vault_witness_genesis_sentinel(tenant, provider, r.witness_pred_hash) == 0);
      else
         memcpy(r.witness_pred_hash, prev, 32);
      assert(vault_witness_record_digest(&r, prev) == 0);
      uint8_t wire[VAULT_WITNESS_RECORD_MAX];
      size_t wl = 0;
      assert(vault_witness_record_encode(&r, wire, sizeof wire, &wl) == 0);
      frame_payload(VAULT_WITNESS_EXPORT_RECORD, wire, wl);
   }
}

/* Build a one-leaf SMT for a shard head, sign a checkpoint over it, emit both the
 * checkpoint and an inclusion proof. Returns the anchor for verification. */
static void emit_checkpoint_and_proof(const char *tenant, const char *provider, uint64_t seq,
                                      const uint8_t head[32], const uint8_t priv[32],
                                      const uint8_t pub[32], vault_witness_anchor_t *anchor)
{
   vault_witness_leaf_t leaf;
   assert(vault_witness_shard_key_hash(tenant, provider, leaf.key) == 0);
   assert(vault_witness_leaf_hash(tenant, provider, seq, head, leaf.hash) == 0);
   uint8_t root[32];
   assert(vault_witness_merkle_root(&leaf, 1, root) == 0);

   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = 1;
   cp.has_predecessor = 0;
   cp.shard_count = 1;
   cp.sig_alg = VAULT_WITNESS_SIG_ED25519;
   cp.sig_version = 1;
   memcpy(cp.root, root, 32);
   memset(cp.leaf_snapshot_digest, 0x55, 32);
   uint8_t key_id[16];
   memset(key_id, 0xC0, 16);
   memcpy(cp.signer_key_id, key_id, 16);
   snprintf(cp.created_at, sizeof cp.created_at, "2026-07-23T00:00:00Z");
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);

   uint8_t cw[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
   size_t cl = 0;
   assert(vault_witness_checkpoint_encode(&cp, cw, sizeof cw, &cl) == 0);
   frame_payload(VAULT_WITNESS_EXPORT_CHECKPOINT, cw, cl);

   vault_witness_proof_t p;
   memset(&p, 0, sizeof p);
   p.checkpoint_seq = 1;
   snprintf(p.tenant, sizeof p.tenant, "%s", tenant);
   snprintf(p.provider, sizeof p.provider, "%s", provider);
   p.sequence = seq;
   memcpy(p.head_hash, head, 32);
   assert(vault_witness_merkle_proof(&leaf, 1, 0, p.path) == 0);
   uint8_t pw[VAULT_WITNESS_PROOF_WIRE_MAX];
   size_t pl = 0;
   assert(vault_witness_proof_encode(&p, pw, sizeof pw, &pl) == 0);
   frame_payload(VAULT_WITNESS_EXPORT_PROOF, pw, pl);

   memset(anchor, 0, sizeof *anchor);
   memcpy(anchor->key_id, key_id, 16);
   memcpy(anchor->ed25519_pub, pub, 32);
}

static void test_clean_stream(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t head[32];
   memset(head, 0x99, 32);
   vault_witness_anchor_t anchor;
   emit_shard_records("!kb", "!audit", 4);
   emit_checkpoint_and_proof("!kb", "!audit", 4, head, priv, pub, &anchor);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.any_tamper == 0);
   assert(rep.records == 4 && rep.shards_ok == 1 && rep.shards_broken == 0);
   assert(rep.checkpoints == 1 && rep.checkpoints_ok == 1);
   assert(rep.proofs == 1 && rep.proofs_ok == 1 && rep.proofs_bad == 0);
   assert(rep.malformed == 0);
}

static void test_tampered_record(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t head[32];
   memset(head, 0x99, 32);
   vault_witness_anchor_t anchor;
   emit_shard_records("!kb", "!audit", 4);
   emit_checkpoint_and_proof("!kb", "!audit", 4, head, priv, pub, &anchor);

   /* Corrupt a byte inside the first record's frame payload (a hash region). Flip a
    * byte well past the export + record headers so it lands in a hash field. */
   g_stream[16 + 60] ^= 0xFF;

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.any_tamper == 1);
}

static void test_wrong_anchor(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t head[32];
   memset(head, 0x99, 32);
   vault_witness_anchor_t anchor;
   emit_shard_records("!kb", "!audit", 2);
   emit_checkpoint_and_proof("!kb", "!audit", 4, head, priv, pub, &anchor);

   /* Verify with a different pubkey -> checkpoint signature fails. */
   uint8_t priv2[32], pub2[32];
   gen_keypair(priv2, pub2);
   vault_witness_anchor_t bad = anchor;
   memcpy(bad.ed25519_pub, pub2, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &bad, 1, &rep) == 0);
   assert(rep.checkpoints_bad_sig == 1 && rep.any_tamper == 1);
}

static void test_malformed_frame(void)
{
   reset_stream();
   /* A truncated frame header. */
   uint8_t junk[10] = {0};
   assert(vault_witness_offline_verify(junk, sizeof junk, NULL, 0,
                                       &(vault_witness_offline_report_t){0}) == 0);
}


/* A retained stream that repeats records byte-identically (re-emission after a
 * restart, or a collector retry) must still verify clean. */
static void test_duplicate_records_tolerated(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t head[32];
   memset(head, 0x99, 32);
   vault_witness_anchor_t anchor;
   emit_shard_records("!kb", "!audit", 3);
   emit_shard_records("!kb", "!audit", 3); /* exact re-emission of the same run */
   emit_checkpoint_and_proof("!kb", "!audit", 3, head, priv, pub, &anchor);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.records == 6 && rep.records_duplicate == 3);
   assert(rep.records_conflict == 0);
   assert(rep.shards_ok == 1 && rep.shards_broken == 0);
   assert(rep.any_tamper == 0);
}

/* Two DIFFERENT records at the same shard_seq is a fork: hard tamper evidence. */
static void test_fork_conflict_detected(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t head[32];
   memset(head, 0x99, 32);
   vault_witness_anchor_t anchor;
   emit_shard_records("!kb", "!audit", 3);

   /* Emit a second, DIFFERENT record at shard_seq 2 (same position, other content). */
   vault_witness_record_t f;
   memset(&f, 0, sizeof f);
   f.source = VAULT_WITNESS_SRC_REWRAP;
   f.shard_seq = 2;
   f.is_first_in_shard = 0;
   f.seal_epoch = 1;
   f.fencing_token = 1;
   memset(f.source_hash, 0xF0, 32);       /* different content */
   memset(f.witness_pred_hash, 0xA5, 32); /* some other predecessor */
   snprintf(f.source_id, sizeof f.source_id, "forked");
   snprintf(f.tenant, sizeof f.tenant, "!kb");
   snprintf(f.provider, sizeof f.provider, "!audit");
   snprintf(f.timestamp, sizeof f.timestamp, "2026-07-23T00:00:09Z");
   uint8_t fw[VAULT_WITNESS_RECORD_MAX];
   size_t fl = 0;
   assert(vault_witness_record_encode(&f, fw, sizeof fw, &fl) == 0);
   frame_payload(VAULT_WITNESS_EXPORT_RECORD, fw, fl);

   emit_checkpoint_and_proof("!kb", "!audit", 3, head, priv, pub, &anchor);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.records_conflict == 1);
   assert(rep.any_tamper == 1);
}

/* Emit a checkpoint whose leaf_snapshot_digest genuinely covers `snap`, plus the
 * matching snapshot frame. Returns the anchor. */
static void emit_checkpoint_with_snapshot(uint64_t cp_seq, const uint8_t *snap, size_t snap_len,
                                          const uint8_t priv[32], const uint8_t pub[32],
                                          vault_witness_anchor_t *anchor)
{
   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = cp_seq;
   cp.has_predecessor = 0;
   cp.shard_count = 1;
   cp.sig_alg = VAULT_WITNESS_SIG_ED25519;
   cp.sig_version = 1;
   memset(cp.root, 0x77, 32);
   SHA256(snap_len ? snap : (const uint8_t *)"", snap_len, cp.leaf_snapshot_digest);
   uint8_t key_id[16];
   memset(key_id, 0xC1, 16);
   memcpy(cp.signer_key_id, key_id, 16);
   snprintf(cp.created_at, sizeof cp.created_at, "2026-07-23T00:00:00Z");
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);

   uint8_t cw[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
   size_t cl = 0;
   assert(vault_witness_checkpoint_encode(&cp, cw, sizeof cw, &cl) == 0);
   frame_payload(VAULT_WITNESS_EXPORT_CHECKPOINT, cw, cl);

   uint8_t payload[512];
   assert(8 + snap_len <= sizeof payload);
   for (unsigned i = 0; i < 8; i++)
      payload[i] = (uint8_t)(cp_seq >> (56U - 8U * i));
   memcpy(payload + 8, snap, snap_len);
   frame_payload(VAULT_WITNESS_EXPORT_SNAPSHOT, payload, 8 + snap_len);

   memset(anchor, 0, sizeof *anchor);
   memcpy(anchor->key_id, key_id, 16);
   memcpy(anchor->ed25519_pub, pub, 32);
}

/* A snapshot whose bytes hash to the digest the checkpoint signature commits to
 * verifies; this is what lets a consumer rebuild the leaf set offline. */
static void test_snapshot_verified(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t snap[64];
   for (unsigned i = 0; i < sizeof snap; i++)
      snap[i] = (uint8_t)(i * 7 + 3);
   vault_witness_anchor_t anchor;
   emit_checkpoint_with_snapshot(1, snap, sizeof snap, priv, pub, &anchor);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.snapshots == 1 && rep.snapshots_ok == 1);
   assert(rep.snapshots_bad == 0 && rep.snapshots_unmatched == 0);
   assert(rep.any_tamper == 0);
}

/* A substituted snapshot cannot pass: the signature already commits to the digest
 * of the real leaf set, so any edit is caught. */
static void test_snapshot_substituted_detected(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t snap[64];
   memset(snap, 0x11, sizeof snap);
   vault_witness_anchor_t anchor;
   emit_checkpoint_with_snapshot(1, snap, sizeof snap, priv, pub, &anchor);

   /* Flip a byte inside the snapshot payload (past the frame header and the u64
    * checkpoint seq) — the checkpoint frame itself is left untouched, so its
    * signature still verifies and only the snapshot check can catch this. */
   g_stream[g_len - 1] ^= 0xFF;

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.checkpoints_ok == 1); /* signature still good */
   assert(rep.snapshots_bad == 1 && rep.snapshots_ok == 0);
   assert(rep.any_tamper == 1);
}

/* A snapshot with no checkpoint in the stream is unverifiable, not tampered. */
static void test_snapshot_unmatched_is_not_tamper(void)
{
   reset_stream();
   uint8_t payload[16] = {0};
   payload[7] = 9; /* checkpoint seq 9, which is not in this stream */
   frame_payload(VAULT_WITNESS_EXPORT_SNAPSHOT, payload, sizeof payload);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, NULL, 0, &rep) == 0);
   assert(rep.snapshots == 1 && rep.snapshots_unmatched == 1);
   assert(rep.snapshots_bad == 0 && rep.any_tamper == 0);
}

/* A retained stream that repeats a checkpoint byte-identically (emission re-sends
 * one whose snapshot was rejected; a reset cursor re-sends the run) must verify
 * clean. Before this was handled the duplicate read as CONTINUITY_BROKEN, which is
 * a false tampering alarm on a healthy system. */
static void test_duplicate_checkpoint_tolerated(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t snap[32];
   memset(snap, 0x31, sizeof snap);
   vault_witness_anchor_t anchor;
   emit_checkpoint_with_snapshot(1, snap, sizeof snap, priv, pub, &anchor);
   /* Re-emit the identical checkpoint frame by rebuilding the same stream twice. */
   size_t one = g_len;
   assert(g_len * 2 <= sizeof g_stream);
   memcpy(g_stream + g_len, g_stream, one);
   g_len += one;

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.checkpoints == 2 && rep.checkpoints_duplicate == 1);
   assert(rep.checkpoints_conflict == 0);
   assert(rep.continuity == VAULT_WITNESS_CONTINUITY_OK);
   assert(rep.any_tamper == 0);
}

/* Two DIFFERENT checkpoints at one seq is a fork: the signer certified two
 * histories. That must be hard tamper evidence, not collapsed away. */
static void test_conflicting_checkpoint_is_fork(void)
{
   reset_stream();
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   uint8_t snapA[32], snapB[32];
   memset(snapA, 0x41, sizeof snapA);
   memset(snapB, 0x42, sizeof snapB); /* different leaf set at the same seq */
   vault_witness_anchor_t anchor;
   emit_checkpoint_with_snapshot(1, snapA, sizeof snapA, priv, pub, &anchor);
   emit_checkpoint_with_snapshot(1, snapB, sizeof snapB, priv, pub, &anchor);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   assert(rep.checkpoints_conflict == 1);
   assert(rep.checkpoints_duplicate == 0);
   assert(rep.any_tamper == 1);
}

int main(void)
{
   test_clean_stream();
   test_duplicate_records_tolerated();
   test_fork_conflict_detected();
   test_tampered_record();
   test_wrong_anchor();
   test_malformed_frame();
   test_snapshot_verified();
   test_snapshot_substituted_detected();
   test_snapshot_unmatched_is_not_tamper();
   test_duplicate_checkpoint_tolerated();
   test_conflicting_checkpoint_is_fork();
   printf("test_vault_witness_offline: all passed\n");
   return 0;
}
