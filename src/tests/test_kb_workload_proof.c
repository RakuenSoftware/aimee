#include "kb/kb_workload_proof.h"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EVP_PKEY *generate_key(const char *group)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_group_name(ctx, group) == 1 &&
          EVP_PKEY_generate(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static size_t spki(EVP_PKEY *key, unsigned char out[KB_WORKLOAD_PROOF_SPKI_MAX])
{
   int len = i2d_PUBKEY(key, NULL);
   assert(len > 0 && (size_t)len <= KB_WORKLOAD_PROOF_SPKI_MAX);
   unsigned char *cursor = out;
   assert(i2d_PUBKEY(key, &cursor) == len);
   return (size_t)len;
}

static size_t sign_low_s(EVP_PKEY *key, const unsigned char *message, size_t message_len,
                         unsigned char out[KB_WORKLOAD_WIRE_PROOF_MAX])
{
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   size_t len = KB_WORKLOAD_WIRE_PROOF_MAX;
   assert(ctx && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
          EVP_DigestSign(ctx, out, &len, message, message_len) == 1);
   EVP_MD_CTX_free(ctx);

   const unsigned char *cursor = out;
   ECDSA_SIG *signature = d2i_ECDSA_SIG(NULL, &cursor, (long)len);
   BIGNUM *order = NULL;
   assert(signature && cursor == out + len &&
          EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_ORDER, &order) == 1);
   const BIGNUM *r0 = NULL, *s0 = NULL;
   ECDSA_SIG_get0(signature, &r0, &s0);
   BIGNUM *r = BN_dup(r0), *s = BN_dup(s0), *half = BN_dup(order);
   assert(r && s && half && BN_rshift1(half, half) == 1);
   if (BN_cmp(s, half) > 0)
      assert(BN_sub(s, order, s) == 1);
   assert(ECDSA_SIG_set0(signature, r, s) == 1);
   unsigned char *write = out;
   int encoded = i2d_ECDSA_SIG(signature, &write);
   assert(encoded > 0 && encoded <= (int)KB_WORKLOAD_WIRE_PROOF_MAX);
   BN_free(half);
   BN_free(order);
   ECDSA_SIG_free(signature);
   return (size_t)encoded;
}

static size_t high_s_twin(EVP_PKEY *key, const unsigned char *low, size_t low_len,
                          unsigned char out[KB_WORKLOAD_WIRE_PROOF_MAX])
{
   const unsigned char *cursor = low;
   ECDSA_SIG *signature = d2i_ECDSA_SIG(NULL, &cursor, (long)low_len);
   BIGNUM *order = NULL;
   assert(signature && cursor == low + low_len &&
          EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_ORDER, &order) == 1);
   const BIGNUM *r0 = NULL, *s0 = NULL;
   ECDSA_SIG_get0(signature, &r0, &s0);
   BIGNUM *r = BN_dup(r0), *s = BN_new();
   assert(r && s && BN_sub(s, order, s0) == 1 && ECDSA_SIG_set0(signature, r, s) == 1);
   unsigned char *write = out;
   int encoded = i2d_ECDSA_SIG(signature, &write);
   assert(encoded > 0 && encoded <= (int)KB_WORKLOAD_WIRE_PROOF_MAX);
   BN_free(order);
   ECDSA_SIG_free(signature);
   return (size_t)encoded;
}

static void test_key_load(void)
{
   EVP_PKEY *p256 = generate_key("prime256v1");
   unsigned char der[KB_WORKLOAD_PROOF_SPKI_MAX + 1];
   size_t len = spki(p256, der);
   kb_workload_proof_key_t *loaded = NULL;
   assert(kb_workload_proof_key_load_der(der, len, &loaded) == 0 && loaded);
   unsigned char anchor[32], expected[32];
   unsigned int digest_len = 0;
   assert(kb_workload_proof_anchor_id(loaded, anchor) == 0 &&
          EVP_Digest(der, len, expected, &digest_len, EVP_sha256(), NULL) == 1 &&
          digest_len == 32 && memcmp(anchor, expected, 32) == 0);
   kb_workload_proof_key_close(loaded);

   der[len] = 0;
   loaded = (void *)1;
   assert(kb_workload_proof_key_load_der(der, len + 1, &loaded) == -1 && !loaded);
   assert(kb_workload_proof_key_load_der(der, len - 1, &loaded) == -1 && !loaded);

   EVP_PKEY *p384 = generate_key("secp384r1");
   len = spki(p384, der);
   assert(kb_workload_proof_key_load_der(der, len, &loaded) == -1 && !loaded);
   EVP_PKEY_free(p384);
   EVP_PKEY_free(p256);
}

static void test_transcript_and_verify(void)
{
   EVP_PKEY *private_key = generate_key("prime256v1");
   unsigned char der[KB_WORKLOAD_PROOF_SPKI_MAX];
   size_t der_len = spki(private_key, der);
   kb_workload_proof_key_t *key = NULL;
   assert(kb_workload_proof_key_load_der(der, der_len, &key) == 0);

   unsigned char challenge[32], binding[32], proof_anchor[32], custody_anchor[32];
   unsigned char request[41], response[73], transcript[KB_WORKLOAD_PROOF_TRANSCRIPT_LEN];
   memset(challenge, 0x11, sizeof(challenge));
   memset(binding, 0x22, sizeof(binding));
   memset(custody_anchor, 0x33, sizeof(custody_anchor));
   memset(request, 0x44, sizeof(request));
   memset(response, 0x55, sizeof(response));
   assert(kb_workload_proof_anchor_id(key, proof_anchor) == 0);
   const unsigned char token[] = "header.payload.signature";
   assert(kb_workload_proof_transcript(KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                       sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                       sizeof(request), response, sizeof(response),
                                       transcript) == 0);
   assert(memcmp(transcript, "\0\0\0\32aimee.workload.provider.v1\0\0\0\1\2", 35) == 0);

   unsigned char signature[KB_WORKLOAD_WIRE_PROOF_MAX];
   size_t signature_len = sign_low_s(private_key, transcript, sizeof(transcript), signature);
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), signature,
                                   signature_len) == 0);

   unsigned char changed[32];
   memcpy(changed, binding, sizeof(changed));
   changed[0] ^= 1;
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, changed, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), signature,
                                   signature_len) == -1);
   memcpy(changed, proof_anchor, sizeof(changed));
   changed[0] ^= 1;
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, changed, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), signature,
                                   signature_len) == -1);
   response[0] ^= 1;
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), signature,
                                   signature_len) == -1);
   response[0] ^= 1;

   unsigned char high[KB_WORKLOAD_WIRE_PROOF_MAX];
   size_t high_len = high_s_twin(private_key, signature, signature_len, high);
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), high,
                                   high_len) == -1);
   unsigned char extra[KB_WORKLOAD_WIRE_PROOF_MAX];
   memcpy(extra, signature, signature_len);
   extra[signature_len] = 0;
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), extra,
                                   signature_len + 1) == -1);
   assert(kb_workload_proof_verify(key, KB_WORKLOAD_OP_WRAP, challenge, binding, token,
                                   sizeof(token) - 1, proof_anchor, custody_anchor, request,
                                   sizeof(request), response, sizeof(response), signature,
                                   signature_len - 1) == -1);

   unsigned char attest[KB_WORKLOAD_PROOF_TRANSCRIPT_LEN];
   assert(kb_workload_proof_transcript(KB_WORKLOAD_OP_ATTEST, challenge, binding, token,
                                       sizeof(token) - 1, proof_anchor, custody_anchor, NULL, 0,
                                       NULL, 0, attest) == 0);
   assert(kb_workload_proof_transcript(KB_WORKLOAD_OP_ATTEST, challenge, binding, token,
                                       sizeof(token) - 1, proof_anchor, custody_anchor, request, 1,
                                       NULL, 0, attest) == -1);
   assert(kb_workload_proof_transcript(KB_WORKLOAD_OP_UNWRAP, challenge, binding, token,
                                       sizeof(token) - 1, proof_anchor, custody_anchor, request, 0,
                                       response, sizeof(response), attest) == -1);

   kb_workload_proof_key_close(key);
   EVP_PKEY_free(private_key);
}

int main(void)
{
   test_key_load();
   test_transcript_and_verify();
   puts("kb_workload_proof: ok");
   return 0;
}
