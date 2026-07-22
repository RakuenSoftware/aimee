#include "kb_mgmt_jwks_publication.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define SIGNATURE_ALG "EdDSA"

static void put_u32be(uint8_t out[4], uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

static void put_u64be(uint8_t out[8], uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      out[i] = (uint8_t)(value >> (56 - i * 8));
}

static int digest(const void *value, size_t len, uint8_t out[32])
{
   unsigned int n = 0;
   if (!out)
      return -1;
   OPENSSL_cleanse(out, 32);
   if ((!value && len) || EVP_Digest(value, len, out, &n, EVP_sha256(), NULL) != 1 || n != 32)
   {
      OPENSSL_cleanse(out, 32);
      return -1;
   }
   return 0;
}

static void hex_encode(const uint8_t *value, size_t len, char *out)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < len; ++i)
   {
      out[i * 2] = hex[value[i] >> 4];
      out[i * 2 + 1] = hex[value[i] & 15];
   }
   out[len * 2] = 0;
}

static int digest_part(EVP_MD_CTX *md, const void *value, size_t len)
{
   uint8_t size[4];
   if (!md || (!value && len) || len > UINT32_MAX)
      return -1;
   put_u32be(size, (uint32_t)len);
   return EVP_DigestUpdate(md, size, sizeof(size)) == 1 &&
                  (!len || EVP_DigestUpdate(md, value, len) == 1)
              ? 0
              : -1;
}

static int transcript_digest(const char *domain, const void *a, size_t an, const void *b, size_t bn,
                             const void *c, size_t cn, const void *d, size_t dn, uint8_t out[32])
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            !digest_part(md, domain, strlen(domain)) && !digest_part(md, a, an) &&
            !digest_part(md, b, bn) && !digest_part(md, c, cn) && !digest_part(md, d, dn) &&
            EVP_DigestFinal_ex(md, out, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   if (!ok)
      OPENSSL_cleanse(out, 32);
   return ok ? 0 : -1;
}

static int b64url(const uint8_t *value, size_t len, char *out, size_t cap, size_t *out_len)
{
   unsigned char encoded[128];
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!value || !out || !out_len || len > 64)
      return -1;
   size_t padded = 4 * ((len + 2) / 3);
   if (padded >= sizeof(encoded) || padded + 1 > cap ||
       EVP_EncodeBlock(encoded, value, (int)len) != (int)padded)
      return -1;
   while (padded && encoded[padded - 1] == '=')
      --padded;
   for (size_t i = 0; i < padded; ++i)
      out[i] = encoded[i] == '+' ? '-' : (encoded[i] == '/' ? '_' : (char)encoded[i]);
   out[padded] = 0;
   *out_len = padded;
   OPENSSL_cleanse(encoded, sizeof(encoded));
   return 0;
}

int kb_mgmt_jwks_ed25519_sign(const uint8_t seed[32], const uint8_t *payload, size_t payload_len,
                              uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN])
{
   EVP_PKEY *key = seed ? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32) : NULL;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   size_t n = KB_MGMT_JWKS_SIGNATURE_LEN;
   if (signature)
      OPENSSL_cleanse(signature, KB_MGMT_JWKS_SIGNATURE_LEN);
   int ok = key && md && payload && payload_len && signature &&
            EVP_DigestSignInit(md, NULL, NULL, NULL, key) == 1 &&
            EVP_DigestSign(md, signature, &n, payload, payload_len) == 1 &&
            n == KB_MGMT_JWKS_SIGNATURE_LEN;
   EVP_MD_CTX_free(md);
   EVP_PKEY_free(key);
   if (!ok && signature)
      OPENSSL_cleanse(signature, KB_MGMT_JWKS_SIGNATURE_LEN);
   return ok ? 0 : -1;
}

int kb_mgmt_jwks_ed25519_verify(const uint8_t public_key[32], const uint8_t *payload,
                                size_t payload_len,
                                const uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN])
{
   EVP_PKEY *key =
       public_key ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, 32) : NULL;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   int ok = key && md && payload && payload_len && signature &&
            EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) == 1 &&
            EVP_DigestVerify(md, signature, KB_MGMT_JWKS_SIGNATURE_LEN, payload, payload_len) == 1;
   EVP_MD_CTX_free(md);
   EVP_PKEY_free(key);
   return ok ? 0 : -1;
}

static int candidate_id(kb_mgmt_jwks_record_t *record, const char *manifest_id)
{
   static const char domain[] = "aimee.p5.jwks.candidate.v1";
   uint8_t generation[8], value[32];
   put_u64be(generation, record->generation);
   if (transcript_digest(domain, generation, sizeof(generation), record->payload_digest, 32,
                         manifest_id, strlen(manifest_id), NULL, 0, value))
      return -1;
   hex_encode(value, sizeof(value), record->candidate_id);
   OPENSSL_cleanse(value, sizeof(value));
   return 0;
}

int kb_mgmt_jwks_build_unsigned(const uint8_t *token_modulus, size_t token_modulus_len,
                                int64_t valid_from, int64_t valid_until,
                                kb_mgmt_jwks_record_t *record)
{
   static const char previous[65] =
       "0000000000000000000000000000000000000000000000000000000000000000";
   char jwk[KB_MGMT_TOKEN_JWK_MAX], jwks_hex[65];
   size_t jwk_len = 0;
   if (!record)
      return -1;
   OPENSSL_cleanse(record, sizeof(*record));
   if (valid_until <= valid_from ||
       kb_mgmt_token_jwk(token_modulus, token_modulus_len, jwk, sizeof(jwk), &jwk_len))
      goto fail;
   int n = snprintf(record->jwks, sizeof(record->jwks), "{\"keys\":[%.*s]}", (int)jwk_len, jwk);
   if (n < 0 || (size_t)n >= sizeof(record->jwks))
      goto fail;
   record->jwks_len = (size_t)n;
   if (digest(record->jwks, record->jwks_len, record->jwks_digest))
      goto fail;
   hex_encode(record->jwks_digest, 32, jwks_hex);
   n = snprintf(record->payload, sizeof(record->payload),
                "{\"format_version\":1,\"generation\":1,\"valid_from\":%lld,"
                "\"valid_until\":%lld,\"previous_manifest_sha256\":\"%s\","
                "\"keys\":[%.*s],\"jwks_sha256\":\"%s\"}",
                (long long)valid_from, (long long)valid_until, previous, (int)jwk_len, jwk,
                jwks_hex);
   if (n < 0 || (size_t)n >= sizeof(record->payload))
      goto fail;
   record->payload_len = (size_t)n;
   if (digest(record->payload, record->payload_len, record->payload_digest))
      goto fail;
   record->phase = KB_MGMT_JWKS_STAGED;
   record->generation = KB_MGMT_JWKS_GENERATION;
   record->valid_from = valid_from;
   record->valid_until = valid_until;
   OPENSSL_cleanse(jwk, sizeof(jwk));
   OPENSSL_cleanse(jwks_hex, sizeof(jwks_hex));
   return 0;
fail:
   OPENSSL_cleanse(jwk, sizeof(jwk));
   OPENSSL_cleanse(jwks_hex, sizeof(jwks_hex));
   OPENSSL_cleanse(record, sizeof(*record));
   return -1;
}

int kb_mgmt_jwks_complete(const uint8_t manifest_public[32], const char *manifest_id,
                          const uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN],
                          kb_mgmt_jwks_record_t *record)
{
   static const char domain[] = "aimee.p5.jwks.manifest.v1";
   char signature_text[96], manifest_hex[65];
   size_t signature_len = 0;
   if (!manifest_public || !manifest_id ||
       strnlen(manifest_id, KB_MGMT_MANIFEST_ID_MAX + 1) > KB_MGMT_MANIFEST_ID_MAX ||
       !manifest_id[0] || !signature || !record || record->generation != 1 ||
       !record->payload_len || record->payload_len >= sizeof(record->payload) ||
       kb_mgmt_jwks_ed25519_verify(manifest_public, (const uint8_t *)record->payload,
                                   record->payload_len, signature) ||
       candidate_id(record, manifest_id) ||
       transcript_digest(domain, record->payload, record->payload_len, manifest_id,
                         strlen(manifest_id), SIGNATURE_ALG, sizeof(SIGNATURE_ALG) - 1, signature,
                         KB_MGMT_JWKS_SIGNATURE_LEN, record->manifest_digest) ||
       b64url(signature, KB_MGMT_JWKS_SIGNATURE_LEN, signature_text, sizeof(signature_text),
              &signature_len))
      goto fail;
   memcpy(record->signature, signature, KB_MGMT_JWKS_SIGNATURE_LEN);
   snprintf(record->manifest_id, sizeof(record->manifest_id), "%s", manifest_id);
   hex_encode(record->manifest_digest, 32, manifest_hex);
   int n = snprintf(record->envelope, sizeof(record->envelope),
                    "{\"payload\":%.*s,\"manifest_kid\":\"%s\","
                    "\"signature_alg\":\"EdDSA\",\"signature\":\"%.*s\","
                    "\"manifest_sha256\":\"%s\"}",
                    (int)record->payload_len, record->payload, record->manifest_id,
                    (int)signature_len, signature_text, manifest_hex);
   if (n < 0 || (size_t)n >= sizeof(record->envelope))
      goto fail;
   record->envelope_len = (size_t)n;
   if (digest(record->envelope, record->envelope_len, record->envelope_digest))
      goto fail;
   OPENSSL_cleanse(signature_text, sizeof(signature_text));
   OPENSSL_cleanse(manifest_hex, sizeof(manifest_hex));
   return 0;
fail:
   OPENSSL_cleanse(signature_text, sizeof(signature_text));
   OPENSSL_cleanse(manifest_hex, sizeof(manifest_hex));
   if (record)
   {
      OPENSSL_cleanse(record->candidate_id, sizeof(record->candidate_id));
      OPENSSL_cleanse(record->manifest_id, sizeof(record->manifest_id));
      OPENSSL_cleanse(record->signature, sizeof(record->signature));
      OPENSSL_cleanse(record->manifest_digest, sizeof(record->manifest_digest));
      OPENSSL_cleanse(record->envelope, sizeof(record->envelope));
      record->envelope_len = 0;
      OPENSSL_cleanse(record->envelope_digest, sizeof(record->envelope_digest));
   }
   return -1;
}

static int root_snapshot_valid(const kb_mgmt_jwks_roots_t *roots)
{
   uint8_t value[32];
   char id[KB_MGMT_MANIFEST_ID_MAX + 1];
   char token_id[KB_MGMT_TOKEN_KID_MAX + 1];
   size_t jwk_len = 0;
   char jwk[KB_MGMT_TOKEN_JWK_MAX];
   int ok = roots && roots->token.kind == KB_MGMT_ROOT_TOKEN &&
            roots->token.phase == KB_MGMT_ROOT_FINAL &&
            roots->token.public_key_len == KB_MGMT_TOKEN_MODULUS_LEN &&
            roots->manifest.kind == KB_MGMT_ROOT_MANIFEST &&
            roots->manifest.phase == KB_MGMT_ROOT_FINAL && roots->manifest.public_key_len == 32 &&
            roots->manifest.seal_epoch && roots->publication.bound &&
            roots->publication.custody_key_id[0] && roots->publication.helper[0] &&
            roots->publication.verifier_domain[0] && roots->publication.hwm1_attestation_len &&
            roots->publication.hwm1_attestation_len <= KB_MGMT_ROOT_ATTEST_MAX &&
            !digest(roots->token.public_key, roots->token.public_key_len, value) &&
            !CRYPTO_memcmp(value, roots->token.public_digest, 32) &&
            !kb_mgmt_token_jwk(roots->token.public_key, roots->token.public_key_len, jwk,
                               sizeof(jwk), &jwk_len) &&
            !digest(jwk, jwk_len, value) && !CRYPTO_memcmp(value, roots->token.jwk_digest, 32) &&
            !kb_mgmt_token_kid(roots->token.public_key, roots->token.public_key_len, token_id,
                               sizeof(token_id)) &&
            !strcmp(token_id, roots->token.wire_id) &&
            !digest(roots->manifest.public_key, 32, value) &&
            !CRYPTO_memcmp(value, roots->manifest.public_digest, 32) &&
            !kb_mgmt_manifest_wire_id(roots->manifest.public_key, id, sizeof(id)) &&
            !strcmp(id, roots->manifest.wire_id);
   OPENSSL_cleanse(value, sizeof(value));
   OPENSSL_cleanse(id, sizeof(id));
   OPENSSL_cleanse(token_id, sizeof(token_id));
   OPENSSL_cleanse(jwk, sizeof(jwk));
   return ok;
}

static void bind_roots(const kb_mgmt_jwks_roots_t *roots, kb_mgmt_jwks_record_t *record)
{
   memcpy(record->token_public_digest, roots->token.public_digest, 32);
   memcpy(record->token_jwk_digest, roots->token.jwk_digest, 32);
   memcpy(record->manifest_public_digest, roots->manifest.public_digest, 32);
   memcpy(record->publication_identity_digest, roots->publication.identity_digest, 32);
   record->seal_epoch = roots->manifest.seal_epoch;
}

int kb_mgmt_jwks_validate(const kb_mgmt_jwks_roots_t *roots, const kb_mgmt_jwks_record_t *record)
{
   kb_mgmt_jwks_record_t expected;
   if (!root_snapshot_valid(roots) || !record || record->phase < KB_MGMT_JWKS_STAGED ||
       record->phase > KB_MGMT_JWKS_FINAL || record->generation != 1 || !record->seal_epoch ||
       record->hwm1_attestation_len == 0 ||
       record->hwm1_attestation_len > KB_MGMT_ROOT_ATTEST_MAX ||
       kb_mgmt_jwks_build_unsigned(roots->token.public_key, roots->token.public_key_len,
                                   record->valid_from, record->valid_until, &expected))
      return -1;
   bind_roots(roots, &expected);
   if (kb_mgmt_jwks_complete(roots->manifest.public_key, roots->manifest.wire_id, record->signature,
                             &expected))
      goto fail;
   int ok = !strcmp(record->candidate_id, expected.candidate_id) &&
            !strcmp(record->manifest_id, expected.manifest_id) &&
            record->jwks_len == expected.jwks_len &&
            !CRYPTO_memcmp(record->jwks, expected.jwks, expected.jwks_len + 1) &&
            !CRYPTO_memcmp(record->jwks_digest, expected.jwks_digest, 32) &&
            record->payload_len == expected.payload_len &&
            !CRYPTO_memcmp(record->payload, expected.payload, expected.payload_len + 1) &&
            !CRYPTO_memcmp(record->payload_digest, expected.payload_digest, 32) &&
            record->envelope_len == expected.envelope_len &&
            !CRYPTO_memcmp(record->envelope, expected.envelope, expected.envelope_len + 1) &&
            !CRYPTO_memcmp(record->manifest_digest, expected.manifest_digest, 32) &&
            !CRYPTO_memcmp(record->envelope_digest, expected.envelope_digest, 32) &&
            !CRYPTO_memcmp(record->token_public_digest, expected.token_public_digest, 32) &&
            !CRYPTO_memcmp(record->token_jwk_digest, expected.token_jwk_digest, 32) &&
            !CRYPTO_memcmp(record->manifest_public_digest, expected.manifest_public_digest, 32) &&
            !CRYPTO_memcmp(record->publication_identity_digest,
                           expected.publication_identity_digest, 32) &&
            record->hwm1_attestation_len == roots->publication.hwm1_attestation_len &&
            !CRYPTO_memcmp(record->hwm1_attestation, roots->publication.hwm1_attestation,
                           record->hwm1_attestation_len);
   OPENSSL_cleanse(&expected, sizeof(expected));
   return ok ? 0 : -1;
fail:
   OPENSSL_cleanse(&expected, sizeof(expected));
   return -1;
}

static kb_mgmt_jwks_result_t db_result(kb_mgmt_jwks_db_result_t value)
{
   switch (value)
   {
   case KB_MGMT_JWKS_DB_SEALED:
      return KB_MGMT_JWKS_SEALED;
   case KB_MGMT_JWKS_DB_CONFLICT:
      return KB_MGMT_JWKS_CONFLICT;
   case KB_MGMT_JWKS_DB_INTEGRITY:
      return KB_MGMT_JWKS_INTEGRITY;
   default:
      return KB_MGMT_JWKS_RETRY;
   }
}

static kb_mgmt_jwks_result_t read_hwm(const kb_mgmt_jwks_callbacks_t *cb, const char *id,
                                      uint64_t *version, uint8_t att[KB_MGMT_ROOT_ATTEST_MAX],
                                      size_t *att_len)
{
   *version = 0;
   *att_len = 0;
   OPENSSL_cleanse(att, KB_MGMT_ROOT_ATTEST_MAX);
   kb_mgmt_jwks_hwm_result_t rc =
       cb->hwm_read(cb->ctx, id, version, att, KB_MGMT_ROOT_ATTEST_MAX, att_len);
   if (rc == KB_MGMT_JWKS_HWM_INTEGRITY)
      return KB_MGMT_JWKS_INTEGRITY;
   if (rc != KB_MGMT_JWKS_HWM_OK)
      return KB_MGMT_JWKS_RETRY;
   if (!*version || !*att_len || *att_len > KB_MGMT_ROOT_ATTEST_MAX ||
       cb->hwm_verify(cb->ctx, id, *version, att, *att_len))
      return KB_MGMT_JWKS_INTEGRITY;
   return KB_MGMT_JWKS_FRESH;
}

static int config_valid(const kb_mgmt_jwks_config_t *config)
{
   if (!config || config->valid_until <= config->valid_from || !config->clock_skew_seconds ||
       !config->maximum_lifetime_seconds)
      return 0;
   uint64_t lifetime = (uint64_t)config->valid_until - (uint64_t)config->valid_from;
   return lifetime <= config->maximum_lifetime_seconds;
}

static int fresh_time_valid(const kb_mgmt_jwks_config_t *config)
{
   uint64_t delta = config->valid_from >= config->now
                        ? (uint64_t)config->valid_from - (uint64_t)config->now
                        : (uint64_t)config->now - (uint64_t)config->valid_from;
   return delta <= config->clock_skew_seconds;
}

static int callbacks_valid(const kb_mgmt_jwks_callbacks_t *cb)
{
   return cb && cb->inspect && cb->stage && cb->record_cas && cb->finalize && cb->hwm_read &&
          cb->hwm_cas && cb->hwm_verify && cb->protected_sign;
}

static kb_mgmt_jwks_result_t inspect_valid(const kb_mgmt_jwks_callbacks_t *cb,
                                           kb_mgmt_jwks_roots_t *roots,
                                           kb_mgmt_jwks_record_t *record)
{
   OPENSSL_cleanse(roots, sizeof(*roots));
   OPENSSL_cleanse(record, sizeof(*record));
   kb_mgmt_jwks_db_result_t dr = cb->inspect(cb->ctx, roots, record);
   if (dr != KB_MGMT_JWKS_DB_OK)
      return db_result(dr);
   if (!root_snapshot_valid(roots) || record->phase < KB_MGMT_JWKS_EMPTY ||
       record->phase > KB_MGMT_JWKS_FINAL)
      return KB_MGMT_JWKS_INTEGRITY;
   return KB_MGMT_JWKS_FRESH;
}

static kb_mgmt_jwks_result_t publish_impl(const kb_mgmt_jwks_config_t *config,
                                          const kb_mgmt_jwks_callbacks_t *cb, int export_only,
                                          char *out, size_t cap, size_t *out_len)
{
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!out || !cap || !out_len || !callbacks_valid(cb) || (!export_only && !config_valid(config)))
      return KB_MGMT_JWKS_INTEGRITY;
   kb_mgmt_jwks_roots_t roots;
   kb_mgmt_jwks_record_t record;
   kb_mgmt_jwks_result_t rc = inspect_valid(cb, &roots, &record);
   kb_mgmt_jwks_phase_t initial_phase = record.phase;
   if (rc != KB_MGMT_JWKS_FRESH || (export_only && record.phase != KB_MGMT_JWKS_FINAL))
   {
      if (rc == KB_MGMT_JWKS_FRESH)
         rc = KB_MGMT_JWKS_INTEGRITY;
      goto done;
   }
   if (!export_only && record.phase != KB_MGMT_JWKS_EMPTY &&
       (record.valid_from != config->valid_from || record.valid_until != config->valid_until))
   {
      rc = KB_MGMT_JWKS_CONFLICT;
      goto done;
   }
   uint8_t att[KB_MGMT_ROOT_ATTEST_MAX], signature[KB_MGMT_JWKS_SIGNATURE_LEN];
   uint8_t att_digest[KB_MGMT_JWKS_SHA256_LEN];
   size_t att_len = 0;
   uint64_t live = 0;
   if (record.phase == KB_MGMT_JWKS_EMPTY)
   {
      if (!fresh_time_valid(config))
      {
         rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      rc = read_hwm(cb, roots.publication.custody_key_id, &live, att, &att_len);
      if (rc != KB_MGMT_JWKS_FRESH || live != 1 ||
          cb->hwm_verify(cb->ctx, roots.publication.custody_key_id, 1,
                         roots.publication.hwm1_attestation,
                         roots.publication.hwm1_attestation_len) ||
          att_len != roots.publication.hwm1_attestation_len ||
          CRYPTO_memcmp(att, roots.publication.hwm1_attestation, att_len) ||
          kb_mgmt_jwks_build_unsigned(roots.token.public_key, roots.token.public_key_len,
                                      config->valid_from, config->valid_until, &record))
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      bind_roots(&roots, &record);
      snprintf(record.manifest_id, sizeof(record.manifest_id), "%s", roots.manifest.wire_id);
      if (candidate_id(&record, record.manifest_id))
      {
         rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      rc = cb->protected_sign(cb->ctx, &roots.manifest, 1, record.candidate_id,
                              record.payload_digest, (const uint8_t *)record.payload,
                              record.payload_len, signature);
      if (rc != KB_MGMT_JWKS_FRESH ||
          kb_mgmt_jwks_complete(roots.manifest.public_key, roots.manifest.wire_id, signature,
                                &record))
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      memcpy(record.hwm1_attestation, att, att_len);
      record.hwm1_attestation_len = att_len;
      kb_mgmt_jwks_db_result_t dr = cb->stage(cb->ctx, &record);
      if (dr != KB_MGMT_JWKS_DB_OK)
      {
         rc = db_result(dr);
         goto secret_done;
      }
      rc = inspect_valid(cb, &roots, &record);
      if (rc != KB_MGMT_JWKS_FRESH || record.phase != KB_MGMT_JWKS_STAGED)
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
   }
   if (record.phase == KB_MGMT_JWKS_STAGED)
   {
      if (kb_mgmt_jwks_validate(&roots, &record) ||
          cb->hwm_verify(cb->ctx, roots.publication.custody_key_id, 1, record.hwm1_attestation,
                         record.hwm1_attestation_len) ||
          (!export_only &&
           (record.valid_from != config->valid_from || record.valid_until != config->valid_until)))
      {
         rc = !export_only && (record.valid_from != config->valid_from ||
                               record.valid_until != config->valid_until)
                  ? KB_MGMT_JWKS_CONFLICT
                  : KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      rc = cb->protected_sign(cb->ctx, &roots.manifest, 1, record.candidate_id,
                              record.payload_digest, (const uint8_t *)record.payload,
                              record.payload_len, signature);
      if (rc != KB_MGMT_JWKS_FRESH || CRYPTO_memcmp(signature, record.signature, sizeof(signature)))
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      rc = read_hwm(cb, roots.publication.custody_key_id, &live, att, &att_len);
      if (rc != KB_MGMT_JWKS_FRESH || (live != 1 && live != 2))
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      if (live == 1 && (att_len != record.hwm1_attestation_len ||
                        CRYPTO_memcmp(att, record.hwm1_attestation, att_len)))
      {
         rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      if (live == 1)
      {
         OPENSSL_cleanse(att, sizeof(att));
         att_len = 0;
         kb_mgmt_jwks_hwm_result_t hr = cb->hwm_cas(cb->ctx, roots.publication.custody_key_id, 1, 2,
                                                    att, sizeof(att), &att_len);
         if (hr == KB_MGMT_JWKS_HWM_INTEGRITY)
         {
            rc = KB_MGMT_JWKS_INTEGRITY;
            goto secret_done;
         }
         if (hr != KB_MGMT_JWKS_HWM_OK || !att_len || att_len > sizeof(att) ||
             cb->hwm_verify(cb->ctx, roots.publication.custody_key_id, 2, att, att_len))
         {
            rc = read_hwm(cb, roots.publication.custody_key_id, &live, att, &att_len);
            if (rc != KB_MGMT_JWKS_FRESH || live != 2)
            {
               if (rc == KB_MGMT_JWKS_FRESH)
                  rc = hr == KB_MGMT_JWKS_HWM_INTEGRITY ? KB_MGMT_JWKS_INTEGRITY
                                                        : KB_MGMT_JWKS_RETRY;
               goto secret_done;
            }
         }
      }
      kb_mgmt_jwks_db_result_t dr = cb->record_cas(cb->ctx, &record, att, att_len);
      if (dr != KB_MGMT_JWKS_DB_OK)
      {
         rc = db_result(dr);
         goto secret_done;
      }
      rc = inspect_valid(cb, &roots, &record);
      if (rc != KB_MGMT_JWKS_FRESH || record.phase != KB_MGMT_JWKS_CAS_DONE)
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
   }
   if (record.phase == KB_MGMT_JWKS_CAS_DONE)
   {
      if (kb_mgmt_jwks_validate(&roots, &record))
      {
         rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      rc = read_hwm(cb, roots.publication.custody_key_id, &live, att, &att_len);
      if (rc != KB_MGMT_JWKS_FRESH || live != 2 || digest(att, att_len, att_digest) ||
          CRYPTO_memcmp(record.hwm2_attestation_digest, att_digest, sizeof(att_digest)))
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
      kb_mgmt_jwks_db_result_t dr = cb->finalize(cb->ctx, &record);
      if (dr != KB_MGMT_JWKS_DB_OK)
      {
         rc = db_result(dr);
         goto secret_done;
      }
      rc = inspect_valid(cb, &roots, &record);
      if (rc != KB_MGMT_JWKS_FRESH || record.phase != KB_MGMT_JWKS_FINAL)
      {
         if (rc == KB_MGMT_JWKS_FRESH)
            rc = KB_MGMT_JWKS_INTEGRITY;
         goto secret_done;
      }
   }
   if (record.phase != KB_MGMT_JWKS_FINAL || kb_mgmt_jwks_validate(&roots, &record))
   {
      rc = KB_MGMT_JWKS_INTEGRITY;
      goto secret_done;
   }
   rc = read_hwm(cb, roots.publication.custody_key_id, &live, att, &att_len);
   if (rc != KB_MGMT_JWKS_FRESH || live != 2 || digest(att, att_len, att_digest) ||
       CRYPTO_memcmp(record.hwm2_attestation_digest, att_digest, sizeof(att_digest)) ||
       record.envelope_len + 1 > cap)
   {
      if (rc == KB_MGMT_JWKS_FRESH)
         rc = KB_MGMT_JWKS_INTEGRITY;
      goto secret_done;
   }
   memcpy(out, record.envelope, record.envelope_len + 1);
   *out_len = record.envelope_len;
   rc = export_only || initial_phase == KB_MGMT_JWKS_FINAL ? KB_MGMT_JWKS_CONVERGED
        : initial_phase == KB_MGMT_JWKS_EMPTY              ? KB_MGMT_JWKS_FRESH
                                                           : KB_MGMT_JWKS_RECOVERED;
secret_done:
   OPENSSL_cleanse(att, sizeof(att));
   OPENSSL_cleanse(att_digest, sizeof(att_digest));
   OPENSSL_cleanse(signature, sizeof(signature));
done:
   if (rc != KB_MGMT_JWKS_FRESH && !(export_only && rc == KB_MGMT_JWKS_CONVERGED))
   {
      OPENSSL_cleanse(out, cap);
      *out_len = 0;
   }
   OPENSSL_cleanse(&record, sizeof(record));
   OPENSSL_cleanse(&roots, sizeof(roots));
   return rc;
}

kb_mgmt_jwks_result_t kb_mgmt_jwks_publish(const kb_mgmt_jwks_config_t *config,
                                           const kb_mgmt_jwks_callbacks_t *callbacks, char *out,
                                           size_t cap, size_t *out_len)
{
   return publish_impl(config, callbacks, 0, out, cap, out_len);
}

kb_mgmt_jwks_result_t kb_mgmt_jwks_export(const kb_mgmt_jwks_callbacks_t *callbacks, char *out,
                                          size_t cap, size_t *out_len)
{
   return publish_impl(NULL, callbacks, 1, out, cap, out_len);
}
