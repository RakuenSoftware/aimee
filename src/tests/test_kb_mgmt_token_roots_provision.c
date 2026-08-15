#include "kb_mgmt_token_roots_provision.h"
#include "modules/vault/vault_server_key.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

struct vault_maintenance_guard
{
   int unused;
};

static struct vault_maintenance_guard g_guard;
static uint8_t g_kek[VAULT_KEK_LEN];
static uint64_t g_token_hwm = 1, g_manifest_hwm = 1, g_publication_hwm = 1;

static uint64_t *hwm_for(const char *id)
{
   if (!strcmp(id, "kms:p5-token"))
      return &g_token_hwm;
   if (!strcmp(id, "kms:p5-manifest"))
      return &g_manifest_hwm;
   if (!strcmp(id, "kms:p5-publication"))
      return &g_publication_hwm;
   return NULL;
}

static void attest(const char *id, uint64_t version, uint8_t out[64])
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   assert(md && EVP_DigestInit_ex(md, EVP_sha512(), NULL) == 1 &&
          EVP_DigestUpdate(md, id, strlen(id)) == 1 &&
          EVP_DigestUpdate(md, &version, sizeof(version)) == 1 &&
          EVP_DigestFinal_ex(md, out, &n) == 1 && n == 64);
   EVP_MD_CTX_free(md);
}

int vault_hwm_verify(const char *id, uint64_t version, const uint8_t *att, size_t len)
{
   uint8_t expected[64];
   if (!hwm_for(id) || !att || len != 64)
      return -1;
   attest(id, version, expected);
   int rc = CRYPTO_memcmp(expected, att, 64) ? -1 : 0;
   OPENSSL_cleanse(expected, sizeof(expected));
   return rc;
}

int vault_hwm_read(const char *id, uint64_t *version, uint8_t *att, size_t cap, size_t *len)
{
   uint64_t *hwm = hwm_for(id);
   if (!hwm || cap < 64)
      return -1;
   *version = *hwm;
   attest(id, *hwm, att);
   *len = 64;
   return 0;
}

int vault_hwm_cas(const char *id, uint64_t expected, uint64_t next, uint8_t *att, size_t cap,
                  size_t *len)
{
   uint64_t *hwm = hwm_for(id);
   if (!hwm || cap < 64 || *hwm != expected || expected != 1 || next != 2)
      return -1;
   *hwm = next;
   attest(id, next, att);
   *len = 64;
   return 0;
}

int vault_maintenance_guard_begin(vault_maintenance_guard_t **guard)
{
   *guard = &g_guard;
   return VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_sync_primary_epoch(vault_maintenance_guard_t *guard, uint64_t epoch)
{
   return guard == &g_guard && epoch == 9 ? VAULT_MAINTENANCE_OK : VAULT_MAINTENANCE_EPOCH;
}

int vault_maintenance_guard_unseal(vault_maintenance_guard_t *guard, const void *p, size_t n)
{
   return guard == &g_guard && !p && !n ? VAULT_MAINTENANCE_OK : VAULT_MAINTENANCE_ERROR;
}

int vault_maintenance_guard_with_active_kek(vault_maintenance_guard_t *guard,
                                            vault_maintenance_kek_fn fn, void *ctx)
{
   return guard == &g_guard ? fn(g_kek, ctx) : VAULT_MAINTENANCE_INVALID;
}

int vault_maintenance_guard_end(vault_maintenance_guard_t **guard)
{
   if (!guard || *guard != &g_guard)
      return VAULT_MAINTENANCE_INVALID;
   *guard = NULL;
   return VAULT_MAINTENANCE_OK;
}

typedef struct
{
   kb_mgmt_root_record_t token;
   kb_mgmt_root_record_t manifest;
   kb_mgmt_publication_root_t publication;
   int fail_record_cas_once;
   unsigned stages, cas_records, finals, publication_binds;
} mock_db_t;

static kb_mgmt_root_record_t *record_for(mock_db_t *db, kb_mgmt_root_kind_t kind)
{
   return kind == KB_MGMT_ROOT_TOKEN ? &db->token : &db->manifest;
}

static kb_mgmt_root_db_result_t inspect_root(void *opaque, kb_mgmt_root_kind_t kind, const char *id,
                                             kb_mgmt_root_record_t *out)
{
   mock_db_t *db = opaque;
   *out = *record_for(db, kind);
   if (out->phase == KB_MGMT_ROOT_EMPTY)
   {
      memset(out, 0, sizeof(*out));
      out->kind = kind;
      out->phase = KB_MGMT_ROOT_EMPTY;
      out->seal_epoch = 9;
   }
   else
      assert(!strcmp(out->custody_key_id, id));
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t stage_root(void *opaque, const kb_mgmt_root_record_t *r)
{
   mock_db_t *db = opaque;
   assert(r->phase == KB_MGMT_ROOT_STAGED && record_for(db, r->kind)->phase == KB_MGMT_ROOT_EMPTY);
   *record_for(db, r->kind) = *r;
   db->stages++;
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t record_cas(void *opaque, const kb_mgmt_root_record_t *r,
                                           const uint8_t *att, size_t len)
{
   mock_db_t *db = opaque;
   if (db->fail_record_cas_once)
   {
      db->fail_record_cas_once = 0;
      return KB_MGMT_ROOT_DB_RETRY;
   }
   kb_mgmt_root_record_t *saved = record_for(db, r->kind);
   assert(saved->phase == KB_MGMT_ROOT_STAGED && !vault_hwm_verify(r->custody_key_id, 2, att, len));
   saved->phase = KB_MGMT_ROOT_CAS_DONE;
   memcpy(saved->hwm2_attestation, att, len);
   saved->hwm2_attestation_len = len;
   db->cas_records++;
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t finalize_root(void *opaque, const kb_mgmt_root_record_t *r)
{
   mock_db_t *db = opaque;
   kb_mgmt_root_record_t *saved = record_for(db, r->kind);
   assert(saved->phase == KB_MGMT_ROOT_CAS_DONE && r->phase == KB_MGMT_ROOT_CAS_DONE);
   saved->phase = KB_MGMT_ROOT_FINAL;
   db->finals++;
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t inspect_publication(void *opaque, kb_mgmt_publication_root_t *out)
{
   *out = ((mock_db_t *)opaque)->publication;
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t bind_publication(void *opaque, const kb_mgmt_publication_root_t *p)
{
   mock_db_t *db = opaque;
   assert(!db->publication.bound && p->bound);
   db->publication = *p;
   db->publication_binds++;
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_roots_db_t seam(mock_db_t *db)
{
   kb_mgmt_roots_db_t value = {.inspect_root = inspect_root,
                               .stage_root = stage_root,
                               .record_cas = record_cas,
                               .finalize_root = finalize_root,
                               .inspect_publication = inspect_publication,
                               .bind_publication = bind_publication,
                               .ctx = db};
   return value;
}

static kb_mgmt_roots_config_t config(void)
{
   kb_mgmt_roots_config_t c = {.token_custody_key_id = "kms:p5-token",
                               .manifest_custody_key_id = "kms:p5-manifest",
                               .publication_custody_key_id = "kms:p5-publication",
                               .publication_helper = "vault-kms-hwm-v1",
                               .publication_verifier_domain = "aimee.p5.jwks.publication.v1"};
   for (size_t i = 0; i < sizeof(c.publication_identity_digest); ++i)
      c.publication_identity_digest[i] = (uint8_t)(0x40 + i);
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

static void reset_hwm(void)
{
   g_token_hwm = g_manifest_hwm = g_publication_hwm = 1;
   for (size_t i = 0; i < sizeof(g_kek); ++i)
      g_kek[i] = (uint8_t)(0xa0 + i);
}

static void test_canonical_codec(void)
{
   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN], decoded[KB_MGMT_TOKEN_MODULUS_LEN];
   for (size_t i = 0; i < sizeof(modulus); ++i)
      modulus[i] = (uint8_t)(0x80u + i * 17u);
   char kid[65], jwk[KB_MGMT_TOKEN_JWK_MAX];
   size_t jwk_len = 0, decoded_len = 0;
   assert(!kb_mgmt_token_kid(modulus, sizeof(modulus), kid, sizeof(kid)));
   assert(!strcmp(kid, "p5-token-v1-e1ea1a272dfff71b183da6c86cac9fec"));
   modulus[0] = 0;
   memset(kid, 0xa5, sizeof(kid));
   assert(kb_mgmt_token_kid(modulus, sizeof(modulus), kid, sizeof(kid)));
   assert(zero(kid, sizeof(kid)));
   modulus[0] = 0x80;
   assert(!kb_mgmt_token_jwk(modulus, sizeof(modulus), jwk, sizeof(jwk), &jwk_len));
   assert(!kb_mgmt_token_jwk_validate(jwk, jwk_len, decoded, sizeof(decoded), &decoded_len));
   assert(decoded_len == sizeof(modulus) && !CRYPTO_memcmp(decoded, modulus, sizeof(modulus)));
   char exact_jwk[KB_MGMT_TOKEN_JWK_MAX];
   memcpy(exact_jwk, jwk, jwk_len);
   memset(exact_jwk + jwk_len, 0xa5, sizeof(exact_jwk) - jwk_len);
   assert(!kb_mgmt_token_jwk_validate(exact_jwk, jwk_len, decoded, sizeof(decoded), &decoded_len));
   jwk[jwk_len - 2] = '=';
   assert(kb_mgmt_token_jwk_validate(jwk, jwk_len, decoded, sizeof(decoded), &decoded_len));
   assert(zero(decoded, sizeof(decoded)) && !decoded_len);

   uint8_t manifest[32], publication[32];
   for (size_t i = 0; i < 32; ++i)
   {
      manifest[i] = (uint8_t)i;
      publication[i] = (uint8_t)(0xff - i);
   }
   char manifest_id[65], bundle[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_len = 0;
   assert(!kb_mgmt_manifest_wire_id(manifest, manifest_id, sizeof(manifest_id)));
   assert(!strcmp(manifest_id, "p5-jwks-root-v1-630dcd2966c4336691125448bbb25b4f"));
   assert(!kb_mgmt_public_bundle(modulus, sizeof(modulus), manifest, publication, bundle,
                                 sizeof(bundle), &bundle_len));
   assert(!kb_mgmt_public_bundle_validate(bundle, bundle_len));
   assert(strstr(
       bundle,
       "\"bundle_sha256\":\"874471b4b7b3d6feb46e719fa960441a0c6af42abb27e864b07e680f4a5a4ac1\"}"));
   char exact_bytes[KB_MGMT_PUBLIC_BUNDLE_MAX];
   memcpy(exact_bytes, bundle, bundle_len); /* deliberately no terminator at bundle_len */
   memset(exact_bytes + bundle_len, 0xa5, sizeof(exact_bytes) - bundle_len);
   assert(!kb_mgmt_public_bundle_validate(exact_bytes, bundle_len));
   bundle[bundle_len - 3] ^= 1;
   assert(kb_mgmt_public_bundle_validate(bundle, bundle_len));

   char bootstrap[65];
   assert(!kb_mgmt_root_bootstrap_id(KB_MGMT_ROOT_TOKEN, "kms:p5-token", bootstrap));
   assert(!strcmp(bootstrap, "15fb2dc9b74adb4a87d0d74a5683f13a61bf1e71b31a830108f09723786da9c6"));
   assert(!kb_mgmt_root_bootstrap_id(KB_MGMT_ROOT_MANIFEST, "kms:p5-manifest", bootstrap));
   assert(!strcmp(bootstrap, "61131823f787c38cc793c9fd7a44b085d4f404492b220febe93d189022496525"));
}

static void test_root_aad_fixtures(void)
{
   static const uint8_t token_sha256[32] = {0x79, 0x46, 0x4a, 0xd8, 0x8b, 0xe2, 0x60, 0xde,
                                            0x59, 0x93, 0x7b, 0xec, 0xe0, 0x54, 0x5c, 0x63,
                                            0xf4, 0x50, 0xa1, 0xd0, 0xfa, 0x41, 0x94, 0xd8,
                                            0x63, 0xb9, 0x01, 0xb2, 0x43, 0x33, 0x48, 0xc1};
   static const uint8_t manifest_sha256[32] = {0x8b, 0x10, 0x24, 0x52, 0xbe, 0x37, 0xa2, 0xf5,
                                               0x14, 0x95, 0xfe, 0x4f, 0x15, 0xac, 0x7d, 0x8b,
                                               0x73, 0x64, 0xce, 0xea, 0x3b, 0xc1, 0x6d, 0xd7,
                                               0x9f, 0xab, 0x8e, 0xe0, 0x48, 0x7d, 0x5c, 0xfd};
   uint8_t token[VAULT_ENVELOPE_AAD_MAX], leaf_token[KB_MGMT_TOKEN_ROOT_AAD_MAX];
   uint8_t manifest[VAULT_ENVELOPE_AAD_MAX], digest[32];
   size_t token_len = 0, leaf_token_len = 0, manifest_len = 0;
   unsigned int digest_len = 0;
   assert(!kb_mgmt_root_aad(KB_MGMT_ROOT_TOKEN, 2, token, sizeof(token), &token_len));
   assert(token_len == 90 &&
          EVP_Digest(token, token_len, digest, &digest_len, EVP_sha256(), NULL) == 1 &&
          digest_len == 32 && !CRYPTO_memcmp(digest, token_sha256, 32));
   assert(!kb_mgmt_token_root_aad(2, leaf_token, sizeof(leaf_token), &leaf_token_len));
   assert(leaf_token_len == token_len && !CRYPTO_memcmp(leaf_token, token, token_len));
   uint8_t versioned[KB_MGMT_TOKEN_ROOT_AAD_MAX];
   size_t versioned_len = 0;
   assert(!kb_mgmt_token_root_aad(INT64_C(0x0102030405060708), versioned, sizeof(versioned),
                                  &versioned_len));
   assert(versioned_len == 90 &&
          !memcmp(versioned + versioned_len - 8, "\x01\x02\x03\x04\x05\x06\x07\x08", 8));
   assert(!kb_mgmt_root_aad(KB_MGMT_ROOT_MANIFEST, 2, manifest, sizeof(manifest), &manifest_len));
   assert(manifest_len == 103 &&
          EVP_Digest(manifest, manifest_len, digest, &digest_len, EVP_sha256(), NULL) == 1 &&
          digest_len == 32 && !CRYPTO_memcmp(digest, manifest_sha256, 32));
   assert(token_len != manifest_len || CRYPTO_memcmp(token, manifest, token_len));
   memset(token, 0xa5, sizeof(token));
   token_len = 999;
   assert(kb_mgmt_root_aad(KB_MGMT_ROOT_TOKEN, 2, token, 8, &token_len));
   assert(!token_len && zero(token, 8));
   assert(kb_mgmt_root_aad((kb_mgmt_root_kind_t)99, 2, token, sizeof(token), &token_len));
   assert(!token_len && zero(token, sizeof(token)));
}

static void test_fresh_final_export_and_integrity(void)
{
   reset_hwm();
   mock_db_t db = {0};
   kb_mgmt_roots_db_t callbacks = seam(&db);
   kb_mgmt_roots_config_t c = config();
   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX], original[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_len = 0, original_len = 0;
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FRESH);
   assert(bundle_len && !kb_mgmt_public_bundle_validate(bundle, bundle_len));
   memcpy(original, bundle, bundle_len + 1);
   original_len = bundle_len;
   assert(db.token.phase == KB_MGMT_ROOT_FINAL && db.manifest.phase == KB_MGMT_ROOT_FINAL &&
          db.publication.bound && db.stages == 2 && db.cas_records == 2 && db.finals == 2 &&
          db.publication_binds == 1 && g_token_hwm == 2 && g_manifest_hwm == 2 &&
          g_publication_hwm == 1);

   memset(bundle, 0xa5, sizeof(bundle));
   bundle_len = 999;
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FINAL);
   assert(!bundle_len && zero(bundle, sizeof(bundle)));
   assert(kb_mgmt_token_roots_export(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FINAL);
   assert(bundle_len == original_len && !memcmp(bundle, original, original_len + 1));

   /* The JWKS publisher consumes the bound publication HWM's one permitted
    * CAS. A completed wizard must remain retryable after that irreversible
    * step: roots are still fixed and exportable, while an unbound HWM at 2 is
    * rejected below by the ordinary integrity checks. */
   g_publication_hwm = 2;
   memset(bundle, 0xa5, sizeof(bundle));
   bundle_len = 999;
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FINAL);
   assert(!bundle_len && zero(bundle, sizeof(bundle)));
   assert(kb_mgmt_token_roots_export(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FINAL);
   assert(bundle_len == original_len && !memcmp(bundle, original, original_len + 1));

   kb_mgmt_publication_root_t saved_publication = db.publication;
   unsigned bind_count = db.publication_binds;
   memset(&db.publication, 0, sizeof(db.publication));
   memset(bundle, 0xa5, sizeof(bundle));
   bundle_len = 999;
   assert(kb_mgmt_token_roots_export(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_INTEGRITY);
   assert(db.publication_binds == bind_count && !db.publication.bound && !bundle_len &&
          zero(bundle, sizeof(bundle)));
   db.publication = saved_publication;

   db.token.v2.ciphertext[0] ^= 1;
   assert(!kb_mgmt_root_envelope_digest(KB_MGMT_ROOT_TOKEN, &db.token.v2, db.token.v2_digest));
   memset(bundle, 0xa5, sizeof(bundle));
   bundle_len = 999;
   assert(kb_mgmt_token_roots_export(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_INTEGRITY);
   assert(!bundle_len && zero(bundle, sizeof(bundle)));
}

static void test_crash_after_cas(void)
{
   reset_hwm();
   mock_db_t db = {.fail_record_cas_once = 1};
   kb_mgmt_roots_db_t callbacks = seam(&db);
   kb_mgmt_roots_config_t c = config();
   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_len = 0;
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_RETRY);
   assert(db.token.phase == KB_MGMT_ROOT_STAGED && g_token_hwm == 2 && !bundle_len &&
          zero(bundle, sizeof(bundle)));
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_RECOVERED);
   assert(db.token.phase == KB_MGMT_ROOT_FINAL && db.manifest.phase == KB_MGMT_ROOT_FINAL &&
          db.publication.bound && !bundle_len && zero(bundle, sizeof(bundle)));
   assert(kb_mgmt_token_roots_export(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FINAL);
   assert(bundle_len && !kb_mgmt_public_bundle_validate(bundle, bundle_len));
}

static void test_invalid_hwm_cross_product(void)
{
   reset_hwm();
   mock_db_t baseline = {0};
   kb_mgmt_roots_db_t callbacks = seam(&baseline);
   kb_mgmt_roots_config_t c = config();
   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_len = 0;
   assert(kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len) ==
          KB_MGMT_ROOTS_FRESH);

   static const struct
   {
      kb_mgmt_root_phase_t phase;
      uint64_t live;
   } invalid[] = {
       {KB_MGMT_ROOT_EMPTY, 0},    {KB_MGMT_ROOT_EMPTY, 2},    {KB_MGMT_ROOT_EMPTY, 3},
       {KB_MGMT_ROOT_STAGED, 0},   {KB_MGMT_ROOT_STAGED, 3},   {KB_MGMT_ROOT_CAS_DONE, 0},
       {KB_MGMT_ROOT_CAS_DONE, 1}, {KB_MGMT_ROOT_CAS_DONE, 3}, {KB_MGMT_ROOT_FINAL, 0},
       {KB_MGMT_ROOT_FINAL, 1},    {KB_MGMT_ROOT_FINAL, 3}};
   for (kb_mgmt_root_kind_t kind = KB_MGMT_ROOT_TOKEN; kind <= KB_MGMT_ROOT_MANIFEST; ++kind)
   {
      for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
      {
         mock_db_t db = baseline;
         callbacks = seam(&db);
         kb_mgmt_root_record_t *record = record_for(&db, kind);
         if (invalid[i].phase == KB_MGMT_ROOT_EMPTY)
            memset(record, 0, sizeof(*record));
         else
         {
            record->phase = invalid[i].phase;
            if (invalid[i].phase == KB_MGMT_ROOT_STAGED)
            {
               OPENSSL_cleanse(record->hwm2_attestation, sizeof(record->hwm2_attestation));
               record->hwm2_attestation_len = 0;
            }
         }
         g_token_hwm = kind == KB_MGMT_ROOT_TOKEN ? invalid[i].live : 2;
         g_manifest_hwm = kind == KB_MGMT_ROOT_MANIFEST ? invalid[i].live : 2;
         g_publication_hwm = 1;
         memset(bundle, 0xa5, sizeof(bundle));
         bundle_len = 999;
         kb_mgmt_roots_result_t result =
             kb_mgmt_token_roots_provision(&c, &callbacks, bundle, sizeof(bundle), &bundle_len);
         assert((result == KB_MGMT_ROOTS_INTEGRITY || result == KB_MGMT_ROOTS_RETRY) &&
                !bundle_len && zero(bundle, sizeof(bundle)));
      }
   }
}

int main(void)
{
   test_canonical_codec();
   test_root_aad_fixtures();
   test_fresh_final_export_and_integrity();
   test_crash_after_cas();
   test_invalid_hwm_cross_product();
   puts("kb_mgmt_token_roots_provision: all tests passed");
   return 0;
}
