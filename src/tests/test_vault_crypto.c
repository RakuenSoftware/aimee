/* test_vault_crypto.c: WP-C.1 envelope-crypto core. Pins the round-trip and —
 * crucially — every fail-closed property the vault's confidentiality depends on:
 * wrong key, tampered ciphertext/tag/wrapper, AAD substitution, and unique
 * (DEK,nonce) per encryption. */
#include "vault_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_random_distinct_and_sized(void)
{
   uint8_t a[32], b[32];
   assert(vault_crypto_random(a, sizeof(a)) == 0);
   assert(vault_crypto_random(b, sizeof(b)) == 0);
   assert(memcmp(a, b, sizeof(a)) != 0); /* astronomically unlikely to collide */
   printf("  PASS: test_random_distinct_and_sized\n");
}

static void test_kek_derive_deterministic_and_salted(void)
{
   uint8_t root[VAULT_ROOT_KEY_LEN];
   uint8_t salt1[VAULT_SALT_LEN], salt2[VAULT_SALT_LEN];
   memset(root, 0x11, sizeof(root));
   memset(salt1, 0x22, sizeof(salt1));
   memset(salt2, 0x23, sizeof(salt2));

   uint8_t k1[VAULT_KEK_LEN], k1b[VAULT_KEK_LEN], k2[VAULT_KEK_LEN];
   assert(vault_kek_derive(root, sizeof(root), salt1, sizeof(salt1), k1) == 0);
   assert(vault_kek_derive(root, sizeof(root), salt1, sizeof(salt1), k1b) == 0);
   assert(memcmp(k1, k1b, VAULT_KEK_LEN) == 0); /* deterministic */
   assert(vault_kek_derive(root, sizeof(root), salt2, sizeof(salt2), k2) == 0);
   assert(memcmp(k1, k2, VAULT_KEK_LEN) != 0); /* salt changes the KEK */

   /* A different root key yields a different KEK. */
   uint8_t root2[VAULT_ROOT_KEY_LEN];
   memset(root2, 0x99, sizeof(root2));
   uint8_t k3[VAULT_KEK_LEN];
   assert(vault_kek_derive(root2, sizeof(root2), salt1, sizeof(salt1), k3) == 0);
   assert(memcmp(k1, k3, VAULT_KEK_LEN) != 0);

   /* Wrong root-key length is rejected (fail-closed). */
   assert(vault_kek_derive(root, 16, salt1, sizeof(salt1), k1) == -1);
   assert(vault_kek_derive(root, sizeof(root), salt1, 0, k1) == -1);
   printf("  PASS: test_kek_derive_deterministic_and_salted\n");
}

static void test_dek_wrap_roundtrip_and_tamper(void)
{
   uint8_t kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(kek, sizeof(kek)) == 0);
   assert(vault_crypto_random(dek, sizeof(dek)) == 0);

   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   assert(vault_dek_wrap(kek, dek, wrapped) == 0);
   /* The wrapper must not be the bare DEK. */
   assert(memcmp(wrapped, dek, VAULT_DEK_LEN) != 0);

   uint8_t out[VAULT_DEK_LEN];
   assert(vault_dek_unwrap(kek, wrapped, out) == 0);
   assert(memcmp(out, dek, VAULT_DEK_LEN) == 0);

   /* Wrong KEK fails the RFC 3394 integrity check. */
   uint8_t wrong_kek[VAULT_KEK_LEN];
   assert(vault_crypto_random(wrong_kek, sizeof(wrong_kek)) == 0);
   uint8_t bad[VAULT_DEK_LEN];
   assert(vault_dek_unwrap(wrong_kek, wrapped, bad) == -1);

   /* Tampered wrapper fails. */
   uint8_t tampered[VAULT_WRAPPED_DEK_LEN];
   memcpy(tampered, wrapped, sizeof(tampered));
   tampered[5] ^= 0x40;
   assert(vault_dek_unwrap(kek, tampered, bad) == -1);
   printf("  PASS: test_dek_wrap_roundtrip_and_tamper\n");
}

static void test_secret_gcm_roundtrip(void)
{
   uint8_t dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(dek, sizeof(dek)) == 0);
   const char *secret = "sk-ant-supersecret-credential-value";
   const uint8_t aad[] = "uid:1000|claude|api_key";
   size_t pt_len = strlen(secret);

   uint8_t nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
   uint8_t ct[64];
   assert(pt_len <= sizeof(ct));
   assert(vault_secret_encrypt(dek, aad, sizeof(aad) - 1, (const uint8_t *)secret, pt_len, nonce,
                               ct, tag) == 0);
   /* Ciphertext must differ from plaintext. */
   assert(memcmp(ct, secret, pt_len) != 0);

   uint8_t out[64];
   assert(vault_secret_decrypt(dek, aad, sizeof(aad) - 1, nonce, ct, pt_len, tag, out) == 0);
   assert(memcmp(out, secret, pt_len) == 0);
   printf("  PASS: test_secret_gcm_roundtrip\n");
}

static void test_secret_fresh_nonce_per_encrypt(void)
{
   uint8_t dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(dek, sizeof(dek)) == 0);
   const char *secret = "the-same-plaintext-twice";
   size_t pt_len = strlen(secret);

   uint8_t n1[VAULT_GCM_NONCE_LEN], n2[VAULT_GCM_NONCE_LEN];
   uint8_t t1[VAULT_GCM_TAG_LEN], t2[VAULT_GCM_TAG_LEN];
   uint8_t c1[32], c2[32];
   assert(vault_secret_encrypt(dek, NULL, 0, (const uint8_t *)secret, pt_len, n1, c1, t1) == 0);
   assert(vault_secret_encrypt(dek, NULL, 0, (const uint8_t *)secret, pt_len, n2, c2, t2) == 0);
   /* Same DEK + same plaintext must still differ — distinct nonce => distinct
    * ciphertext + tag (never a reused (DEK,nonce) pair). */
   assert(memcmp(n1, n2, VAULT_GCM_NONCE_LEN) != 0);
   assert(memcmp(c1, c2, pt_len) != 0);
   printf("  PASS: test_secret_fresh_nonce_per_encrypt\n");
}

static void test_secret_tamper_and_wrong_key_fail_closed(void)
{
   uint8_t dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(dek, sizeof(dek)) == 0);
   const char *secret = "credential-to-protect";
   const uint8_t aad[] = "uid:1000|claude|api_key";
   size_t pt_len = strlen(secret);
   uint8_t nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN], ct[32], out[32];
   assert(vault_secret_encrypt(dek, aad, sizeof(aad) - 1, (const uint8_t *)secret, pt_len, nonce,
                               ct, tag) == 0);

   /* Wrong DEK. */
   uint8_t wrong_dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(wrong_dek, sizeof(wrong_dek)) == 0);
   assert(vault_secret_decrypt(wrong_dek, aad, sizeof(aad) - 1, nonce, ct, pt_len, tag, out) == -1);

   /* Tampered ciphertext. */
   uint8_t ct2[32];
   memcpy(ct2, ct, pt_len);
   ct2[3] ^= 0x80;
   assert(vault_secret_decrypt(dek, aad, sizeof(aad) - 1, nonce, ct2, pt_len, tag, out) == -1);

   /* Tampered tag. */
   uint8_t tag2[VAULT_GCM_TAG_LEN];
   memcpy(tag2, tag, sizeof(tag2));
   tag2[0] ^= 0x01;
   assert(vault_secret_decrypt(dek, aad, sizeof(aad) - 1, nonce, ct, pt_len, tag2, out) == -1);

   /* AAD substitution: a ciphertext encrypted for one (principal|agent|cred)
    * must NOT decrypt under a different identity slot — blocks row/file swap. */
   const uint8_t aad_other[] = "uid:1001|claude|api_key";
   assert(vault_secret_decrypt(dek, aad_other, sizeof(aad_other) - 1, nonce, ct, pt_len, tag,
                               out) == -1);
   printf("  PASS: test_secret_tamper_and_wrong_key_fail_closed\n");
}

/* End-to-end envelope: derive KEK, wrap a fresh DEK, GCM-encrypt under it, then
 * reverse the whole chain — the path the vault store will run per credential. */
static void test_full_envelope_roundtrip(void)
{
   uint8_t root[VAULT_ROOT_KEY_LEN], salt[VAULT_SALT_LEN];
   assert(vault_crypto_random(root, sizeof(root)) == 0);
   assert(vault_crypto_random(salt, sizeof(salt)) == 0);

   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_kek_derive(root, sizeof(root), salt, sizeof(salt), kek) == 0);

   uint8_t dek[VAULT_DEK_LEN];
   assert(vault_crypto_random(dek, sizeof(dek)) == 0);
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   assert(vault_dek_wrap(kek, dek, wrapped) == 0);

   const char *secret = "sk-live-0123456789";
   const uint8_t aad[] = "webuser:alice|openai|api_key";
   size_t pt_len = strlen(secret);
   uint8_t nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN], ct[64];
   assert(vault_secret_encrypt(dek, aad, sizeof(aad) - 1, (const uint8_t *)secret, pt_len, nonce,
                               ct, tag) == 0);

   /* Reverse: re-derive KEK, unwrap DEK, decrypt. */
   uint8_t kek2[VAULT_KEK_LEN], dek2[VAULT_DEK_LEN], out[64];
   assert(vault_kek_derive(root, sizeof(root), salt, sizeof(salt), kek2) == 0);
   assert(vault_dek_unwrap(kek2, wrapped, dek2) == 0);
   assert(vault_secret_decrypt(dek2, aad, sizeof(aad) - 1, nonce, ct, pt_len, tag, out) == 0);
   assert(memcmp(out, secret, pt_len) == 0);
   printf("  PASS: test_full_envelope_roundtrip\n");
}

int main(void)
{
   test_random_distinct_and_sized();
   test_kek_derive_deterministic_and_salted();
   test_dek_wrap_roundtrip_and_tamper();
   test_secret_gcm_roundtrip();
   test_secret_fresh_nonce_per_encrypt();
   test_secret_tamper_and_wrong_key_fail_closed();
   test_full_envelope_roundtrip();
   printf("vault_crypto: all tests passed\n");
   return 0;
}
