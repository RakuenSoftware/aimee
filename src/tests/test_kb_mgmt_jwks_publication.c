#include "kb_mgmt_jwks_publication.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static const uint8_t g_seed[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
static uint64_t g_hwm = 1;

static void sha256(const void *p, size_t n, uint8_t out[32])
{
   unsigned int z = 0;
   assert(EVP_Digest(p, n, out, &z, EVP_sha256(), NULL) == 1 && z == 32);
}

static void attest(uint64_t version, uint8_t out[64])
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   static const char id[] = "kms:p5-publication";
   assert(md && EVP_DigestInit_ex(md, EVP_sha512(), NULL) == 1 &&
          EVP_DigestUpdate(md, id, sizeof(id) - 1) == 1 &&
          EVP_DigestUpdate(md, &version, sizeof(version)) == 1 &&
          EVP_DigestFinal_ex(md, out, &n) == 1 && n == 64);
   EVP_MD_CTX_free(md);
}

typedef struct
{
   kb_mgmt_jwks_roots_t roots;
   kb_mgmt_jwks_record_t record;
   unsigned stages, records, finals, signs;
   int fail_record_once;
   int forged_read;
   int cas_advances_before_error;
   kb_mgmt_jwks_hwm_result_t read_result;
   kb_mgmt_jwks_hwm_result_t cas_result;
} mock_t;

static void roots(mock_t *m)
{
   memset(m, 0, sizeof(*m));
   for (size_t i = 0; i < KB_MGMT_TOKEN_MODULUS_LEN; ++i)
      m->roots.token.public_key[i] = (uint8_t)(0x80u + i * 17u);
   m->roots.token.kind = KB_MGMT_ROOT_TOKEN;
   m->roots.token.phase = KB_MGMT_ROOT_FINAL;
   m->roots.token.public_key_len = KB_MGMT_TOKEN_MODULUS_LEN;
   sha256(m->roots.token.public_key, m->roots.token.public_key_len, m->roots.token.public_digest);
   char jwk[KB_MGMT_TOKEN_JWK_MAX];
   size_t jwk_len = 0;
   assert(!kb_mgmt_token_jwk(m->roots.token.public_key, m->roots.token.public_key_len, jwk,
                             sizeof(jwk), &jwk_len));
   sha256(jwk, jwk_len, m->roots.token.jwk_digest);
   assert(!kb_mgmt_token_kid(m->roots.token.public_key, m->roots.token.public_key_len,
                             m->roots.token.wire_id, sizeof(m->roots.token.wire_id)));

   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, g_seed, sizeof(g_seed));
   size_t public_len = 32;
   assert(key && EVP_PKEY_get_raw_public_key(key, m->roots.manifest.public_key, &public_len) == 1 &&
          public_len == 32);
   EVP_PKEY_free(key);
   m->roots.manifest.kind = KB_MGMT_ROOT_MANIFEST;
   m->roots.manifest.phase = KB_MGMT_ROOT_FINAL;
   m->roots.manifest.public_key_len = 32;
   m->roots.manifest.seal_epoch = 9;
   sha256(m->roots.manifest.public_key, 32, m->roots.manifest.public_digest);
   assert(!kb_mgmt_manifest_wire_id(m->roots.manifest.public_key, m->roots.manifest.wire_id,
                                    sizeof(m->roots.manifest.wire_id)));

   m->roots.publication.bound = 1;
   snprintf(m->roots.publication.custody_key_id, sizeof(m->roots.publication.custody_key_id),
            "kms:p5-publication");
   snprintf(m->roots.publication.helper, sizeof(m->roots.publication.helper), "kms-helper-v1");
   snprintf(m->roots.publication.verifier_domain, sizeof(m->roots.publication.verifier_domain),
            "aimee.p5.jwks.publication.v1");
   for (size_t i = 0; i < 32; ++i)
      m->roots.publication.identity_digest[i] = (uint8_t)(0x40 + i);
   attest(1, m->roots.publication.hwm1_attestation);
   m->roots.publication.hwm1_attestation_len = 64;
}

static kb_mgmt_jwks_db_result_t inspect(void *p, kb_mgmt_jwks_roots_t *r, kb_mgmt_jwks_record_t *v)
{
   mock_t *m = p;
   *r = m->roots;
   *v = m->record;
   return KB_MGMT_JWKS_DB_OK;
}

static kb_mgmt_jwks_db_result_t stage(void *p, const kb_mgmt_jwks_record_t *v)
{
   mock_t *m = p;
   assert(m->record.phase == KB_MGMT_JWKS_EMPTY && v->phase == KB_MGMT_JWKS_STAGED);
   m->record = *v;
   ++m->stages;
   return KB_MGMT_JWKS_DB_OK;
}

static kb_mgmt_jwks_db_result_t record_cas(void *p, const kb_mgmt_jwks_record_t *v,
                                           const uint8_t *att, size_t att_len)
{
   mock_t *m = p;
   if (m->fail_record_once)
   {
      m->fail_record_once = 0;
      return KB_MGMT_JWKS_DB_RETRY;
   }
   assert(v->phase == KB_MGMT_JWKS_STAGED && att_len == 64);
   m->record = *v;
   m->record.phase = KB_MGMT_JWKS_CAS_DONE;
   sha256(att, att_len, m->record.hwm2_attestation_digest);
   ++m->records;
   return KB_MGMT_JWKS_DB_OK;
}

static kb_mgmt_jwks_db_result_t finalize(void *p, const kb_mgmt_jwks_record_t *v)
{
   mock_t *m = p;
   assert(v->phase == KB_MGMT_JWKS_CAS_DONE);
   m->record = *v;
   m->record.phase = KB_MGMT_JWKS_FINAL;
   ++m->finals;
   return KB_MGMT_JWKS_DB_OK;
}

static int verify_att(void *p, const char *id, uint64_t version, const uint8_t *att, size_t n)
{
   uint8_t expected[64];
   (void)p;
   if (strcmp(id, "kms:p5-publication") || n != 64)
      return -1;
   attest(version, expected);
   int rc = CRYPTO_memcmp(expected, att, 64) ? -1 : 0;
   OPENSSL_cleanse(expected, sizeof(expected));
   return rc;
}

static kb_mgmt_jwks_hwm_result_t hwm_read(void *p, const char *id, uint64_t *version, uint8_t *att,
                                          size_t cap, size_t *n)
{
   mock_t *m = p;
   if (m->read_result != KB_MGMT_JWKS_HWM_OK)
      return m->read_result;
   if (strcmp(id, "kms:p5-publication") || cap < 64)
      return KB_MGMT_JWKS_HWM_RETRY;
   *version = g_hwm;
   attest(g_hwm, att);
   if (m->forged_read)
      att[0] ^= 1;
   *n = 64;
   return KB_MGMT_JWKS_HWM_OK;
}

static kb_mgmt_jwks_hwm_result_t hwm_cas(void *p, const char *id, uint64_t old, uint64_t next,
                                         uint8_t *att, size_t cap, size_t *n)
{
   mock_t *m = p;
   if (m->cas_result != KB_MGMT_JWKS_HWM_OK)
   {
      if (m->cas_advances_before_error)
         g_hwm = next;
      return m->cas_result;
   }
   if (strcmp(id, "kms:p5-publication") || cap < 64 || g_hwm != old || old != 1 || next != 2)
      return KB_MGMT_JWKS_HWM_COMPARE;
   g_hwm = next;
   attest(next, att);
   *n = 64;
   return KB_MGMT_JWKS_HWM_OK;
}

static kb_mgmt_jwks_result_t protected_sign(void *p, const kb_mgmt_root_record_t *manifest,
                                            uint64_t generation, const char *candidate_id,
                                            const uint8_t payload_digest[32],
                                            const uint8_t *payload, size_t payload_len,
                                            uint8_t signature[64])
{
   mock_t *m = p;
   uint8_t actual[32];
   sha256(payload, payload_len, actual);
   if (manifest->kind != KB_MGMT_ROOT_MANIFEST || generation != 1 ||
       strlen(candidate_id) != KB_MGMT_JWKS_CANDIDATE_ID_LEN ||
       CRYPTO_memcmp(actual, payload_digest, 32) ||
       kb_mgmt_jwks_ed25519_sign(g_seed, payload, payload_len, signature))
      return KB_MGMT_JWKS_INTEGRITY;
   ++m->signs;
   return KB_MGMT_JWKS_FRESH;
}

static kb_mgmt_jwks_callbacks_t callbacks(mock_t *m)
{
   kb_mgmt_jwks_callbacks_t cb = {.inspect = inspect,
                                  .stage = stage,
                                  .record_cas = record_cas,
                                  .finalize = finalize,
                                  .hwm_read = hwm_read,
                                  .hwm_cas = hwm_cas,
                                  .hwm_verify = verify_att,
                                  .protected_sign = protected_sign,
                                  .ctx = m};
   return cb;
}

static kb_mgmt_jwks_config_t config(void)
{
   kb_mgmt_jwks_config_t c = {.valid_from = 1784728800,
                              .valid_until = 1784732400,
                              .now = 1784728801,
                              .clock_skew_seconds = 60,
                              .maximum_lifetime_seconds = 7200};
   return c;
}

static int zero(const void *p, size_t n)
{
   const uint8_t *b = p;
   for (size_t i = 0; i < n; ++i)
      if (b[i])
         return 0;
   return 1;
}

static void test_codec_and_crypto(void)
{
   mock_t m;
   roots(&m);
   kb_mgmt_jwks_record_t r;
   assert(!kb_mgmt_jwks_build_unsigned(m.roots.token.public_key, m.roots.token.public_key_len,
                                       1784728800, 1784732400, &r));
   assert(r.jwks_len == 633 && r.payload_len == 891);
   static const uint8_t jwks_digest[32] = {0x12, 0x82, 0xa1, 0x7b, 0x51, 0x77, 0xc6, 0x3e,
                                           0x7a, 0xff, 0xa1, 0x78, 0xdb, 0x81, 0xeb, 0x23,
                                           0xac, 0x93, 0x4e, 0x2e, 0x81, 0xd1, 0xfa, 0xd5,
                                           0x60, 0x4b, 0x37, 0x5f, 0xdc, 0xf7, 0x44, 0x1f};
   static const uint8_t payload_digest[32] = {0x95, 0x8f, 0x59, 0xe0, 0x4e, 0x37, 0xe1, 0xb7,
                                              0xb6, 0x31, 0xdd, 0xd0, 0x39, 0xea, 0x12, 0x58,
                                              0x1c, 0x21, 0xce, 0x9e, 0xaf, 0xf3, 0x58, 0x6d,
                                              0x47, 0xf8, 0xf4, 0x3e, 0x2c, 0x2a, 0x78, 0x1a};
   assert(!CRYPTO_memcmp(r.jwks_digest, jwks_digest, 32));
   assert(!CRYPTO_memcmp(r.payload_digest, payload_digest, 32));
   uint8_t sig[64], wrong[32] = {0};
   static const uint8_t signature_fixture[64] = {
       0x1f, 0x03, 0x42, 0x65, 0x19, 0x8c, 0x03, 0xde, 0xdf, 0x22, 0x5e, 0x12, 0xf0,
       0xd3, 0x0a, 0x17, 0x82, 0x10, 0x56, 0x05, 0x27, 0x0f, 0xaa, 0x9b, 0x2f, 0x5c,
       0xc1, 0xf8, 0xb7, 0x4e, 0xa1, 0xb1, 0x19, 0xdb, 0x45, 0x9b, 0xa7, 0x5b, 0x2e,
       0x7d, 0x00, 0xa3, 0x1b, 0x20, 0x39, 0x49, 0x40, 0xec, 0xcc, 0x52, 0x00, 0x8e,
       0x77, 0x7c, 0x19, 0x70, 0xf8, 0x00, 0xfd, 0x07, 0x58, 0x58, 0x50, 0x0d};
   assert(!kb_mgmt_jwks_ed25519_sign(g_seed, (const uint8_t *)r.payload, r.payload_len, sig));
   assert(!CRYPTO_memcmp(sig, signature_fixture, sizeof(sig)));
   assert(!kb_mgmt_jwks_ed25519_verify(m.roots.manifest.public_key, (const uint8_t *)r.payload,
                                       r.payload_len, sig));
   assert(kb_mgmt_jwks_ed25519_verify(wrong, (const uint8_t *)r.payload, r.payload_len, sig));
   assert(!kb_mgmt_jwks_complete(m.roots.manifest.public_key, m.roots.manifest.wire_id, sig, &r));
   static const uint8_t manifest_digest[32] = {0x3c, 0xed, 0x83, 0x71, 0xdc, 0x93, 0x86, 0x93,
                                               0x5b, 0x2e, 0x84, 0x7a, 0x79, 0x5b, 0x5b, 0x6d,
                                               0x37, 0xae, 0xcd, 0x64, 0x63, 0xac, 0xc2, 0x08,
                                               0x31, 0x63, 0x8b, 0x6e, 0x2e, 0x7d, 0x3d, 0x7b};
   static const uint8_t envelope_digest[32] = {0xb0, 0x6a, 0x1f, 0x1a, 0x3e, 0x37, 0xbe, 0xed,
                                               0xa5, 0x57, 0x8d, 0x48, 0x63, 0xed, 0xff, 0xdb,
                                               0x6d, 0xa6, 0x9c, 0x59, 0x42, 0x1c, 0x14, 0x7f,
                                               0x2b, 0x47, 0xf1, 0xd4, 0x28, 0x5b, 0x04, 0x82};
   assert(r.envelope_len == 1179 && !strstr(r.envelope, "=") &&
          !strcmp(r.candidate_id,
                  "96c7cf6add6b6a6b6e786959d3d4bb1eb52884e11fb5f1fde3653b3871466954") &&
          !CRYPTO_memcmp(r.manifest_digest, manifest_digest, 32) &&
          !CRYPTO_memcmp(r.envelope_digest, envelope_digest, 32));
   memcpy(r.token_public_digest, m.roots.token.public_digest, 32);
   memcpy(r.token_jwk_digest, m.roots.token.jwk_digest, 32);
   memcpy(r.manifest_public_digest, m.roots.manifest.public_digest, 32);
   memcpy(r.publication_identity_digest, m.roots.publication.identity_digest, 32);
   r.seal_epoch = m.roots.manifest.seal_epoch;
   memcpy(r.hwm1_attestation, m.roots.publication.hwm1_attestation,
          m.roots.publication.hwm1_attestation_len);
   r.hwm1_attestation_len = m.roots.publication.hwm1_attestation_len;
   assert(!kb_mgmt_jwks_validate(&m.roots, &r));
   r.payload[0] ^= 1;
   assert(kb_mgmt_jwks_validate(&m.roots, &r));
}

static void test_fresh_export_and_recovery(void)
{
   mock_t m;
   roots(&m);
   g_hwm = 1;
   kb_mgmt_jwks_callbacks_t cb = callbacks(&m);
   kb_mgmt_jwks_config_t c = config();
   char out[KB_MGMT_JWKS_ENVELOPE_MAX], saved[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t n = 0, saved_len = 0;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_FRESH);
   assert(n && m.record.phase == KB_MGMT_JWKS_FINAL && m.stages == 1 && m.records == 1 &&
          m.finals == 1 && g_hwm == 2 && m.signs == 2);
   memcpy(saved, out, n + 1);
   saved_len = n;
   memset(out, 0xa5, sizeof(out));
   n = 999;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_CONVERGED);
   assert(!n && zero(out, sizeof(out)));
   assert(kb_mgmt_jwks_export(&cb, out, sizeof(out), &n) == KB_MGMT_JWKS_CONVERGED);
   assert(n == saved_len && !memcmp(out, saved, n + 1));

   roots(&m);
   m.fail_record_once = 1;
   cb = callbacks(&m);
   g_hwm = 1;
   n = 0;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_RETRY);
   assert(g_hwm == 2 && m.record.phase == KB_MGMT_JWKS_STAGED && !n);
   assert(m.record.seal_epoch == 9);
   m.roots.manifest.seal_epoch = 10; /* a later open cycle re-admits private use */
   c.now += 100000;                  /* expiry never changes recovery bytes */
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_RECOVERED);
   assert(m.record.phase == KB_MGMT_JWKS_FINAL && m.record.seal_epoch == 9 && !n);
   assert(kb_mgmt_jwks_export(&cb, out, sizeof(out), &n) == KB_MGMT_JWKS_CONVERGED);
   assert(n == saved_len && !memcmp(out, saved, saved_len + 1) && m.record.seal_epoch == 9);
}

static void test_bounds_conflicts_and_fail_closed(void)
{
   mock_t m;
   roots(&m);
   kb_mgmt_jwks_record_t record;
   memset(&record, 0xa5, sizeof(record));
   assert(kb_mgmt_jwks_build_unsigned(m.roots.token.public_key, m.roots.token.public_key_len - 1,
                                      10, 11, &record));
   assert(zero(&record, sizeof(record)));
   assert(kb_mgmt_jwks_build_unsigned(m.roots.token.public_key, m.roots.token.public_key_len, 10,
                                      10, &record));
   assert(zero(&record, sizeof(record)));
   assert(kb_mgmt_jwks_build_unsigned(m.roots.token.public_key, m.roots.token.public_key_len, -1,
                                      10, &record));
   assert(zero(&record, sizeof(record)));
   assert(kb_mgmt_jwks_build_unsigned(m.roots.token.public_key, m.roots.token.public_key_len,
                                      KB_MGMT_JWKS_TIME_MAX, KB_MGMT_JWKS_TIME_MAX + INT64_C(1),
                                      &record));
   assert(zero(&record, sizeof(record)));

   kb_mgmt_jwks_callbacks_t cb = callbacks(&m);
   kb_mgmt_jwks_config_t c = config();
   char out[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t n = 999;
   memset(out, 0xa5, sizeof(out));
   g_hwm = 1;
   m.forged_read = 1;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(m.record.phase == KB_MGMT_JWKS_EMPTY && !n && zero(out, sizeof(out)));
   m.forged_read = 0;
   c.now = c.valid_from + (int64_t)c.clock_skew_seconds + 1;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(m.record.phase == KB_MGMT_JWKS_EMPTY && !n && zero(out, sizeof(out)));
   c = config();
   c.valid_from = -1;
   c.now = 0;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(m.record.phase == KB_MGMT_JWKS_EMPTY && !n && zero(out, sizeof(out)));
   c = config();
   c.valid_until = KB_MGMT_JWKS_TIME_MAX + INT64_C(1);
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(m.record.phase == KB_MGMT_JWKS_EMPTY && !n && zero(out, sizeof(out)));

   c = config();
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_FRESH);
   c.valid_until++;
   memset(out, 0xa5, sizeof(out));
   n = 999;
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_CONFLICT);
   assert(!n && zero(out, sizeof(out)));

   m.record.signature[0] ^= 1;
   assert(kb_mgmt_jwks_export(&cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(!n && zero(out, sizeof(out)));
   m.record.signature[0] ^= 1;
   g_hwm = 3;
   assert(kb_mgmt_jwks_export(&cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(!n && zero(out, sizeof(out)));
}

static void test_provider_failure_matrix(void)
{
   mock_t m;
   char out[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t n;

#define RESET_PROVIDER_CASE()                                                                      \
   do                                                                                              \
   {                                                                                               \
      roots(&m);                                                                                   \
      g_hwm = 1;                                                                                   \
      memset(out, 0xa5, sizeof(out));                                                              \
      n = 999;                                                                                     \
   } while (0)

   RESET_PROVIDER_CASE();
   m.read_result = KB_MGMT_JWKS_HWM_RETRY;
   kb_mgmt_jwks_callbacks_t cb = callbacks(&m);
   kb_mgmt_jwks_config_t c = config();
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_RETRY);
   assert(!m.stages && !m.signs && !n && zero(out, sizeof(out)));

   RESET_PROVIDER_CASE();
   m.read_result = KB_MGMT_JWKS_HWM_INTEGRITY;
   cb = callbacks(&m);
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(!m.stages && !m.signs && !n && zero(out, sizeof(out)));

   const uint64_t invalid_fresh_versions[] = {0, 2, 3};
   for (size_t i = 0; i < sizeof(invalid_fresh_versions) / sizeof(invalid_fresh_versions[0]); ++i)
   {
      RESET_PROVIDER_CASE();
      g_hwm = invalid_fresh_versions[i];
      cb = callbacks(&m);
      assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
      assert(!m.stages && !m.signs && !n && zero(out, sizeof(out)));
   }

   RESET_PROVIDER_CASE();
   m.cas_result = KB_MGMT_JWKS_HWM_COMPARE;
   cb = callbacks(&m);
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_RETRY);
   assert(m.record.phase == KB_MGMT_JWKS_STAGED && g_hwm == 1 && !n && zero(out, sizeof(out)));

   RESET_PROVIDER_CASE();
   m.cas_result = KB_MGMT_JWKS_HWM_COMPARE;
   m.cas_advances_before_error = 1;
   cb = callbacks(&m);
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_FRESH);
   assert(m.record.phase == KB_MGMT_JWKS_FINAL && g_hwm == 2 && n > 0);

   RESET_PROVIDER_CASE();
   m.cas_result = KB_MGMT_JWKS_HWM_INTEGRITY;
   cb = callbacks(&m);
   assert(kb_mgmt_jwks_publish(&c, &cb, out, sizeof(out), &n) == KB_MGMT_JWKS_INTEGRITY);
   assert(m.record.phase == KB_MGMT_JWKS_STAGED && g_hwm == 1 && !n && zero(out, sizeof(out)));

#undef RESET_PROVIDER_CASE
}

int main(void)
{
   test_codec_and_crypto();
   test_fresh_export_and_recovery();
   test_bounds_conflicts_and_fail_closed();
   test_provider_failure_matrix();
   puts("kb_mgmt_jwks_publication: all tests passed");
   return 0;
}
