#include "db1.h"
#include "db1/db1_internal.h"
#include "kb/kb_mgmt_jwks_publication.h"
#include "kb/kb_mgmt_token_roots_provision.h"
#include "server/server_mgmt_jwks_cache.h"

#include <assert.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
   const char *bundle;
   size_t bundle_n;
   const char *envelope;
   size_t envelope_n;
   atomic_int calls;
} refresh_ctx_t;

static int fetch_fixture(void *opaque, char *out, size_t cap, size_t *out_n)
{
   refresh_ctx_t *ctx = opaque;
   atomic_fetch_add(&ctx->calls, 1);
   usleep(50000);
   if (ctx->envelope_n + 1 > cap)
      return -1;
   memcpy(out, ctx->envelope, ctx->envelope_n + 1);
   *out_n = ctx->envelope_n;
   return 0;
}

static void *refresh_thread(void *opaque)
{
   refresh_ctx_t *ctx = opaque;
   assert(server_mgmt_jwks_cache_refresh(ctx->bundle, ctx->bundle_n, 100, fetch_fixture, ctx) ==
          SERVER_MGMT_JWKS_CACHE_OK);
   return NULL;
}

static void fixture(int64_t from, int64_t until, unsigned char manifest_seed[32],
                    char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX], size_t *bundle_n,
                    char envelope[KB_MGMT_JWKS_ENVELOPE_MAX], size_t *envelope_n)
{
   unsigned char modulus[KB_MGMT_TOKEN_MODULUS_LEN];
   for (size_t i = 0; i < sizeof(modulus); ++i)
      modulus[i] = (unsigned char)(i * 17 + 3);
   modulus[0] |= 0x80;
   for (size_t i = 0; i < 32; ++i)
      manifest_seed[i] = (unsigned char)(i * 11 + 7);
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, manifest_seed, 32);
   assert(key);
   unsigned char manifest_public[32], publication[32];
   size_t public_n = sizeof(manifest_public);
   assert(EVP_PKEY_get_raw_public_key(key, manifest_public, &public_n) == 1 && public_n == 32);
   EVP_PKEY_free(key);
   for (size_t i = 0; i < sizeof(publication); ++i)
      publication[i] = (unsigned char)(0xa0 + i);
   assert(kb_mgmt_public_bundle(modulus, sizeof(modulus), manifest_public, publication, bundle,
                                KB_MGMT_PUBLIC_BUNDLE_MAX, bundle_n) == 0);
   kb_mgmt_jwks_record_t record;
   assert(kb_mgmt_jwks_build_unsigned(modulus, sizeof(modulus), from, until, &record) == 0);
   unsigned char signature[64];
   assert(kb_mgmt_jwks_ed25519_sign(manifest_seed, (const unsigned char *)record.payload,
                                    record.payload_len, signature) == 0);
   char manifest_id[65];
   assert(kb_mgmt_manifest_wire_id(manifest_public, manifest_id, sizeof(manifest_id)) == 0);
   assert(kb_mgmt_jwks_complete(manifest_public, manifest_id, signature, &record) == 0);
   memcpy(envelope, record.envelope, record.envelope_len + 1);
   *envelope_n = record.envelope_len;
}

int main(void)
{
   char db_path[] = "/tmp/aimee-management-jwks-cache-XXXXXX";
   int fd = mkstemp(db_path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(db_path) == 0);

   unsigned char seed[32];
   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX], envelope[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t bundle_n = 0, envelope_n = 0;
   fixture(90, 200, seed, bundle, &bundle_n, envelope, &envelope_n);
   if (geteuid() == 0)
   {
      char trust_path[] = "/tmp/aimee-management-jwks-trust-XXXXXX";
      int trust_fd = mkstemp(trust_path);
      assert(trust_fd >= 0);
      assert(write(trust_fd, bundle, bundle_n) == (ssize_t)bundle_n);
      assert(close(trust_fd) == 0 && chmod(trust_path, 0600) == 0);
      char loaded[SERVER_MGMT_JWKS_BUNDLE_MAX];
      size_t loaded_n = 0;
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) ==
             0);
      assert(loaded_n == bundle_n && !memcmp(loaded, bundle, bundle_n));
      assert(chmod(trust_path, 0660) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) !=
             0);
      assert(chmod(trust_path, 0600) == 0);
      char link_path[sizeof(trust_path) + 8];
      snprintf(link_path, sizeof(link_path), "%s.link", trust_path);
      assert(link(trust_path, link_path) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) !=
             0);
      assert(unlink(link_path) == 0);
      assert(symlink(trust_path, link_path) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(link_path, loaded, sizeof(loaded), &loaded_n) != 0);
      assert(unlink(link_path) == 0 && unlink(trust_path) == 0);
   }
   server_mgmt_jwks_cache_record_t record;
   assert(server_mgmt_jwks_envelope_validate(bundle, bundle_n, envelope, envelope_n, 100,
                                             &record) == SERVER_MGMT_JWKS_CACHE_OK);
   assert(record.generation == 1 && record.valid_from == 90 && record.valid_until == 200);
   assert(strncmp(record.jwks, "{\"keys\":[", 9) == 0);
   char expected_jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   snprintf(expected_jwks, sizeof(expected_jwks), "%s", record.jwks);
   assert(server_mgmt_jwks_envelope_validate(bundle, bundle_n, envelope, envelope_n, 200,
                                             &record) == SERVER_MGMT_JWKS_CACHE_STALE);
   char corrupt[KB_MGMT_JWKS_ENVELOPE_MAX];
   memcpy(corrupt, envelope, envelope_n + 1);
   corrupt[20] ^= 1;
   assert(server_mgmt_jwks_envelope_validate(bundle, bundle_n, corrupt, envelope_n, 100, &record) ==
          SERVER_MGMT_JWKS_CACHE_INVALID);

   refresh_ctx_t refresh = {
       .bundle = bundle, .bundle_n = bundle_n, .envelope = envelope, .envelope_n = envelope_n};
   pthread_t threads[32];
   for (size_t i = 0; i < 32; ++i)
      assert(pthread_create(&threads[i], NULL, refresh_thread, &refresh) == 0);
   for (size_t i = 0; i < 32; ++i)
      assert(pthread_join(threads[i], NULL) == 0);
   assert(atomic_load(&refresh.calls) == 1);
   assert(server_mgmt_jwks_cache_install(bundle, bundle_n, envelope, envelope_n, 101) ==
          SERVER_MGMT_JWKS_CACHE_OK);
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_n = 0;
   assert(server_mgmt_jwks_cache_load(bundle, bundle_n, 101, jwks, sizeof(jwks), &jwks_n) ==
          SERVER_MGMT_JWKS_CACHE_OK);
   assert(jwks_n && !strcmp(jwks, expected_jwks));

   char other[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t other_n = 0;
   fixture(90, 201, seed, bundle, &bundle_n, other, &other_n);
   assert(server_mgmt_jwks_cache_install(bundle, bundle_n, other, other_n, 100) ==
          SERVER_MGMT_JWKS_CACHE_CONFLICT);

   db1_shutdown();
   assert(db1_init(db_path) == 0);
   assert(server_mgmt_jwks_cache_load(bundle, bundle_n, 199, jwks, sizeof(jwks), &jwks_n) ==
          SERVER_MGMT_JWKS_CACHE_OK);
   assert(server_mgmt_jwks_cache_load(bundle, bundle_n, 200, jwks, sizeof(jwks), &jwks_n) ==
          SERVER_MGMT_JWKS_CACHE_STALE);
   assert(jwks[0] == '\0' && jwks_n == 0);

   assert(sqlite3_exec(db1_conn(),
                       "UPDATE server_management_jwks_cache SET envelope_sha256=zeroblob(32)", NULL,
                       NULL, NULL) == SQLITE_OK);
   assert(server_mgmt_jwks_cache_load(bundle, bundle_n, 100, jwks, sizeof(jwks), &jwks_n) ==
          SERVER_MGMT_JWKS_CACHE_INVALID);
   db1_shutdown();
   unlink(db_path);
   printf("server management JWKS cache: ok\n");
   return 0;
}
