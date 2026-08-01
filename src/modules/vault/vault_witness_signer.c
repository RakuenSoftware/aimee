/* Pure KEK-parameterized signer cores. No dependency on vault_server_key/config,
 * so they are directly unit-testable with a fixed KEK. The production wrappers
 * that fetch the server KEK live in vault_witness_signer_kek.c. */
#include "vault_witness_signer.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/sha.h>
#include <string.h>
#include <sys/mman.h>

/* HKDF-SHA256(ikm=kek, salt="", info=SEED_INFO) -> 32-byte Ed25519 seed. */
static int derive_seed(const uint8_t kek[32], uint8_t seed[32])
{
   int rc = -1;
   EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
   size_t outlen = 32;
   if (c && EVP_PKEY_derive_init(c) == 1 &&
       EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1 &&
       EVP_PKEY_CTX_set1_hkdf_key(c, kek, 32) == 1 &&
       EVP_PKEY_CTX_add1_hkdf_info(c, (const unsigned char *)VAULT_WITNESS_SIGNER_SEED_INFO,
                                   (int)strlen(VAULT_WITNESS_SIGNER_SEED_INFO)) == 1 &&
       EVP_PKEY_derive(c, seed, &outlen) == 1 && outlen == 32)
      rc = 0;
   EVP_PKEY_CTX_free(c);
   if (rc != 0)
      OPENSSL_cleanse(seed, 32);
   return rc;
}

static void key_id_from_pub(const uint8_t pub[32], uint8_t key_id[16])
{
   uint8_t buf[sizeof(VAULT_WITNESS_SIGNER_ID_LABEL) + 32];
   size_t label_len = strlen(VAULT_WITNESS_SIGNER_ID_LABEL);
   uint8_t full[32];
   memcpy(buf, VAULT_WITNESS_SIGNER_ID_LABEL, label_len);
   memcpy(buf + label_len, pub, 32);
   SHA256(buf, label_len + 32, full);
   memcpy(key_id, full, 16);
}

int vault_witness_signer_identity_from_kek(const uint8_t kek[32], uint8_t pub[32],
                                           uint8_t key_id[16])
{
   if (!kek || !pub || !key_id)
      return -1;
   OPENSSL_cleanse(pub, 32);
   OPENSSL_cleanse(key_id, 16);
   uint8_t seed[32];
   if (derive_seed(kek, seed) != 0)
      return -1;
   (void)mlock(seed, sizeof seed);
   int rc = -1;
   EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
   if (pkey)
   {
      size_t publen = 32;
      if (EVP_PKEY_get_raw_public_key(pkey, pub, &publen) == 1 && publen == 32)
      {
         key_id_from_pub(pub, key_id);
         rc = 0;
      }
      EVP_PKEY_free(pkey);
   }
   OPENSSL_cleanse(seed, sizeof seed);
   (void)munlock(seed, sizeof seed);
   if (rc != 0)
   {
      OPENSSL_cleanse(pub, 32);
      OPENSSL_cleanse(key_id, 16);
   }
   return rc;
}

int vault_witness_signer_sign_from_kek(const uint8_t kek[32], const uint8_t *msg, size_t msg_len,
                                       uint8_t sig[64])
{
   if (!kek || (!msg && msg_len) || !sig)
      return -1;
   OPENSSL_cleanse(sig, 64);
   uint8_t seed[32];
   if (derive_seed(kek, seed) != 0)
      return -1;
   /* Best-effort pin: mlock can fail under a low RLIMIT_MEMLOCK (common in
    * containers). Failing the signature there would stop checkpoint production
    * entirely — a worse outcome than a pageable seed that is still cleansed
    * immediately below. The cleanse, not the pin, is the load-bearing control. */
   (void)mlock(seed, sizeof seed);
   int rc = -1;
   EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (pkey && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1)
   {
      size_t siglen = 64;
      if (EVP_DigestSign(ctx, sig, &siglen, msg, msg_len) == 1 && siglen == 64)
         rc = 0;
   }
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   OPENSSL_cleanse(seed, sizeof seed);
   (void)munlock(seed, sizeof seed);
   if (rc != 0)
      OPENSSL_cleanse(sig, 64);
   return rc;
}

