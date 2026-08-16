#include "kb_mgmt_jwks_publication.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_INPUT_MAX 65536u

static const uint8_t seed[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static void sha256(const void *data, size_t len, uint8_t out[32])
{
   unsigned int out_len = 0;
   assert(EVP_Digest(data, len, out, &out_len, EVP_sha256(), NULL) == 1 && out_len == 32);
}

static void valid_fixture(kb_mgmt_jwks_roots_t *roots, kb_mgmt_jwks_record_t *record)
{
   memset(roots, 0, sizeof(*roots));
   for (size_t i = 0; i < KB_MGMT_TOKEN_MODULUS_LEN; ++i)
      roots->token.public_key[i] = (uint8_t)(0x80u + i * 17u);
   roots->token.kind = KB_MGMT_ROOT_TOKEN;
   roots->token.phase = KB_MGMT_ROOT_FINAL;
   roots->token.public_key_len = KB_MGMT_TOKEN_MODULUS_LEN;
   sha256(roots->token.public_key, roots->token.public_key_len, roots->token.public_digest);
   char jwk[KB_MGMT_TOKEN_JWK_MAX];
   size_t jwk_len = 0;
   assert(!kb_mgmt_token_jwk(roots->token.public_key, roots->token.public_key_len, jwk, sizeof(jwk),
                             &jwk_len));
   sha256(jwk, jwk_len, roots->token.jwk_digest);
   assert(!kb_mgmt_token_kid(roots->token.public_key, roots->token.public_key_len,
                             roots->token.wire_id, sizeof(roots->token.wire_id)));

   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, sizeof(seed));
   size_t public_len = 32;
   assert(key && EVP_PKEY_get_raw_public_key(key, roots->manifest.public_key, &public_len) == 1 &&
          public_len == 32);
   EVP_PKEY_free(key);
   roots->manifest.kind = KB_MGMT_ROOT_MANIFEST;
   roots->manifest.phase = KB_MGMT_ROOT_FINAL;
   roots->manifest.public_key_len = 32;
   roots->manifest.seal_epoch = 1;
   sha256(roots->manifest.public_key, 32, roots->manifest.public_digest);
   assert(!kb_mgmt_manifest_wire_id(roots->manifest.public_key, roots->manifest.wire_id,
                                    sizeof(roots->manifest.wire_id)));
   roots->publication.bound = 1;
   snprintf(roots->publication.custody_key_id, sizeof(roots->publication.custody_key_id),
            "kms:p5-publication");
   snprintf(roots->publication.helper, sizeof(roots->publication.helper), "kms-helper-v1");
   snprintf(roots->publication.verifier_domain, sizeof(roots->publication.verifier_domain),
            "aimee.p5.jwks.publication.v1");
   memset(roots->publication.identity_digest, 0x5a, 32);
   roots->publication.hwm1_attestation_len = 64;
   memset(roots->publication.hwm1_attestation, 0x6b, 64);

   assert(!kb_mgmt_jwks_build_unsigned(roots->token.public_key, roots->token.public_key_len,
                                       INT64_C(1784728800), INT64_C(1784732400), record));
   uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN];
   assert(!kb_mgmt_jwks_ed25519_sign(seed, (const uint8_t *)record->payload, record->payload_len,
                                     signature));
   assert(!kb_mgmt_jwks_complete(roots->manifest.public_key, roots->manifest.wire_id, signature,
                                 record));
   memcpy(record->token_public_digest, roots->token.public_digest, 32);
   memcpy(record->token_jwk_digest, roots->token.jwk_digest, 32);
   memcpy(record->manifest_public_digest, roots->manifest.public_digest, 32);
   memcpy(record->publication_identity_digest, roots->publication.identity_digest, 32);
   record->seal_epoch = 1;
   record->hwm1_attestation_len = roots->publication.hwm1_attestation_len;
   memcpy(record->hwm1_attestation, roots->publication.hwm1_attestation,
          roots->publication.hwm1_attestation_len);
   assert(!kb_mgmt_jwks_validate(roots, record));
   OPENSSL_cleanse(signature, sizeof(signature));
   OPENSSL_cleanse(jwk, sizeof(jwk));
}

static void fuzz_one(const uint8_t *data, size_t size)
{
   if ((!data && size) || size > FUZZ_INPUT_MAX)
      return;
   kb_mgmt_jwks_roots_t roots;
   kb_mgmt_jwks_record_t valid, mutated;
   valid_fixture(&roots, &valid);
   mutated = valid;
   if (size)
   {
      uint8_t bit = (uint8_t)(1u << (data[0] & 7u));
      size_t selector = size > 1 ? data[1] % 9u : 0;
      size_t offset = size > 2 ? data[2] : 0;
      uint8_t *field = NULL;
      size_t field_len = 0;
      switch (selector)
      {
      case 0:
         field = (uint8_t *)mutated.jwks;
         field_len = mutated.jwks_len;
         break;
      case 1:
         field = (uint8_t *)mutated.payload;
         field_len = mutated.payload_len;
         break;
      case 2:
         field = (uint8_t *)mutated.envelope;
         field_len = mutated.envelope_len;
         break;
      case 3:
         field = mutated.signature;
         field_len = sizeof(mutated.signature);
         break;
      case 4:
         field = mutated.manifest_digest;
         field_len = sizeof(mutated.manifest_digest);
         break;
      case 5:
         field = mutated.envelope_digest;
         field_len = sizeof(mutated.envelope_digest);
         break;
      case 6:
         field = (uint8_t *)mutated.manifest_id;
         field_len = strlen(mutated.manifest_id);
         break;
      case 7:
         field = mutated.token_jwk_digest;
         field_len = sizeof(mutated.token_jwk_digest);
         break;
      default:
         field = mutated.publication_identity_digest;
         field_len = sizeof(mutated.publication_identity_digest);
         break;
      }
      assert(field && field_len);
      field[offset % field_len] ^= bit;
      assert(kb_mgmt_jwks_validate(&roots, &mutated) != 0);
   }

   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN];
   memcpy(modulus, roots.token.public_key, sizeof(modulus));
   for (size_t i = 0; i < size && i < sizeof(modulus); ++i)
      modulus[i] ^= data[i];
   int64_t from = 0, until = 0;
   if (size >= sizeof(from))
      memcpy(&from, data, sizeof(from));
   if (size >= sizeof(from) + sizeof(until))
      memcpy(&until, data + sizeof(from), sizeof(until));
   (void)kb_mgmt_jwks_build_unsigned(modulus, sizeof(modulus), from, until, &mutated);
   OPENSSL_cleanse(&mutated, sizeof(mutated));
   OPENSSL_cleanse(&valid, sizeof(valid));
   OPENSSL_cleanse(&roots, sizeof(roots));
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
int main(int argc, char **argv)
{
   uint8_t input[FUZZ_INPUT_MAX];
   if (argc < 2)
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   else
      for (int i = 1; i < argc; ++i)
      {
         FILE *file = fopen(argv[i], "rb");
         if (!file)
            return 1;
         size_t size = fread(input, 1, sizeof(input), file);
         int failed = ferror(file);
         fclose(file);
         if (failed)
            return 1;
         fuzz_one(input, size);
      }
   printf("fuzz_kb_mgmt_jwks_publication: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
