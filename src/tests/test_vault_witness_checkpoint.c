#include "modules/vault/vault_witness_checkpoint.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Generate a raw Ed25519 keypair for the test. */
static void gen_keypair(uint8_t priv[32], uint8_t pub[32])
{
   EVP_PKEY *pkey = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1);
   size_t l = 32;
   assert(EVP_PKEY_get_raw_private_key(pkey, priv, &l) == 1 && l == 32);
   l = 32;
   assert(EVP_PKEY_get_raw_public_key(pkey, pub, &l) == 1 && l == 32);
   EVP_PKEY_free(pkey);
   EVP_PKEY_CTX_free(ctx);
}

static vault_witness_checkpoint_t fixture(void)
{
   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = 12;
   cp.has_predecessor = 1;
   cp.shard_count = 40;
   cp.sig_alg = VAULT_WITNESS_SIG_ED25519;
   cp.sig_version = 1;
   for (int i = 0; i < 32; i++)
   {
      cp.root[i] = (uint8_t)(0x10 + i);
      cp.predecessor_digest[i] = (uint8_t)(0x30 + i);
      cp.leaf_snapshot_digest[i] = (uint8_t)(0x50 + i);
   }
   for (int i = 0; i < VAULT_WITNESS_SIGNER_KEY_ID_LEN; i++)
      cp.signer_key_id[i] = (uint8_t)(0xA0 + i);
   snprintf(cp.created_at, sizeof cp.created_at, "2026-07-23T12:00:00Z");
   return cp;
}

static void test_sign_verify(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cp = fixture();
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor.ed25519_pub, pub, 32);
   assert(vault_witness_checkpoint_verify(&cp, &anchor, 1) == VAULT_WITNESS_CP_OK);

   /* Tampered root -> bad signature. */
   vault_witness_checkpoint_t t = cp;
   t.root[0] ^= 0xFF;
   assert(vault_witness_checkpoint_verify(&t, &anchor, 1) == VAULT_WITNESS_CP_BAD_SIG);
}

static void test_unknown_and_revoked_key(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cp = fixture();
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);

   /* Empty anchor set -> unknown key. */
   assert(vault_witness_checkpoint_verify(&cp, NULL, 0) == VAULT_WITNESS_CP_UNKNOWN_KEY);

   /* Present but revoked -> rejected even though the signature is valid. */
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor.ed25519_pub, pub, 32);
   anchor.revoked = 1;
   assert(vault_witness_checkpoint_verify(&cp, &anchor, 1) == VAULT_WITNESS_CP_REVOKED_KEY);
}

static void test_rotation_retained_key(void)
{
   /* Two keys; a checkpoint signed by the retired one still verifies while it is
    * in the anchor set, and fails unknown once removed. */
   uint8_t p1[32], k1[32], p2[32], k2[32];
   gen_keypair(p1, k1);
   gen_keypair(p2, k2);
   vault_witness_checkpoint_t cp = fixture();
   assert(vault_witness_checkpoint_sign_ed25519(&cp, p1) == 0);

   vault_witness_anchor_t anchors[2];
   memset(anchors, 0, sizeof anchors);
   memcpy(anchors[0].key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchors[0].ed25519_pub, k1, 32);
   memset(anchors[1].key_id, 0x11, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchors[1].ed25519_pub, k2, 32);
   assert(vault_witness_checkpoint_verify(&cp, anchors, 2) == VAULT_WITNESS_CP_OK);
   /* Only the current key present -> the retired-signed checkpoint is unknown. */
   assert(vault_witness_checkpoint_verify(&cp, &anchors[1], 1) == VAULT_WITNESS_CP_UNKNOWN_KEY);
}

static void test_continuity(void)
{
   vault_witness_checkpoint_t cp = fixture();
   uint8_t expected[32];
   memcpy(expected, cp.predecessor_digest, 32);
   assert(vault_witness_checkpoint_continuity(&cp, expected) == VAULT_WITNESS_CONTINUITY_OK);

   /* No expected predecessor (a gap) -> unproven, never a clean pass. */
   assert(vault_witness_checkpoint_continuity(&cp, NULL) == VAULT_WITNESS_CONTINUITY_UNPROVEN);

   /* Mismatch -> broken. */
   expected[0] ^= 0xFF;
   assert(vault_witness_checkpoint_continuity(&cp, expected) == VAULT_WITNESS_CONTINUITY_BROKEN);
}

static void test_digest_stability(void)
{
   vault_witness_checkpoint_t cp = fixture();
   uint8_t d1[32], d2[32];
   assert(vault_witness_checkpoint_digest(&cp, d1) == 0);
   assert(vault_witness_checkpoint_digest(&cp, d2) == 0);
   assert(memcmp(d1, d2, 32) == 0);

   /* The signature is not part of the digest (digest is over the signable body). */
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);
   uint8_t d3[32];
   assert(vault_witness_checkpoint_digest(&cp, d3) == 0);
   assert(memcmp(d1, d3, 32) == 0);

   /* First checkpoint: has_predecessor 0 requires an all-zero predecessor. */
   vault_witness_checkpoint_t first = fixture();
   first.has_predecessor = 0;
   uint8_t d[32];
   assert(vault_witness_checkpoint_digest(&first, d) == -1); /* nonzero predecessor rejected */
   memset(first.predecessor_digest, 0, 32);
   assert(vault_witness_checkpoint_digest(&first, d) == 0);
}

static void test_encode_decode(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cp = fixture();
   assert(vault_witness_checkpoint_sign_ed25519(&cp, priv) == 0);

   uint8_t wire[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
   size_t len = 0;
   assert(vault_witness_checkpoint_encode(&cp, wire, sizeof wire, &len) == 0);

   vault_witness_checkpoint_t back;
   assert(vault_witness_checkpoint_decode(wire, len, &back) == 0);

   /* The decoded checkpoint verifies against the same anchor. */
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor.ed25519_pub, pub, 32);
   assert(vault_witness_checkpoint_verify(&back, &anchor, 1) == VAULT_WITNESS_CP_OK);

   /* Digests agree, confirming full-field round-trip. */
   uint8_t d1[32], d2[32];
   assert(vault_witness_checkpoint_digest(&cp, d1) == 0);
   assert(vault_witness_checkpoint_digest(&back, d2) == 0);
   assert(memcmp(d1, d2, 32) == 0);

   /* Decoder rejections: truncated, trailing byte, corrupt label. */
   vault_witness_checkpoint_t junk;
   assert(vault_witness_checkpoint_decode(wire, len - 1, &junk) == -1);
   uint8_t bad[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
   memcpy(bad, wire, len);
   assert(vault_witness_checkpoint_decode(bad, len + 1, &junk) == -1);
   memcpy(bad, wire, len);
   bad[4] ^= 0xFF; /* first label byte */
   assert(vault_witness_checkpoint_decode(bad, len, &junk) == -1);
   /* A flipped signature byte still decodes structurally but fails verify. */
   memcpy(bad, wire, len);
   bad[len - 1] ^= 0xFF;
   assert(vault_witness_checkpoint_decode(bad, len, &back) == 0);
   assert(vault_witness_checkpoint_verify(&back, &anchor, 1) == VAULT_WITNESS_CP_BAD_SIG);
}

int main(void)
{
   test_sign_verify();
   test_encode_decode();
   test_unknown_and_revoked_key();
   test_rotation_retained_key();
   test_continuity();
   test_digest_stability();
   printf("test_vault_witness_checkpoint: all passed\n");
   return 0;
}
