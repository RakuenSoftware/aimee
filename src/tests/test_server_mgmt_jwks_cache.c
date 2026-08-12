#include "db1.h"
#include "db1/db1_internal.h"
#include "kb/kb_mgmt_jwks_publication.h"
#include "kb/kb_mgmt_token_roots_provision.h"
#include "server/server_mgmt_jwks_cache.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#include <assert.h>
#include <fcntl.h>
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
   /* Test-controlled overlap window; see fetch_fixture. */
   pthread_mutex_t gate;
   pthread_cond_t signal;
   int entered;       /* threads that have reached the refresh */
   int fetch_started; /* the winner is inside the fetch */
   int release;       /* the test has let the fetch finish */
} refresh_ctx_t;

/* The refresh under test coalesces only the callers that arrive WHILE a fetch is
 * in flight: one thread wins, the rest wait on a condition variable and then
 * just load. A caller arriving after the winner finishes legitimately starts a
 * second fetch — that is what "refresh" means, and is not a bug.
 *
 * So `calls == 1` is only true if all 32 threads reach the refresh before the
 * winner's fetch returns. That used to be a bet on a fixed usleep(50000)
 * outlasting 32 pthread_creates, which the test wins on an idle machine and
 * loses under the parallel suite — it failed roughly 1 run in 5 there while
 * passing 240/240 in isolation. Proven by shrinking the sleep to zero, which
 * fails the assertion every time.
 *
 * The window is now held open by the TEST instead of by a clock: the winner's
 * fetch blocks until the test has seen every thread arrive and releases it. */
static int fetch_fixture(void *opaque, char *out, size_t cap, size_t *out_n)
{
   refresh_ctx_t *ctx = opaque;
   atomic_fetch_add(&ctx->calls, 1);

   pthread_mutex_lock(&ctx->gate);
   ctx->fetch_started = 1;
   pthread_cond_broadcast(&ctx->signal);
   while (!ctx->release)
      pthread_cond_wait(&ctx->signal, &ctx->gate);
   pthread_mutex_unlock(&ctx->gate);

   if (ctx->envelope_n + 1 > cap)
      return -1;
   memcpy(out, ctx->envelope, ctx->envelope_n + 1);
   *out_n = ctx->envelope_n;
   return 0;
}

static void *refresh_thread(void *opaque)
{
   refresh_ctx_t *ctx = opaque;
   /* Announce arrival BEFORE entering the refresh, so the test can wait for all
    * of them rather than guessing how long they take to spawn. */
   pthread_mutex_lock(&ctx->gate);
   ctx->entered++;
   pthread_cond_broadcast(&ctx->signal);
   pthread_mutex_unlock(&ctx->gate);

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
   /* Built from TMPDIR, not hardcoded to /tmp. The runner exports TMPDIR and
    * removes it on exit, so a test that hardcodes /tmp escapes that sandbox and
    * leaves one entry behind on every run — the drip that has already put ~40k
    * of them in /tmp on the development box. */
   char db_path[256];
   snprintf(db_path, sizeof db_path, "%s/aimee-management-jwks-cache-XXXXXX", platform_tmpdir());
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
      char trust_path[256];
      snprintf(trust_path, sizeof trust_path, "%s/aimee-management-jwks-trust-XXXXXX",
               platform_tmpdir());
      int trust_fd = mkstemp(trust_path);
      assert(trust_fd >= 0);
      assert(write(trust_fd, bundle, bundle_n) == (ssize_t)bundle_n);
      /* The container runs aimee-server as UID 1000. The bundle is public
       * verification material, so root-owned 0644 is the supported shape: the
       * server can read it but cannot replace or modify it. */
      assert(close(trust_fd) == 0 && chmod(trust_path, 0644) == 0);
      char loaded[SERVER_MGMT_JWKS_BUNDLE_MAX];
      size_t loaded_n = 0;
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) ==
             0);
      assert(loaded_n == bundle_n && !memcmp(loaded, bundle, bundle_n));
      /* The exporter writes one framing newline. A directly redirected export
       * is the documented file shape and must load as the canonical JSON bytes. */
      int append_fd = open(trust_path, O_WRONLY | O_APPEND | O_CLOEXEC);
      assert(append_fd >= 0 && write(append_fd, "\n", 1) == 1 && close(append_fd) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) ==
             0);
      assert(loaded_n == bundle_n && !memcmp(loaded, bundle, bundle_n));
      append_fd = open(trust_path, O_WRONLY | O_APPEND | O_CLOEXEC);
      assert(append_fd >= 0 && write(append_fd, "\n", 1) == 1 && close(append_fd) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) !=
             0);
      assert(truncate(trust_path, (off_t)bundle_n) == 0);
      assert(chmod(trust_path, 0664) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) !=
             0);
      assert(chmod(trust_path, 0600) == 0);
      assert(server_mgmt_jwks_trust_bundle_load(trust_path, loaded, sizeof(loaded), &loaded_n) ==
             0);
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

   refresh_ctx_t refresh = {.bundle = bundle,
                            .bundle_n = bundle_n,
                            .envelope = envelope,
                            .envelope_n = envelope_n,
                            .gate = PTHREAD_MUTEX_INITIALIZER,
                            .signal = PTHREAD_COND_INITIALIZER};
   pthread_t threads[32];
   for (size_t i = 0; i < 32; ++i)
      assert(pthread_create(&threads[i], NULL, refresh_thread, &refresh) == 0);

   /* Wait for every thread to arrive AND for the winner to be inside the fetch.
    * Unbounded on purpose: the fetch is held open, so there is no deadline to
    * race and nothing to tune. */
   pthread_mutex_lock(&refresh.gate);
   while (refresh.entered < 32 || !refresh.fetch_started)
      pthread_cond_wait(&refresh.signal, &refresh.gate);
   pthread_mutex_unlock(&refresh.gate);

   /* Every thread has announced itself; give the 31 losers the few instructions
    * they need to get from that announcement into the refresh's own wait. This
    * is the one remaining timing assumption, and unlike the sleep it replaces it
    * is not competing with a fetch that is about to end — the fetch cannot
    * finish until released below. */
   usleep(50000);

   pthread_mutex_lock(&refresh.gate);
   refresh.release = 1;
   pthread_cond_broadcast(&refresh.signal);
   pthread_mutex_unlock(&refresh.gate);

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
   int64_t current_generation = 0;
   assert(server_mgmt_jwks_cache_current_generation(bundle, bundle_n, 101, &current_generation) ==
          SERVER_MGMT_JWKS_CACHE_OK);
   assert(current_generation == 1);

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
