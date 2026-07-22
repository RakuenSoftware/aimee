/* kb_workload_proof.c: canonical SPKI, transcript, and low-S ECDSA verifier. */
#include "kb_workload_proof.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct kb_workload_proof_key
{
   EVP_PKEY *key;
   BIGNUM *order;
   BIGNUM *half_order;
   unsigned char anchor_id[KB_WORKLOAD_ANCHOR_LEN];
};

static int sha256(const void *data, size_t len, unsigned char out[32])
{
   static const unsigned char empty = 0;
   unsigned int out_len = 0;
   return EVP_Digest(len ? data : &empty, len, out, &out_len, EVP_sha256(), NULL) == 1 &&
                  out_len == 32
              ? 0
              : -1;
}

int kb_workload_proof_key_load_der(const unsigned char *der, size_t der_len,
                                   kb_workload_proof_key_t **out)
{
   if (out)
      *out = NULL;
   if (!out || !der || der_len == 0 || der_len > KB_WORKLOAD_PROOF_SPKI_MAX)
      return -1;

   const unsigned char *cursor = der;
   EVP_PKEY *key = d2i_PUBKEY(NULL, &cursor, (long)der_len);
   unsigned char *canonical = NULL;
   EVP_PKEY_CTX *check = NULL;
   BIGNUM *order = NULL, *half = NULL;
   kb_workload_proof_key_t *loaded = NULL;
   int ok = 0;

   if (!key || cursor != der + der_len || !EVP_PKEY_is_a(key, "EC"))
      goto done;
   int canonical_len = i2d_PUBKEY(key, NULL);
   if (canonical_len <= 0 || (size_t)canonical_len != der_len ||
       !(canonical = OPENSSL_malloc((size_t)canonical_len)))
      goto done;
   unsigned char *write = canonical;
   if (i2d_PUBKEY(key, &write) != canonical_len || CRYPTO_memcmp(canonical, der, der_len) != 0)
      goto done;

   char group[80] = "";
   size_t group_len = 0;
   if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group),
                                      &group_len) != 1 ||
       group_len == 0 || OBJ_txt2nid(group) != NID_X9_62_prime256v1 ||
       !(check = EVP_PKEY_CTX_new(key, NULL)) || EVP_PKEY_public_check(check) != 1 ||
       EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_ORDER, &order) != 1 || !order ||
       BN_is_negative(order) || BN_is_zero(order) || !(half = BN_dup(order)) ||
       BN_rshift1(half, half) != 1 || !(loaded = calloc(1, sizeof(*loaded))))
      goto done;

   loaded->key = key;
   key = NULL;
   loaded->order = order;
   order = NULL;
   loaded->half_order = half;
   half = NULL;
   if (sha256(der, der_len, loaded->anchor_id) != 0)
      goto done;
   *out = loaded;
   loaded = NULL;
   ok = 1;

done:
   if (loaded)
      kb_workload_proof_key_close(loaded);
   BN_free(half);
   BN_free(order);
   EVP_PKEY_CTX_free(check);
   OPENSSL_free(canonical);
   EVP_PKEY_free(key);
   return ok ? 0 : -1;
}

void kb_workload_proof_key_close(kb_workload_proof_key_t *key)
{
   if (!key)
      return;
   EVP_PKEY_free(key->key);
   BN_free(key->order);
   BN_free(key->half_order);
   OPENSSL_cleanse(key, sizeof(*key));
   free(key);
}

int kb_workload_proof_anchor_id(const kb_workload_proof_key_t *key,
                                unsigned char out[KB_WORKLOAD_ANCHOR_LEN])
{
   if (!out)
      return -1;
   memset(out, 0, KB_WORKLOAD_ANCHOR_LEN);
   if (!key)
      return -1;
   memcpy(out, key->anchor_id, KB_WORKLOAD_ANCHOR_LEN);
   return 0;
}

static void put_u32be(unsigned char out[4], uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static void put_field(unsigned char *out, size_t *offset, const void *data, size_t len)
{
   put_u32be(out + *offset, (uint32_t)len);
   *offset += 4;
   memcpy(out + *offset, data, len);
   *offset += len;
}

static int valid_data(kb_workload_operation_t operation, const void *request_data,
                      size_t request_len, const void *response_data, size_t response_len)
{
   if (operation == KB_WORKLOAD_OP_ATTEST)
      return request_len == 0 && response_len == 0;
   if (!request_data || !response_data || request_len == 0 || response_len == 0)
      return 0;
   if (operation == KB_WORKLOAD_OP_WRAP)
      return request_len <= KB_WORKLOAD_WIRE_PLAIN_MAX &&
             response_len <= KB_WORKLOAD_WIRE_CIPHER_MAX;
   if (operation == KB_WORKLOAD_OP_UNWRAP)
      return request_len <= KB_WORKLOAD_WIRE_CIPHER_MAX &&
             response_len <= KB_WORKLOAD_WIRE_PLAIN_MAX;
   return 0;
}

int kb_workload_proof_transcript(kb_workload_operation_t operation,
                                 const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                 const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                 const unsigned char *token, size_t token_len,
                                 const unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                                 const unsigned char custody_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                                 const void *request_data, size_t request_data_len,
                                 const void *response_data, size_t response_data_len,
                                 unsigned char out[KB_WORKLOAD_PROOF_TRANSCRIPT_LEN])
{
   static const unsigned char domain[] = "aimee.workload.provider.v1";
   if (out)
      memset(out, 0, KB_WORKLOAD_PROOF_TRANSCRIPT_LEN);
   if (!out || !challenge || !binding || !token || token_len == 0 ||
       token_len > KB_WORKLOAD_WIRE_TOKEN_MAX || !proof_anchor_id || !custody_anchor_id ||
       !valid_data(operation, request_data, request_data_len, response_data, response_data_len))
      return -1;

   unsigned char token_hash[32], request_hash[32], response_hash[32];
   if (sha256(token, token_len, token_hash) != 0 ||
       sha256(request_data, request_data_len, request_hash) != 0 ||
       sha256(response_data, response_data_len, response_hash) != 0)
      goto fail;

   unsigned char op = (unsigned char)operation;
   size_t offset = 0;
   put_field(out, &offset, domain, sizeof(domain) - 1);
   put_field(out, &offset, &op, 1);
   put_field(out, &offset, challenge, KB_WORKLOAD_CHALLENGE_LEN);
   put_field(out, &offset, binding, KB_WORKLOAD_BINDING_LEN);
   put_field(out, &offset, token_hash, sizeof(token_hash));
   put_field(out, &offset, proof_anchor_id, KB_WORKLOAD_ANCHOR_LEN);
   put_field(out, &offset, custody_anchor_id, KB_WORKLOAD_ANCHOR_LEN);
   put_field(out, &offset, request_hash, sizeof(request_hash));
   put_field(out, &offset, response_hash, sizeof(response_hash));
   OPENSSL_cleanse(token_hash, sizeof(token_hash));
   OPENSSL_cleanse(request_hash, sizeof(request_hash));
   OPENSSL_cleanse(response_hash, sizeof(response_hash));
   if (offset != KB_WORKLOAD_PROOF_TRANSCRIPT_LEN)
      goto fail_output;
   return 0;

fail:
   OPENSSL_cleanse(token_hash, sizeof(token_hash));
   OPENSSL_cleanse(request_hash, sizeof(request_hash));
   OPENSSL_cleanse(response_hash, sizeof(response_hash));
fail_output:
   OPENSSL_cleanse(out, KB_WORKLOAD_PROOF_TRANSCRIPT_LEN);
   return -1;
}

static int signature_is_canonical_low_s(const kb_workload_proof_key_t *key,
                                        const unsigned char *der, size_t der_len,
                                        ECDSA_SIG **parsed_out)
{
   *parsed_out = NULL;
   if (!key || !der || der_len < KB_WORKLOAD_WIRE_PROOF_MIN || der_len > KB_WORKLOAD_WIRE_PROOF_MAX)
      return 0;
   const unsigned char *cursor = der;
   ECDSA_SIG *signature = d2i_ECDSA_SIG(NULL, &cursor, (long)der_len);
   if (!signature || cursor != der + der_len)
   {
      ECDSA_SIG_free(signature);
      return 0;
   }
   unsigned char canonical[KB_WORKLOAD_WIRE_PROOF_MAX];
   unsigned char *write = canonical;
   int canonical_len = i2d_ECDSA_SIG(signature, NULL);
   if (canonical_len <= 0 || (size_t)canonical_len != der_len ||
       (size_t)canonical_len > sizeof(canonical) ||
       i2d_ECDSA_SIG(signature, &write) != canonical_len ||
       CRYPTO_memcmp(canonical, der, der_len) != 0)
   {
      ECDSA_SIG_free(signature);
      return 0;
   }
   const BIGNUM *r = NULL, *s = NULL;
   ECDSA_SIG_get0(signature, &r, &s);
   if (!r || !s || BN_is_negative(r) || BN_is_zero(r) || BN_cmp(r, key->order) >= 0 ||
       BN_is_negative(s) || BN_is_zero(s) || BN_cmp(s, key->order) >= 0 ||
       BN_cmp(s, key->half_order) > 0)
   {
      ECDSA_SIG_free(signature);
      return 0;
   }
   *parsed_out = signature;
   return 1;
}

int kb_workload_proof_verify(const kb_workload_proof_key_t *key, kb_workload_operation_t operation,
                             const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                             const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                             const unsigned char *token, size_t token_len,
                             const unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                             const unsigned char custody_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                             const void *request_data, size_t request_data_len,
                             const void *response_data, size_t response_data_len,
                             const unsigned char *signature_der, size_t signature_len)
{
   if (!key || !proof_anchor_id ||
       CRYPTO_memcmp(key->anchor_id, proof_anchor_id, KB_WORKLOAD_ANCHOR_LEN) != 0)
      return -1;

   ECDSA_SIG *parsed = NULL;
   if (!signature_is_canonical_low_s(key, signature_der, signature_len, &parsed))
      return -1;
   ECDSA_SIG_free(parsed);

   unsigned char transcript[KB_WORKLOAD_PROOF_TRANSCRIPT_LEN];
   if (kb_workload_proof_transcript(
           operation, challenge, binding, token, token_len, proof_anchor_id, custody_anchor_id,
           request_data, request_data_len, response_data, response_data_len, transcript) != 0)
      return -1;
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   int ok =
       ctx && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key->key) == 1 &&
       EVP_DigestVerify(ctx, signature_der, signature_len, transcript, sizeof(transcript)) == 1;
   EVP_MD_CTX_free(ctx);
   OPENSSL_cleanse(transcript, sizeof(transcript));
   return ok ? 0 : -1;
}
