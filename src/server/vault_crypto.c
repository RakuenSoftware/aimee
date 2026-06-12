/* vault_crypto.c: envelope-encryption primitives for the credential vault
 * (WP-C.1). OpenSSL-backed (HKDF-SHA256 / AES-256-GCM / AES-KW), fail-closed,
 * cleanse-on-failure. See vault_crypto.h for the scheme. These are deliberately
 * free of file/global/log state so they can be tested in isolation. */
#include "vault_crypto.h"
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <string.h>

int vault_crypto_random(uint8_t *out, size_t n)
{
   if (!out)
      return -1;
   /* RAND_bytes returns 1 on success; anything else means the RNG could not
    * provide strong randomness — fail closed rather than emit a weak key. */
   return RAND_bytes(out, (int)n) == 1 ? 0 : -1;
}

int vault_kek_derive(const uint8_t *root_key, size_t root_key_len, const uint8_t *salt,
                     size_t salt_len, uint8_t kek[VAULT_KEK_LEN])
{
   if (!kek)
      return -1;
   memset(kek, 0, VAULT_KEK_LEN);
   if (!root_key || root_key_len != VAULT_ROOT_KEY_LEN || (!salt && salt_len) || salt_len == 0)
      return -1;

   EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
   if (!pctx)
      return -1;

   int rc = -1;
   size_t outlen = VAULT_KEK_LEN;
   if (EVP_PKEY_derive_init(pctx) == 1 && EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) == 1 &&
       EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, (int)salt_len) == 1 &&
       EVP_PKEY_CTX_set1_hkdf_key(pctx, root_key, (int)root_key_len) == 1 &&
       EVP_PKEY_CTX_add1_hkdf_info(pctx, (const unsigned char *)VAULT_KEK_INFO,
                                   (int)strlen(VAULT_KEK_INFO)) == 1 &&
       EVP_PKEY_derive(pctx, kek, &outlen) == 1 && outlen == VAULT_KEK_LEN)
      rc = 0;

   EVP_PKEY_CTX_free(pctx);
   if (rc != 0)
      OPENSSL_cleanse(kek, VAULT_KEK_LEN);
   return rc;
}

int vault_dek_wrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   if (!kek || !dek || !wrapped)
      return -1;
   memset(wrapped, 0, VAULT_WRAPPED_DEK_LEN);

   EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
   if (!ctx)
      return -1;
   /* The wrap ciphers are refused by default; opt in explicitly. NULL IV uses
    * the RFC 3394 default ICV (0xA6...), which doubles as the integrity check on
    * unwrap. */
   EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

   int rc = -1;
   int outl = 0, finl = 0;
   if (EVP_EncryptInit_ex(ctx, EVP_aes_256_wrap(), NULL, kek, NULL) == 1 &&
       EVP_EncryptUpdate(ctx, wrapped, &outl, dek, VAULT_DEK_LEN) == 1 &&
       EVP_EncryptFinal_ex(ctx, wrapped + outl, &finl) == 1 && outl + finl == VAULT_WRAPPED_DEK_LEN)
      rc = 0;

   EVP_CIPHER_CTX_free(ctx);
   if (rc != 0)
      OPENSSL_cleanse(wrapped, VAULT_WRAPPED_DEK_LEN);
   return rc;
}

int vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN],
                     uint8_t dek[VAULT_DEK_LEN])
{
   if (!kek || !wrapped || !dek)
      return -1;
   memset(dek, 0, VAULT_DEK_LEN);

   EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
   if (!ctx)
      return -1;
   EVP_CIPHER_CTX_set_flags(ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);

   int rc = -1;
   int outl = 0, finl = 0;
   /* A wrong KEK or tampered wrapper fails the RFC 3394 ICV check -> a non-1
    * return from one of these calls -> fail closed. */
   if (EVP_DecryptInit_ex(ctx, EVP_aes_256_wrap(), NULL, kek, NULL) == 1 &&
       EVP_DecryptUpdate(ctx, dek, &outl, wrapped, VAULT_WRAPPED_DEK_LEN) == 1 &&
       EVP_DecryptFinal_ex(ctx, dek + outl, &finl) == 1 && outl + finl == VAULT_DEK_LEN)
      rc = 0;

   EVP_CIPHER_CTX_free(ctx);
   if (rc != 0)
      OPENSSL_cleanse(dek, VAULT_DEK_LEN);
   return rc;
}

int vault_secret_encrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t pt_len,
                         uint8_t nonce[VAULT_GCM_NONCE_LEN], uint8_t *ciphertext,
                         uint8_t tag[VAULT_GCM_TAG_LEN])
{
   if (!dek || !nonce || !tag || (pt_len && (!plaintext || !ciphertext)) || (aad_len && !aad))
      return -1;

   /* Fresh nonce per call (fail-closed): the caller never chooses it, so a
    * (DEK, nonce) pair is never reused. */
   if (vault_crypto_random(nonce, VAULT_GCM_NONCE_LEN) != 0)
      return -1;

   EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
   if (!ctx)
      return -1;

   int rc = -1;
   int outl = 0, finl = 0, aoutl = 0;
   if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, VAULT_GCM_NONCE_LEN, NULL) == 1 &&
       EVP_EncryptInit_ex(ctx, NULL, NULL, dek, nonce) == 1 &&
       (aad_len == 0 || EVP_EncryptUpdate(ctx, NULL, &aoutl, aad, (int)aad_len) == 1) &&
       (pt_len == 0 || EVP_EncryptUpdate(ctx, ciphertext, &outl, plaintext, (int)pt_len) == 1) &&
       EVP_EncryptFinal_ex(ctx, ciphertext + outl, &finl) == 1 &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, VAULT_GCM_TAG_LEN, tag) == 1 &&
       (size_t)(outl + finl) == pt_len)
      rc = 0;

   EVP_CIPHER_CTX_free(ctx);
   if (rc != 0)
   {
      if (ciphertext && pt_len)
         OPENSSL_cleanse(ciphertext, pt_len);
      OPENSSL_cleanse(nonce, VAULT_GCM_NONCE_LEN);
      OPENSSL_cleanse(tag, VAULT_GCM_TAG_LEN);
   }
   return rc;
}

int vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                         size_t ct_len, const uint8_t tag[VAULT_GCM_TAG_LEN], uint8_t *plaintext)
{
   if (!dek || !nonce || !tag || (ct_len && (!ciphertext || !plaintext)) || (aad_len && !aad))
      return -1;

   EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
   if (!ctx)
      return -1;

   int rc = -1;
   int outl = 0, finl = 0, aoutl = 0;
   /* Decrypt then verify: EVP_DecryptFinal_ex returns 1 ONLY if the GCM tag
    * authenticates the ciphertext + AAD. A tampered ciphertext, a wrong DEK, or
    * an AAD mismatch (e.g. a row swapped to another principal/agent/cred) all
    * fail here -> plaintext is cleansed and -1 returned. */
   if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, VAULT_GCM_NONCE_LEN, NULL) == 1 &&
       EVP_DecryptInit_ex(ctx, NULL, NULL, dek, nonce) == 1 &&
       (aad_len == 0 || EVP_DecryptUpdate(ctx, NULL, &aoutl, aad, (int)aad_len) == 1) &&
       (ct_len == 0 || EVP_DecryptUpdate(ctx, plaintext, &outl, ciphertext, (int)ct_len) == 1) &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, VAULT_GCM_TAG_LEN, (void *)tag) == 1 &&
       EVP_DecryptFinal_ex(ctx, plaintext + outl, &finl) == 1 && (size_t)(outl + finl) == ct_len)
      rc = 0;

   EVP_CIPHER_CTX_free(ctx);
   if (rc != 0 && plaintext && ct_len)
      OPENSSL_cleanse(plaintext, ct_len);
   return rc;
}
