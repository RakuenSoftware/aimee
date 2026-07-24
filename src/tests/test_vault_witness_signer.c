#include "modules/vault/vault_witness_signer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/vault/vault_witness_checkpoint.h"

/* The signer cores are deterministic in the KEK, so a fixed KEK yields a fixed
 * keypair and (Ed25519 being deterministic) a fixed signature. */
static void fixed_kek(uint8_t kek[32], uint8_t fill)
{
   memset(kek, fill, 32);
}

static void test_determinism(void)
{
   uint8_t kek[32];
   fixed_kek(kek, 0x42);
   uint8_t p1[32], p2[32], id1[16], id2[16];
   assert(vault_witness_signer_identity_from_kek(kek, p1, id1) == 0);
   assert(vault_witness_signer_identity_from_kek(kek, p2, id2) == 0);
   assert(memcmp(p1, p2, 32) == 0 && memcmp(id1, id2, 16) == 0);

   uint8_t msg[19] = "witness-signer-test";
   uint8_t s1[64], s2[64];
   assert(vault_witness_signer_sign_from_kek(kek, msg, sizeof msg, s1) == 0);
   assert(vault_witness_signer_sign_from_kek(kek, msg, sizeof msg, s2) == 0);
   assert(memcmp(s1, s2, 64) == 0);
}

static void test_distinct_kek_distinct_key(void)
{
   uint8_t k1[32], k2[32], p1[32], p2[32], id1[16], id2[16];
   fixed_kek(k1, 0x01);
   fixed_kek(k2, 0x02);
   assert(vault_witness_signer_identity_from_kek(k1, p1, id1) == 0);
   assert(vault_witness_signer_identity_from_kek(k2, p2, id2) == 0);
   assert(memcmp(p1, p2, 32) != 0 && memcmp(id1, id2, 16) != 0);
}

/* End-to-end: derive the identity, sign a checkpoint's signable body, and confirm
 * E1's checkpoint verify accepts it against an anchor built from the derived key. */
static void test_sign_verify_checkpoint(void)
{
   uint8_t kek[32];
   fixed_kek(kek, 0x7e);
   uint8_t pub[32], key_id[16];
   assert(vault_witness_signer_identity_from_kek(kek, pub, key_id) == 0);

   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = 5;
   cp.has_predecessor = 1;
   cp.shard_count = 3;
   cp.sig_alg = VAULT_WITNESS_SIG_ED25519;
   cp.sig_version = 1;
   memset(cp.root, 0x33, 32);
   memset(cp.predecessor_digest, 0x44, 32);
   memset(cp.leaf_snapshot_digest, 0x55, 32);
   memcpy(cp.signer_key_id, key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   snprintf(cp.created_at, sizeof cp.created_at, "2026-07-23T12:00:00Z");

   uint8_t body[512];
   size_t len = 0;
   assert(vault_witness_checkpoint_signable(&cp, body, sizeof body, &len) == 0);
   assert(vault_witness_signer_sign_from_kek(kek, body, len, cp.signature) == 0);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor.ed25519_pub, pub, 32);
   assert(vault_witness_checkpoint_verify(&cp, &anchor, 1) == VAULT_WITNESS_CP_OK);

   /* A tampered root breaks the signature. */
   vault_witness_checkpoint_t t = cp;
   t.root[0] ^= 0xFF;
   assert(vault_witness_checkpoint_verify(&t, &anchor, 1) == VAULT_WITNESS_CP_BAD_SIG);

   /* The key derived from a different KEK does not verify this checkpoint. */
   uint8_t kek2[32], pub2[32], id2[16];
   fixed_kek(kek2, 0x11);
   assert(vault_witness_signer_identity_from_kek(kek2, pub2, id2) == 0);
   vault_witness_anchor_t anchor2;
   memset(&anchor2, 0, sizeof anchor2);
   memcpy(anchor2.key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor2.ed25519_pub, pub2, 32);
   assert(vault_witness_checkpoint_verify(&cp, &anchor2, 1) == VAULT_WITNESS_CP_BAD_SIG);
}

int main(void)
{
   test_determinism();
   test_distinct_kek_distinct_key();
   test_sign_verify_checkpoint();
   printf("test_vault_witness_signer: all passed\n");
   return 0;
}
