#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_lifecycle.h"

#include "kb_management_cert_binding.h"
#include "kb_management_cert_codec.h"
#include "kb_management_cert_crypto.h"
#include "kb_management_cert_storage.h"
#include "kb_management_cert_lifecycle_test.h"
#include "modules/db2/c/lifecycle.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RENEWAL_WINDOW_SECONDS 1200
#define DB_RETRY_RESULT        ((kb_management_cert_result_t)99)
#define DB_ACTIVE_RESULT       ((kb_management_cert_result_t)98)

struct kb_management_cert_lifecycle
{
   kb_workload_provider_t *provider;
   kb_workload_provider_kind_t provider_kind;
   char installation_id[33];
   char custodied_ca_dir[PATH_MAX];
   kb_management_cert_storage_t storage;
   pthread_mutex_t mutex;
   int mutex_ready;
   const kb_management_cert_test_ops_t *test_ops;
   void *test_context;
};

typedef struct
{
   kb_workload_identity_t identity;
   db2_management_client_instance_binding_t db;
} live_binding_t;

typedef struct
{
   kb_management_cert_pending_manifest_t pending;
   uint8_t encoded[1024];
   size_t encoded_len;
} pending_record_t;

typedef struct
{
   kb_management_cert_intent_view_t view;
   kb_management_cert_key_material_t key;
   uint8_t record[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t record_len;
} recovered_intent_t;

typedef struct
{
   kb_management_cert_candidate_view_t view;
   kb_management_cert_verified_t verified;
   kb_management_cert_bundle_t bundle;
   uint8_t record[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t record_len;
} recovered_candidate_t;

typedef struct
{
   kb_management_cert_bundle_t *bundle;
   kb_management_cert_active_t *active;
} cancel_output_t;

typedef struct
{
   uint8_t *base;
   size_t size;
   size_t used;
} secret_arena_t;

#define SECRET_ARENA_BYTES (384U * 1024U)

static int secret_arena_open(kb_management_cert_lifecycle_t *lifecycle, secret_arena_t *arena)
{
   memset(arena, 0, sizeof(*arena));
   if (lifecycle->test_ops && lifecycle->test_ops->arena_fail &&
       lifecycle->test_ops->arena_fail(lifecycle->test_context, 1))
      return -1;
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0 || (size_t)page > SIZE_MAX - SECRET_ARENA_BYTES)
      return -1;
   size_t arena_size = ((SECRET_ARENA_BYTES + (size_t)page - 1U) / (size_t)page) * (size_t)page;
   if (arena_size < SECRET_ARENA_BYTES)
      return -1;
   void *mapping =
       mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (mapping == MAP_FAILED)
      return -1;
   arena->base = mapping;
   arena->size = arena_size;
   if ((lifecycle->test_ops && lifecycle->test_ops->arena_fail &&
        lifecycle->test_ops->arena_fail(lifecycle->test_context, 2)) ||
       mlock(mapping, arena->size) != 0)
      goto failed;
#ifdef MADV_DONTDUMP
   if ((lifecycle->test_ops && lifecycle->test_ops->arena_fail &&
        lifecycle->test_ops->arena_fail(lifecycle->test_context, 3)) ||
       madvise(mapping, arena->size, MADV_DONTDUMP) != 0)
      goto failed_locked;
#endif
#ifdef MADV_WIPEONFORK
   if (lifecycle->test_ops && lifecycle->test_ops->arena_fail &&
       lifecycle->test_ops->arena_fail(lifecycle->test_context, 4))
      goto failed_locked;
   if (madvise(mapping, arena->size, MADV_WIPEONFORK) != 0 && errno != EINVAL && errno != ENOSYS)
      goto failed_locked;
#endif
   return 0;

failed_locked:
   OPENSSL_cleanse(mapping, arena->size);
   munlock(mapping, arena->size);
failed:
   munmap(mapping, arena->size);
   memset(arena, 0, sizeof(*arena));
   return -1;
}

static void *secret_arena_alloc(secret_arena_t *arena, size_t bytes)
{
   size_t aligned = (bytes + 15U) & ~(size_t)15U;
   if (!arena || !arena->base || aligned < bytes || aligned > arena->size - arena->used)
      return NULL;
   void *result = arena->base + arena->used;
   arena->used += aligned;
   memset(result, 0, aligned);
   return result;
}

static void secret_arena_close(secret_arena_t *arena)
{
   if (!arena || !arena->base)
      return;
   OPENSSL_cleanse(arena->base, arena->size);
   munlock(arena->base, arena->size);
   munmap(arena->base, arena->size);
   memset(arena, 0, sizeof(*arena));
}

static void cancel_output_clear(void *opaque)
{
   cancel_output_t *output = opaque;
   if (output && output->bundle)
      kb_management_cert_bundle_clear(output->bundle);
   if (output && output->active)
      OPENSSL_cleanse(output->active, sizeof(*output->active));
}

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int absolute_path(const char *path)
{
   if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) >= PATH_MAX)
      return 0;
   const char *p = path + 1;
   while (*p)
   {
      const char *slash = strchr(p, '/');
      size_t n = slash ? (size_t)(slash - p) : strlen(p);
      if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
         return 0;
      if (!slash)
         return 1;
      p = slash + 1;
   }
   return 0;
}

static kb_management_cert_result_t storage_result(kb_management_cert_storage_result_t result)
{
   switch (result)
   {
   case KB_MANAGEMENT_STORAGE_OK:
      return KB_MANAGEMENT_CERT_OK;
   case KB_MANAGEMENT_STORAGE_CONFLICT:
      return KB_MANAGEMENT_CERT_CONFLICT;
   case KB_MANAGEMENT_STORAGE_INTEGRITY:
      return KB_MANAGEMENT_CERT_INTEGRITY;
   default:
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
}

static kb_management_cert_result_t workload_result(kb_workload_result_t result)
{
   switch (result)
   {
   case KB_WORKLOAD_OK:
      return KB_MANAGEMENT_CERT_OK;
   case KB_WORKLOAD_DISABLED:
      return KB_MANAGEMENT_CERT_DISABLED;
   case KB_WORKLOAD_INVALID:
      return KB_MANAGEMENT_CERT_INVALID;
   case KB_WORKLOAD_INTEGRITY:
      return KB_MANAGEMENT_CERT_INTEGRITY;
   default:
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
}

static int random_bytes(uint8_t *, size_t);

static kb_management_cert_result_t db_result(db2_management_client_instance_result_t result)
{
   switch (result)
   {
   case DB2_MANAGEMENT_CLIENT_INSTANCE_OK:
      return KB_MANAGEMENT_CERT_OK;
   case DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID:
      return KB_MANAGEMENT_CERT_INVALID;
   case DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED:
      return KB_MANAGEMENT_CERT_DENIED;
   case DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT:
      return KB_MANAGEMENT_CERT_CONFLICT;
   case DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY:
      return KB_MANAGEMENT_CERT_INTEGRITY;
   case DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY:
   case DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE:
      return DB_RETRY_RESULT;
   default:
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
}

static int64_t lifecycle_now(kb_management_cert_lifecycle_t *lifecycle)
{
   return lifecycle->test_ops ? lifecycle->test_ops->now(lifecycle->test_context)
                              : (int64_t)time(NULL);
}

static int lifecycle_random(kb_management_cert_lifecycle_t *lifecycle, uint8_t *out, size_t len)
{
   return lifecycle->test_ops ? lifecycle->test_ops->random(lifecycle->test_context, out, len)
                              : random_bytes(out, len);
}

static int lifecycle_crash(kb_management_cert_lifecycle_t *lifecycle,
                           kb_management_cert_crash_point_t point)
{
   return lifecycle->test_ops && lifecycle->test_ops->crash
              ? lifecycle->test_ops->crash(lifecycle->test_context, point)
              : 0;
}

static kb_workload_result_t lifecycle_attest(kb_management_cert_lifecycle_t *lifecycle,
                                             const uint8_t challenge[32], const uint8_t binding[32],
                                             kb_workload_identity_t *identity)
{
   return lifecycle->test_ops
              ? lifecycle->test_ops->attest(lifecycle->test_context, challenge, binding, identity)
              : kb_workload_attest(lifecycle->provider, challenge, binding, identity);
}

static kb_workload_result_t lifecycle_wrap(kb_management_cert_lifecycle_t *lifecycle,
                                           const uint8_t challenge[32], const uint8_t binding[32],
                                           const void *plain, size_t plain_len,
                                           kb_workload_identity_t *identity, uint8_t *cipher,
                                           size_t cap, size_t *len)
{
   return lifecycle->test_ops
              ? lifecycle->test_ops->wrap(lifecycle->test_context, challenge, binding, plain,
                                          plain_len, identity, cipher, cap, len)
              : kb_workload_wrap(lifecycle->provider, challenge, binding, plain, plain_len,
                                 identity, cipher, cap, len);
}

static kb_workload_result_t lifecycle_unwrap(kb_management_cert_lifecycle_t *lifecycle,
                                             const uint8_t challenge[32], const uint8_t binding[32],
                                             const void *cipher, size_t cipher_len,
                                             kb_workload_identity_t *identity, uint8_t *plain,
                                             size_t cap, size_t *len)
{
   return lifecycle->test_ops
              ? lifecycle->test_ops->unwrap(lifecycle->test_context, challenge, binding, cipher,
                                            cipher_len, identity, plain, cap, len)
              : kb_workload_unwrap(lifecycle->provider, challenge, binding, cipher, cipher_len,
                                   identity, plain, cap, len);
}

static int random_bytes(uint8_t *out, size_t len)
{
   return out && len <= INT_MAX && RAND_bytes(out, (int)len) == 1 ? 0 : -1;
}

static int random_hex(kb_management_cert_lifecycle_t *lifecycle, char *out, size_t bytes)
{
   static const char digits[] = "0123456789abcdef";
   uint8_t random[32];
   if (!out || !bytes || bytes > sizeof(random) || lifecycle_random(lifecycle, random, bytes))
      return -1;
   for (size_t i = 0; i < bytes; ++i)
   {
      out[2 * i] = digits[random[i] >> 4];
      out[2 * i + 1] = digits[random[i] & 15];
   }
   out[2 * bytes] = 0;
   OPENSSL_cleanse(random, sizeof(random));
   return 0;
}

static int identity_equal(const kb_workload_identity_t *a, const kb_workload_identity_t *b)
{
   return a && b && !strcmp(a->issuer, b->issuer) && !strcmp(a->subject, b->subject) &&
          CRYPTO_memcmp(a->proof_anchor_id, b->proof_anchor_id, 32) == 0 &&
          CRYPTO_memcmp(a->custody_anchor_id, b->custody_anchor_id, 32) == 0;
}

static kb_management_cert_result_t attest(kb_management_cert_lifecycle_t *lifecycle,
                                          live_binding_t *out)
{
   uint8_t challenge[32], binding[32];
   memset(out, 0, sizeof(*out));
   if (lifecycle_random(lifecycle, challenge, sizeof(challenge)) ||
       kb_management_cert_attest_binding(lifecycle->installation_id, lifecycle->provider_kind,
                                         binding))
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   kb_workload_result_t wr = lifecycle_attest(lifecycle, challenge, binding, &out->identity);
   OPENSSL_cleanse(challenge, sizeof(challenge));
   OPENSSL_cleanse(binding, sizeof(binding));
   kb_management_cert_result_t rc = workload_result(wr);
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   rc = db_result(db2_management_client_instance_binding_init(
       out->identity.issuer, out->identity.subject, out->identity.proof_anchor_id,
       out->identity.custody_anchor_id, &out->db));
   if (rc != KB_MANAGEMENT_CERT_OK)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}

static int binding_matches(const live_binding_t *binding, const uint8_t digest[32])
{
   return CRYPTO_memcmp(binding->db.binding_digest, digest, 32) == 0;
}

static void active_public(const db2_management_client_active_t *active,
                          kb_management_cert_active_t *out)
{
   memset(out, 0, sizeof(*out));
   memcpy(out->installation_id, active->installation_id, sizeof(out->installation_id));
   memcpy(out->lineage_id, active->replacement_lineage_id, sizeof(out->lineage_id));
   out->generation = active->generation;
   out->enrollment_id = active->enrollment_id;
   out->not_before_epoch = active->cert_not_before_epoch;
   out->not_after_epoch = active->cert_not_after_epoch;
   out->revocation_generation = active->revocation_generation;
   memcpy(out->issuer, active->cert_issuer, strlen(active->cert_issuer) + 1);
   memcpy(out->serial_norm, active->cert_serial_norm, strlen(active->cert_serial_norm) + 1);
   memcpy(out->fingerprint, active->cert_fingerprint, 32);
   memcpy(out->spki_digest, active->cert_spki_digest, 32);
   memcpy(out->public_bundle_digest, active->public_bundle_digest, 32);
}

static int active_equal(const db2_management_client_active_t *a,
                        const db2_management_client_active_t *b)
{
   return !strcmp(a->installation_id, b->installation_id) &&
          !strcmp(a->replacement_lineage_id, b->replacement_lineage_id) &&
          !strcmp(a->authority_id, b->authority_id) && a->team_id == b->team_id &&
          a->generation == b->generation && a->enrollment_id == b->enrollment_id &&
          !strcmp(a->operation_id, b->operation_id) && a->issue_kind == b->issue_kind &&
          a->issue_state == b->issue_state &&
          CRYPTO_memcmp(a->binding_digest, b->binding_digest, 32) == 0 &&
          CRYPTO_memcmp(a->public_bundle_digest, b->public_bundle_digest, 32) == 0 &&
          !strcmp(a->cert_identity, b->cert_identity) && !strcmp(a->cert_issuer, b->cert_issuer) &&
          !strcmp(a->cert_serial_norm, b->cert_serial_norm) &&
          CRYPTO_memcmp(a->cert_fingerprint, b->cert_fingerprint, 32) == 0 &&
          CRYPTO_memcmp(a->cert_spki_digest, b->cert_spki_digest, 32) == 0 &&
          a->cert_not_before_epoch == b->cert_not_before_epoch &&
          a->cert_not_after_epoch == b->cert_not_after_epoch &&
          a->revocation_generation == b->revocation_generation;
}

static int identity_component_append(char *out, size_t cap, size_t *used, const char *component)
{
   for (const unsigned char *p = (const unsigned char *)component; *p; ++p)
   {
      const char *replacement = NULL;
      size_t replacement_len = 1;
      if (*p == '%')
         replacement = "%25";
      else if (*p == ':')
         replacement = "%3A";
      if (replacement)
         replacement_len = 3;
      if (*used > cap || replacement_len >= cap - *used)
         return -1;
      if (replacement)
         memcpy(out + *used, replacement, replacement_len);
      else
         out[*used] = (char)*p;
      *used += replacement_len;
   }
   return 0;
}

static int active_identity_valid(const db2_management_client_active_t *active)
{
   char expected[sizeof(active->cert_identity)];
   size_t used = sizeof("cert:") - 1;
   memcpy(expected, "cert:", used);
   if (identity_component_append(expected, sizeof(expected), &used, active->cert_issuer) != 0 ||
       used + 1 >= sizeof(expected))
      return 0;
   expected[used++] = ':';
   if (identity_component_append(expected, sizeof(expected), &used, active->cert_serial_norm) != 0)
      return 0;
   expected[used] = '\0';
   return !strcmp(active->cert_identity, expected);
}

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
int kb_management_cert_identity_matches_for_test(const char *issuer, const char *serial,
                                                 const char *identity)
{
   if (!issuer || !serial || !identity ||
       strlen(issuer) > DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX ||
       strlen(serial) > DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX ||
       strlen(identity) > DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX)
      return 0;
   db2_management_client_active_t active = {0};
   memcpy(active.cert_issuer, issuer, strlen(issuer) + 1);
   memcpy(active.cert_serial_norm, serial, strlen(serial) + 1);
   memcpy(active.cert_identity, identity, strlen(identity) + 1);
   return active_identity_valid(&active);
}
#endif

static int candidate_matches_active(const kb_management_cert_candidate_view_t *c,
                                    const db2_management_client_active_t *a)
{
   return a->issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE &&
          a->issue_kind == (a->generation == 1 ? DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL
                                               : DB2_MANAGEMENT_CLIENT_ISSUE_RENEW) &&
          active_identity_valid(a) && !strcmp(c->installation_id, a->installation_id) &&
          !strcmp(c->lineage_id, a->replacement_lineage_id) &&
          !strcmp(c->operation_id, a->operation_id) && !strcmp(c->authority_id, a->authority_id) &&
          c->generation == a->generation &&
          CRYPTO_memcmp(c->binding_digest, a->binding_digest, 32) == 0 &&
          CRYPTO_memcmp(c->public_bundle_digest, a->public_bundle_digest, 32) == 0 &&
          !strcmp(c->issuer, a->cert_issuer) && !strcmp(c->serial_norm, a->cert_serial_norm) &&
          CRYPTO_memcmp(c->fingerprint, a->cert_fingerprint, 32) == 0 &&
          CRYPTO_memcmp(c->spki_digest, a->cert_spki_digest, 32) == 0 &&
          c->not_before_epoch == a->cert_not_before_epoch &&
          c->not_after_epoch == a->cert_not_after_epoch;
}

static void intent_binding_from_view(const kb_management_cert_intent_view_t *view,
                                     const live_binding_t *live,
                                     kb_management_cert_intent_binding_t *out)
{
   memset(out, 0, sizeof(*out));
   memcpy(out->installation_id, view->installation_id, sizeof(out->installation_id));
   memcpy(out->lineage_id, view->lineage_id, sizeof(out->lineage_id));
   memcpy(out->operation_id, view->operation_id, sizeof(out->operation_id));
   memcpy(out->authority_id, view->authority_id, sizeof(out->authority_id));
   memcpy(out->storage_id, view->storage_id, sizeof(out->storage_id));
   out->generation = view->generation;
   out->provider_kind = view->provider_kind;
   memcpy(out->workload_issuer, live->identity.issuer, strlen(live->identity.issuer) + 1);
   memcpy(out->workload_subject, live->identity.subject, strlen(live->identity.subject) + 1);
   memcpy(out->binding_digest, view->binding_digest, 32);
   memcpy(out->proof_anchor, live->identity.proof_anchor_id, 32);
   memcpy(out->custody_anchor, live->identity.custody_anchor_id, 32);
   memcpy(out->csr_digest, view->csr_digest, 32);
   memcpy(out->csr_spki_digest, view->csr_spki_digest, 32);
   memcpy(out->nonce, view->nonce, 32);
}

static int candidate_binding_from_view(const kb_management_cert_candidate_view_t *view,
                                       const live_binding_t *live,
                                       kb_management_cert_candidate_binding_t *out)
{
   memset(out, 0, sizeof(*out));
   kb_management_cert_intent_view_t intent = {0};
   memcpy(intent.installation_id, view->installation_id, sizeof(intent.installation_id));
   memcpy(intent.lineage_id, view->lineage_id, sizeof(intent.lineage_id));
   memcpy(intent.operation_id, view->operation_id, sizeof(intent.operation_id));
   memcpy(intent.authority_id, view->authority_id, sizeof(intent.authority_id));
   memcpy(intent.storage_id, view->storage_id, sizeof(intent.storage_id));
   intent.generation = view->generation;
   intent.provider_kind = view->provider_kind;
   memcpy(intent.nonce, view->nonce, 32);
   memcpy(intent.binding_digest, view->binding_digest, 32);
   memcpy(intent.csr_digest, view->csr_digest, 32);
   memcpy(intent.csr_spki_digest, view->csr_spki_digest, 32);
   intent_binding_from_view(&intent, live, &out->intent);
   memcpy(out->ca_issuer, view->ca_issuer, strlen(view->ca_issuer) + 1);
   memcpy(out->ca_fingerprint, view->ca_fingerprint, 32);
   memcpy(out->leaf_issuer, view->issuer, strlen(view->issuer) + 1);
   memcpy(out->leaf_serial_norm, view->serial_norm, strlen(view->serial_norm) + 1);
   memcpy(out->leaf_fingerprint, view->fingerprint, 32);
   memcpy(out->leaf_spki_digest, view->spki_digest, 32);
   out->not_before_epoch = view->not_before_epoch;
   out->not_after_epoch = view->not_after_epoch;
   memcpy(out->public_bundle_digest, view->public_bundle_digest, 32);
   OPENSSL_cleanse(&intent, sizeof(intent));
   return 0;
}

static kb_management_cert_result_t read_pending(kb_management_cert_lifecycle_t *lifecycle,
                                                pending_record_t *out, int *present)
{
   memset(out, 0, sizeof(*out));
   *present = 0;
   kb_management_cert_storage_result_t sr = kb_management_cert_storage_pending_read(
       &lifecycle->storage, out->encoded, sizeof(out->encoded), &out->encoded_len);
   if (sr == KB_MANAGEMENT_STORAGE_MISSING)
      return KB_MANAGEMENT_CERT_OK;
   kb_management_cert_result_t rc = storage_result(sr);
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   if (kb_management_cert_pending_decode(out->encoded, out->encoded_len, &out->pending))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   *present = 1;
   return KB_MANAGEMENT_CERT_OK;
}

static kb_management_cert_result_t read_current(kb_management_cert_lifecycle_t *lifecycle,
                                                kb_management_cert_manifest_t *out, int *present)
{
   uint8_t encoded[1024];
   size_t len = 0;
   memset(out, 0, sizeof(*out));
   *present = 0;
   kb_management_cert_storage_result_t sr =
       kb_management_cert_storage_current(&lifecycle->storage, encoded, sizeof(encoded), &len);
   if (sr == KB_MANAGEMENT_STORAGE_MISSING)
   {
      OPENSSL_cleanse(encoded, sizeof(encoded));
      return KB_MANAGEMENT_CERT_OK;
   }
   kb_management_cert_result_t rc = storage_result(sr);
   if (rc == KB_MANAGEMENT_CERT_OK && kb_management_cert_manifest_decode(encoded, len, out))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   OPENSSL_cleanse(encoded, sizeof(encoded));
   if (rc == KB_MANAGEMENT_CERT_OK)
      *present = 1;
   return rc;
}

static kb_management_cert_result_t recover_intent(kb_management_cert_lifecycle_t *lifecycle,
                                                  const live_binding_t *live,
                                                  const pending_record_t *pending,
                                                  recovered_intent_t *out, secret_arena_t *arena)
{
   memset(out, 0, sizeof(*out));
   kb_management_cert_storage_result_t sr =
       kb_management_cert_storage_read(&lifecycle->storage, "intent", pending->pending.operation_id,
                                       out->record, sizeof(out->record), &out->record_len);
   kb_management_cert_result_t rc = storage_result(sr);
   if (sr == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   uint8_t record_digest[32];
   if (kb_management_cert_sha256(out->record, out->record_len, record_digest) ||
       CRYPTO_memcmp(record_digest, pending->pending.intent_record_digest, 32) != 0 ||
       kb_management_cert_intent_decode(out->record, out->record_len, &out->view))
   {
      OPENSSL_cleanse(record_digest, sizeof(record_digest));
      return KB_MANAGEMENT_CERT_INTEGRITY;
   }
   OPENSSL_cleanse(record_digest, sizeof(record_digest));
   if (strcmp(out->view.installation_id, lifecycle->installation_id) ||
       strcmp(out->view.installation_id, pending->pending.installation_id) ||
       strcmp(out->view.lineage_id, pending->pending.lineage_id) ||
       strcmp(out->view.operation_id, pending->pending.operation_id) ||
       strcmp(out->view.authority_id, pending->pending.authority_id) ||
       out->view.generation != pending->pending.generation ||
       out->view.provider_kind != lifecycle->provider_kind ||
       CRYPTO_memcmp(out->view.binding_digest, pending->pending.binding_digest, 32) != 0 ||
       !binding_matches(live, out->view.binding_digest))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   kb_management_cert_intent_binding_t transcript;
   intent_binding_from_view(&out->view, live, &transcript);
   uint8_t custody[32], challenge[32];
   uint8_t *plain = secret_arena_alloc(arena, KB_WORKLOAD_UNWRAP_CAP);
   size_t plain_len = 0;
   if (!plain)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (kb_management_cert_intent_binding(&transcript, custody) ||
       CRYPTO_memcmp(custody, out->view.custody_binding_digest, 32) != 0 ||
       lifecycle_random(lifecycle, challenge, sizeof(challenge)))
   {
      OPENSSL_cleanse(&transcript, sizeof(transcript));
      OPENSSL_cleanse(custody, sizeof(custody));
      return KB_MANAGEMENT_CERT_INTEGRITY;
   }
   kb_workload_identity_t identity = {0};
   kb_workload_result_t wr = lifecycle_unwrap(lifecycle, challenge, custody, out->view.ciphertext,
                                              out->view.ciphertext_len, &identity, plain,
                                              KB_WORKLOAD_UNWRAP_CAP, &plain_len);
   rc = workload_result(wr);
   if (rc == KB_MANAGEMENT_CERT_OK && !identity_equal(&identity, &live->identity))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   kb_management_cert_key_intent_view_t decoded = {0};
   if (rc == KB_MANAGEMENT_CERT_OK &&
       (kb_management_cert_key_intent_decode(plain, plain_len, &decoded) ||
        kb_management_cert_key_intent_verify(decoded.key_der, decoded.key_der_len, decoded.csr_der,
                                             decoded.csr_der_len, &out->key) ||
        CRYPTO_memcmp(out->key.csr_digest, out->view.csr_digest, 32) != 0 ||
        CRYPTO_memcmp(out->key.csr_spki_digest, out->view.csr_spki_digest, 32) != 0))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   OPENSSL_cleanse(&transcript, sizeof(transcript));
   OPENSSL_cleanse(custody, sizeof(custody));
   OPENSSL_cleanse(challenge, sizeof(challenge));
   OPENSSL_cleanse(&identity, sizeof(identity));
   OPENSSL_cleanse(plain, KB_WORKLOAD_UNWRAP_CAP);
   if (rc != KB_MANAGEMENT_CERT_OK)
      kb_management_cert_key_material_clear(&out->key);
   return rc;
}

static kb_management_cert_result_t
recover_candidate(kb_management_cert_lifecycle_t *lifecycle, const live_binding_t *live,
                  const char operation[65], recovered_candidate_t *out, secret_arena_t *arena)
{
   memset(out, 0, sizeof(*out));
   kb_management_cert_storage_result_t sr =
       kb_management_cert_storage_read(&lifecycle->storage, "candidate", operation, out->record,
                                       sizeof(out->record), &out->record_len);
   kb_management_cert_result_t rc = storage_result(sr);
   if (sr == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   if (kb_management_cert_candidate_decode(out->record, out->record_len, &out->view) ||
       strcmp(out->view.operation_id, operation) ||
       strcmp(out->view.installation_id, lifecycle->installation_id) ||
       out->view.provider_kind != lifecycle->provider_kind ||
       !binding_matches(live, out->view.binding_digest))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   kb_management_cert_candidate_binding_t transcript;
   candidate_binding_from_view(&out->view, live, &transcript);
   uint8_t custody[32], challenge[32];
   uint8_t *plain = secret_arena_alloc(arena, KB_WORKLOAD_UNWRAP_CAP);
   size_t plain_len = 0;
   if (!plain)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (kb_management_cert_candidate_binding(&transcript, custody) ||
       CRYPTO_memcmp(custody, out->view.custody_binding_digest, 32) != 0 ||
       lifecycle_random(lifecycle, challenge, sizeof(challenge)))
   {
      OPENSSL_cleanse(&transcript, sizeof(transcript));
      OPENSSL_cleanse(custody, sizeof(custody));
      return KB_MANAGEMENT_CERT_INTEGRITY;
   }
   kb_workload_identity_t identity = {0};
   kb_workload_result_t wr = lifecycle_unwrap(lifecycle, challenge, custody, out->view.ciphertext,
                                              out->view.ciphertext_len, &identity, plain,
                                              KB_WORKLOAD_UNWRAP_CAP, &plain_len);
   rc = workload_result(wr);
   if (rc == KB_MANAGEMENT_CERT_OK && !identity_equal(&identity, &live->identity))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc == KB_MANAGEMENT_CERT_OK &&
       (kb_management_cert_bundle_verify(plain, plain_len, &out->verified, &out->bundle) ||
        CRYPTO_memcmp(out->verified.public_bundle_digest, out->view.public_bundle_digest, 32) ||
        strcmp(out->verified.ca_issuer, out->view.ca_issuer) ||
        CRYPTO_memcmp(out->verified.ca_fingerprint, out->view.ca_fingerprint, 32) ||
        strcmp(out->verified.leaf_issuer, out->view.issuer) ||
        strcmp(out->verified.leaf_serial_norm, out->view.serial_norm) ||
        CRYPTO_memcmp(out->verified.leaf_fingerprint, out->view.fingerprint, 32) ||
        CRYPTO_memcmp(out->verified.leaf_spki_digest, out->view.spki_digest, 32) ||
        out->verified.not_before_epoch != out->view.not_before_epoch ||
        out->verified.not_after_epoch != out->view.not_after_epoch))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   OPENSSL_cleanse(&transcript, sizeof(transcript));
   OPENSSL_cleanse(custody, sizeof(custody));
   OPENSSL_cleanse(challenge, sizeof(challenge));
   OPENSSL_cleanse(&identity, sizeof(identity));
   OPENSSL_cleanse(plain, KB_WORKLOAD_UNWRAP_CAP);
   if (rc != KB_MANAGEMENT_CERT_OK)
   {
      OPENSSL_cleanse(&out->verified, sizeof(out->verified));
      kb_management_cert_bundle_clear(&out->bundle);
   }
   return rc;
}

static kb_management_cert_result_t promote_candidate(kb_management_cert_lifecycle_t *lifecycle,
                                                     const recovered_candidate_t *candidate,
                                                     const db2_management_client_active_t *active,
                                                     const pending_record_t *pending)
{
   if (!candidate_matches_active(&candidate->view, active))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   if (pending)
   {
      kb_management_cert_result_t prepare_rc =
          storage_result(kb_management_cert_storage_cleanup_prepare_promotion(&lifecycle->storage));
      if (prepare_rc != KB_MANAGEMENT_CERT_OK)
         return prepare_rc;
   }
   kb_management_cert_manifest_t current = {.generation = active->generation};
   memcpy(current.operation_id, active->operation_id, sizeof(current.operation_id));
   memcpy(current.public_bundle_digest, active->public_bundle_digest, 32);
   uint8_t encoded[1024];
   size_t encoded_len = 0;
   if (kb_management_cert_manifest_encode(&current, encoded, sizeof(encoded), &encoded_len))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   kb_management_cert_result_t rc = storage_result(
       kb_management_cert_storage_promote(&lifecycle->storage, encoded, encoded_len));
   OPENSSL_cleanse(encoded, sizeof(encoded));
   if (rc != KB_MANAGEMENT_CERT_OK || !pending)
      return rc;
   if (lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_PROMOTE))
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (strcmp(pending->pending.operation_id, active->operation_id) ||
       strcmp(pending->pending.installation_id, active->installation_id) ||
       strcmp(pending->pending.lineage_id, active->replacement_lineage_id) ||
       strcmp(pending->pending.authority_id, active->authority_id) ||
       pending->pending.generation != active->generation ||
       CRYPTO_memcmp(pending->pending.binding_digest, active->binding_digest, 32) ||
       ((pending->pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL) !=
        (active->issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL)))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   rc = storage_result(kb_management_cert_storage_pending_clear_exact(
       &lifecycle->storage, pending->encoded, pending->encoded_len));
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   return storage_result(kb_management_cert_storage_cleanup_apply(&lifecycle->storage));
}

static kb_management_cert_result_t
begin_pending(kb_management_cert_lifecycle_t *lifecycle, const live_binding_t *live,
              const pending_record_t *pending, const recovered_intent_t *intent,
              const db2_management_client_active_t *previous, db2_management_client_pending_t *out)
{
   db2_management_client_instance_result_t dr;
   if (pending->pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL)
   {
      db2_management_client_initial_request_t request = {0};
      memcpy(request.operation_id, pending->pending.operation_id, sizeof(request.operation_id));
      memcpy(request.authority_id, pending->pending.authority_id, sizeof(request.authority_id));
      memcpy(request.installation_id, pending->pending.installation_id,
             sizeof(request.installation_id));
      memcpy(request.expected_lineage_id, pending->pending.lineage_id,
             sizeof(request.expected_lineage_id));
      request.binding = live->db;
      memcpy(request.csr_digest, intent->key.csr_digest, 32);
      memcpy(request.csr_spki_digest, intent->key.csr_spki_digest, 32);
      dr = lifecycle->test_ops
               ? lifecycle->test_ops->begin_initial(lifecycle->test_context, &request, out)
               : db2_management_client_instance_begin_initial(&request, out);
      OPENSSL_cleanse(&request, sizeof(request));
   }
   else
   {
      if (!previous)
         return KB_MANAGEMENT_CERT_INTEGRITY;
      db2_management_client_renewal_request_t request = {0};
      memcpy(request.operation_id, pending->pending.operation_id, sizeof(request.operation_id));
      memcpy(request.installation_id, pending->pending.installation_id,
             sizeof(request.installation_id));
      request.binding = live->db;
      request.generation = pending->pending.generation;
      request.previous_enrollment_id = previous->enrollment_id;
      memcpy(request.previous_cert_issuer, previous->cert_issuer,
             strlen(previous->cert_issuer) + 1);
      memcpy(request.previous_cert_serial_norm, previous->cert_serial_norm,
             strlen(previous->cert_serial_norm) + 1);
      memcpy(request.previous_cert_fingerprint, previous->cert_fingerprint, 32);
      memcpy(request.csr_digest, intent->key.csr_digest, 32);
      memcpy(request.csr_spki_digest, intent->key.csr_spki_digest, 32);
      dr = lifecycle->test_ops
               ? lifecycle->test_ops->begin_renewal(lifecycle->test_context, &request, out)
               : db2_management_client_instance_begin_renewal(&request, out);
      OPENSSL_cleanse(&request, sizeof(request));
   }
   kb_management_cert_result_t rc = db_result(dr);
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   if (strcmp(out->installation_id, pending->pending.installation_id) ||
       strcmp(out->replacement_lineage_id, pending->pending.lineage_id) ||
       strcmp(out->authority_id, pending->pending.authority_id) ||
       strcmp(out->operation_id, pending->pending.operation_id) ||
       out->generation != pending->pending.generation ||
       CRYPTO_memcmp(out->binding_digest, pending->pending.binding_digest, 32) ||
       CRYPTO_memcmp(out->csr_digest, intent->key.csr_digest, 32) ||
       CRYPTO_memcmp(out->csr_spki_digest, intent->key.csr_spki_digest, 32) ||
       ((out->issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL) !=
        (pending->pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL)))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   if (out->has_previous != (pending->pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_RENEWAL))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   if (out->has_previous &&
       (!previous || out->previous_enrollment_id != previous->enrollment_id ||
        strcmp(out->previous_cert_issuer, previous->cert_issuer) ||
        strcmp(out->previous_cert_serial_norm, previous->cert_serial_norm) ||
        CRYPTO_memcmp(out->previous_cert_fingerprint, previous->cert_fingerprint, 32)))
      return KB_MANAGEMENT_CERT_INTEGRITY;
   if (out->issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE)
      return DB_ACTIVE_RESULT;
   if (out->issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_EXPIRED ||
       out->issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_QUARANTINED)
      return KB_MANAGEMENT_CERT_DENIED;
   if (out->issue_state != DB2_MANAGEMENT_CLIENT_ISSUE_PENDING)
      return KB_MANAGEMENT_CERT_INTEGRITY;
   return KB_MANAGEMENT_CERT_OK;
}

static kb_management_cert_result_t
issue_candidate(kb_management_cert_lifecycle_t *lifecycle, const live_binding_t *live,
                const pending_record_t *pending, const recovered_intent_t *intent,
                const db2_management_client_pending_t *begun, recovered_candidate_t *out,
                secret_arena_t *arena, int64_t deadline_epoch)
{
   memset(out, 0, sizeof(*out));
   kb_pki_ca_t *ca = secret_arena_alloc(arena, sizeof(*ca));
   char *leaf = secret_arena_alloc(arena, KB_PKI_CERT_PEM_MAX);
   char *ca_cert = secret_arena_alloc(arena, KB_PKI_CERT_PEM_MAX);
   kb_management_cert_verified_t *verified = secret_arena_alloc(arena, sizeof(*verified));
   uint8_t *plain = secret_arena_alloc(arena, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
   uint8_t *cipher = secret_arena_alloc(arena, KB_WORKLOAD_WRAP_CAP);
   size_t plain_len = 0;
   if (!ca || !leaf || !ca_cert || !verified || !plain || !cipher)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (lifecycle_now(lifecycle) >= deadline_epoch)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   kb_pki_ca_load_result_t ca_rc = kb_pki_ca_load_custodied_ex(lifecycle->custodied_ca_dir, ca);
   if (ca_rc != KB_PKI_CA_LOAD_OK)
   {
      OPENSSL_cleanse(ca, sizeof(*ca));
      OPENSSL_cleanse(leaf, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(ca_cert, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(verified, sizeof(*verified));
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
      return ca_rc == KB_PKI_CA_LOAD_INTEGRITY ? KB_MANAGEMENT_CERT_INTEGRITY
                                               : KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   memcpy(ca_cert, ca->cert_pem, strlen(ca->cert_pem) + 1);
   int sign_rc =
       kb_pki_sign_kb_management_csr(ca, intent->key.csr_pem, 3600, leaf, KB_PKI_CERT_PEM_MAX);
   OPENSSL_cleanse(ca, sizeof(*ca));
   if (lifecycle_now(lifecycle) >= deadline_epoch)
   {
      OPENSSL_cleanse(leaf, KB_PKI_CERT_PEM_MAX);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   if (sign_rc)
   {
      OPENSSL_cleanse(leaf, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(ca_cert, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(verified, sizeof(*verified));
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   if (kb_management_cert_leaf_verify(&intent->key, leaf, ca_cert, verified) ||
       kb_management_cert_bundle_encode(intent->key.key_der, intent->key.key_der_len,
                                        verified->leaf_der, verified->leaf_der_len,
                                        verified->ca_der, verified->ca_der_len, plain,
                                        KB_MANAGEMENT_CERT_PLAINTEXT_MAX, &plain_len) ||
       kb_management_cert_sha256(plain, plain_len, verified->public_bundle_digest))
   {
      OPENSSL_cleanse(leaf, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(ca_cert, KB_PKI_CERT_PEM_MAX);
      OPENSSL_cleanse(verified, sizeof(*verified));
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
      return KB_MANAGEMENT_CERT_INTEGRITY;
   }
   OPENSSL_cleanse(leaf, KB_PKI_CERT_PEM_MAX);
   OPENSSL_cleanse(ca_cert, KB_PKI_CERT_PEM_MAX);

   kb_management_cert_candidate_view_t candidate = {0};
   memcpy(candidate.installation_id, pending->pending.installation_id, 33);
   memcpy(candidate.lineage_id, pending->pending.lineage_id, 33);
   memcpy(candidate.operation_id, pending->pending.operation_id, 65);
   memcpy(candidate.authority_id, pending->pending.authority_id, 33);
   candidate.generation = pending->pending.generation;
   candidate.provider_kind = lifecycle->provider_kind;
   memcpy(candidate.binding_digest, pending->pending.binding_digest, 32);
   memcpy(candidate.csr_digest, intent->key.csr_digest, 32);
   memcpy(candidate.csr_spki_digest, intent->key.csr_spki_digest, 32);
   memcpy(candidate.public_bundle_digest, verified->public_bundle_digest, 32);
   memcpy(candidate.ca_issuer, verified->ca_issuer, strlen(verified->ca_issuer) + 1);
   memcpy(candidate.ca_fingerprint, verified->ca_fingerprint, 32);
   memcpy(candidate.issuer, verified->leaf_issuer, strlen(verified->leaf_issuer) + 1);
   memcpy(candidate.serial_norm, verified->leaf_serial_norm,
          strlen(verified->leaf_serial_norm) + 1);
   memcpy(candidate.fingerprint, verified->leaf_fingerprint, 32);
   memcpy(candidate.spki_digest, verified->leaf_spki_digest, 32);
   candidate.not_before_epoch = verified->not_before_epoch;
   candidate.not_after_epoch = verified->not_after_epoch;
   if (random_hex(lifecycle, candidate.storage_id, 16) ||
       lifecycle_random(lifecycle, candidate.nonce, 32))
   {
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(verified, sizeof(*verified));
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   kb_management_cert_candidate_binding_t transcript;
   candidate_binding_from_view(&candidate, live, &transcript);
   uint8_t custody[32], challenge[32];
   size_t cipher_len = 0;
   if (kb_management_cert_candidate_binding(&transcript, custody) ||
       lifecycle_random(lifecycle, challenge, sizeof(challenge)))
   {
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(verified, sizeof(*verified));
      OPENSSL_cleanse(&transcript, sizeof(transcript));
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   kb_workload_identity_t wrapped_identity = {0};
   if (lifecycle_now(lifecycle) >= deadline_epoch)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   kb_workload_result_t wr =
       lifecycle_wrap(lifecycle, challenge, custody, plain, plain_len, &wrapped_identity, cipher,
                      KB_WORKLOAD_WRAP_CAP, &cipher_len);
   kb_management_cert_result_t rc = workload_result(wr);
   if (rc == KB_MANAGEMENT_CERT_OK && !identity_equal(&wrapped_identity, &live->identity))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      memcpy(candidate.custody_binding_digest, custody, 32);
      candidate.ciphertext = cipher;
      candidate.ciphertext_len = cipher_len;
      if (kb_management_cert_candidate_encode(&candidate, out->record, sizeof(out->record),
                                              &out->record_len))
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
   }
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      if (lifecycle_now(lifecycle) >= deadline_epoch)
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   if (rc == KB_MANAGEMENT_CERT_OK)
      rc = storage_result(kb_management_cert_storage_stage(
          &lifecycle->storage, "candidate", candidate.operation_id, out->record, out->record_len));
   if (rc == KB_MANAGEMENT_CERT_OK &&
       lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_CANDIDATE))
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      if (kb_management_cert_candidate_decode(out->record, out->record_len, &out->view))
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
      else
      {
         out->verified = *verified;
         if (kb_management_cert_bundle_to_pem(intent->key.key_der, intent->key.key_der_len,
                                              verified->leaf_der, verified->leaf_der_len,
                                              verified->ca_der, verified->ca_der_len, &out->bundle))
            rc = KB_MANAGEMENT_CERT_INTEGRITY;
      }
   }
   (void)begun;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(verified, sizeof(*verified));
   OPENSSL_cleanse(&transcript, sizeof(transcript));
   OPENSSL_cleanse(custody, sizeof(custody));
   OPENSSL_cleanse(challenge, sizeof(challenge));
   OPENSSL_cleanse(&wrapped_identity, sizeof(wrapped_identity));
   OPENSSL_cleanse(cipher, KB_WORKLOAD_WRAP_CAP);
   OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
   return rc;
}

static kb_management_cert_result_t
activate_candidate(kb_management_cert_lifecycle_t *lifecycle, const live_binding_t *live,
                   const pending_record_t *pending, const db2_management_client_pending_t *begun,
                   const recovered_candidate_t *candidate, db2_management_client_active_t *out)
{
   db2_management_client_activation_request_t request = {0};
   memcpy(request.operation_id, candidate->view.operation_id, sizeof(request.operation_id));
   memcpy(request.installation_id, candidate->view.installation_id,
          sizeof(request.installation_id));
   request.binding = live->db;
   request.issue_kind = pending->pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL
                            ? DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL
                            : DB2_MANAGEMENT_CLIENT_ISSUE_RENEW;
   request.generation = candidate->view.generation;
   request.has_previous = begun->has_previous;
   request.previous_enrollment_id = begun->previous_enrollment_id;
   memcpy(request.previous_cert_issuer, begun->previous_cert_issuer,
          strlen(begun->previous_cert_issuer) + 1);
   memcpy(request.previous_cert_serial_norm, begun->previous_cert_serial_norm,
          strlen(begun->previous_cert_serial_norm) + 1);
   memcpy(request.previous_cert_fingerprint, begun->previous_cert_fingerprint, 32);
   memcpy(request.csr_digest, candidate->view.csr_digest, 32);
   memcpy(request.csr_spki_digest, candidate->view.csr_spki_digest, 32);
   memcpy(request.public_bundle_digest, candidate->view.public_bundle_digest, 32);
   memcpy(request.verified_ca_issuer, candidate->view.ca_issuer,
          strlen(candidate->view.ca_issuer) + 1);
   memcpy(request.verified_ca_fingerprint, candidate->view.ca_fingerprint, 32);
   memcpy(request.leaf_issuer, candidate->view.issuer, strlen(candidate->view.issuer) + 1);
   memcpy(request.leaf_serial_norm, candidate->view.serial_norm,
          strlen(candidate->view.serial_norm) + 1);
   memcpy(request.leaf_fingerprint, candidate->view.fingerprint, 32);
   memcpy(request.leaf_spki_digest, candidate->view.spki_digest, 32);
   request.leaf_not_before_epoch = candidate->view.not_before_epoch;
   request.leaf_not_after_epoch = candidate->view.not_after_epoch;
   db2_management_client_instance_result_t dr =
       lifecycle->test_ops ? lifecycle->test_ops->activate(lifecycle->test_context, &request, out)
                           : db2_management_client_instance_activate(&request, out);
   kb_management_cert_result_t rc = db_result(dr);
   OPENSSL_cleanse(&request, sizeof(request));
   if (rc == KB_MANAGEMENT_CERT_OK && !candidate_matches_active(&candidate->view, out))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   return rc;
}

static kb_management_cert_result_t
create_pending(kb_management_cert_lifecycle_t *lifecycle, const live_binding_t *live,
               kb_management_cert_issue_kind_t kind, const db2_management_client_active_t *active,
               pending_record_t *pending, recovered_intent_t *intent, secret_arena_t *arena,
               int64_t deadline_epoch)
{
   memset(pending, 0, sizeof(*pending));
   memset(intent, 0, sizeof(*intent));
   db2_management_client_grant_preflight_t preflight = {0};
   kb_management_cert_intent_view_t view = {0};
   kb_management_cert_intent_binding_t transcript = {0};
   uint8_t custody[32] = {0}, challenge[32] = {0};
   uint8_t *plain = secret_arena_alloc(arena, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
   uint8_t *cipher = secret_arena_alloc(arena, KB_WORKLOAD_WRAP_CAP);
   kb_workload_identity_t wrapped_identity = {0};
   size_t plain_len = 0, cipher_len = 0;
   kb_management_cert_result_t rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (!plain || !cipher)
      goto cleanup;
   if (lifecycle_now(lifecycle) >= deadline_epoch)
      goto cleanup;
   if (kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL)
   {
      db2_management_client_grant_preflight_request_t request = {0};
      memcpy(request.installation_id, lifecycle->installation_id, sizeof(request.installation_id));
      request.binding = live->db;
      db2_management_client_instance_result_t dr =
          lifecycle->test_ops
              ? lifecycle->test_ops->preflight(lifecycle->test_context, &request, &preflight)
              : db2_management_client_instance_grant_preflight(&request, &preflight);
      rc = db_result(dr);
      OPENSSL_cleanse(&request, sizeof(request));
      if (rc != KB_MANAGEMENT_CERT_OK)
         goto cleanup;
      if (lifecycle_now(lifecycle) >= deadline_epoch)
         goto cleanup;
      memcpy(pending->pending.lineage_id, preflight.replacement_lineage_id, 33);
      if (random_hex(lifecycle, pending->pending.authority_id, 16))
         goto cleanup;
   }
   else
   {
      if (!active)
      {
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
         goto cleanup;
      }
      memcpy(pending->pending.lineage_id, active->replacement_lineage_id, 33);
      memcpy(pending->pending.authority_id, active->authority_id, 33);
   }
   memcpy(pending->pending.installation_id, lifecycle->installation_id, 33);
   pending->pending.issue_kind = kind;
   if (kind == KB_MANAGEMENT_CERT_ISSUE_RENEWAL && active->generation == INT64_MAX)
   {
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
      goto cleanup;
   }
   pending->pending.generation =
       kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL ? 1 : active->generation + 1;
   memcpy(pending->pending.binding_digest, live->db.binding_digest, 32);
   if (random_hex(lifecycle, pending->pending.operation_id, 32) ||
       kb_management_cert_key_generate(&intent->key))
      goto cleanup;
   if (lifecycle_now(lifecycle) >= deadline_epoch)
      goto cleanup;

   memcpy(view.installation_id, pending->pending.installation_id, 33);
   memcpy(view.lineage_id, pending->pending.lineage_id, 33);
   memcpy(view.operation_id, pending->pending.operation_id, 65);
   memcpy(view.authority_id, pending->pending.authority_id, 33);
   view.generation = pending->pending.generation;
   view.provider_kind = lifecycle->provider_kind;
   memcpy(view.binding_digest, live->db.binding_digest, 32);
   memcpy(view.csr_digest, intent->key.csr_digest, 32);
   memcpy(view.csr_spki_digest, intent->key.csr_spki_digest, 32);
   if (random_hex(lifecycle, view.storage_id, 16) || lifecycle_random(lifecycle, view.nonce, 32))
      goto cleanup;
   intent_binding_from_view(&view, live, &transcript);
   if (kb_management_cert_intent_binding(&transcript, custody) ||
       kb_management_cert_key_intent_encode(intent->key.key_der, intent->key.key_der_len,
                                            intent->key.csr_der, intent->key.csr_der_len, plain,
                                            KB_MANAGEMENT_CERT_PLAINTEXT_MAX, &plain_len) ||
       lifecycle_random(lifecycle, challenge, 32))
      goto cleanup;
   kb_workload_result_t wr =
       lifecycle_wrap(lifecycle, challenge, custody, plain, plain_len, &wrapped_identity, cipher,
                      KB_WORKLOAD_WRAP_CAP, &cipher_len);
   rc = workload_result(wr);
   if (rc == KB_MANAGEMENT_CERT_OK && !identity_equal(&wrapped_identity, &live->identity))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      memcpy(view.custody_binding_digest, custody, 32);
      view.ciphertext = cipher;
      view.ciphertext_len = cipher_len;
      if (kb_management_cert_intent_encode(&view, intent->record, sizeof(intent->record),
                                           &intent->record_len))
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
   }
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      if (lifecycle_now(lifecycle) >= deadline_epoch)
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   if (rc == KB_MANAGEMENT_CERT_OK &&
       (kb_management_cert_sha256(intent->record, intent->record_len,
                                  pending->pending.intent_record_digest) ||
        kb_management_cert_pending_encode(&pending->pending, pending->encoded,
                                          sizeof(pending->encoded), &pending->encoded_len)))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc == KB_MANAGEMENT_CERT_OK)
      rc = storage_result(kb_management_cert_storage_cleanup_prepare_intent(
          &lifecycle->storage, pending->encoded, pending->encoded_len));
   if (rc == KB_MANAGEMENT_CERT_OK &&
       lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_PREPARE))
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (rc == KB_MANAGEMENT_CERT_OK)
      rc = storage_result(kb_management_cert_storage_stage(
          &lifecycle->storage, "intent", view.operation_id, intent->record, intent->record_len));
   if (rc == KB_MANAGEMENT_CERT_OK &&
       lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_INTENT))
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (rc == KB_MANAGEMENT_CERT_OK)
      rc = storage_result(kb_management_cert_storage_pending_publish(
          &lifecycle->storage, pending->encoded, pending->encoded_len));
   if (rc == KB_MANAGEMENT_CERT_OK &&
       lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_PENDING))
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (rc == KB_MANAGEMENT_CERT_OK)
      rc = storage_result(kb_management_cert_storage_cleanup_finish_intent(
          &lifecycle->storage, pending->encoded, pending->encoded_len));
   if (rc == KB_MANAGEMENT_CERT_OK &&
       kb_management_cert_intent_decode(intent->record, intent->record_len, &intent->view))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
cleanup:
   OPENSSL_cleanse(&preflight, sizeof(preflight));
   OPENSSL_cleanse(&view, sizeof(view));
   OPENSSL_cleanse(&transcript, sizeof(transcript));
   OPENSSL_cleanse(custody, sizeof(custody));
   OPENSSL_cleanse(challenge, sizeof(challenge));
   if (plain)
      OPENSSL_cleanse(plain, KB_MANAGEMENT_CERT_PLAINTEXT_MAX);
   if (cipher)
      OPENSSL_cleanse(cipher, KB_WORKLOAD_WRAP_CAP);
   OPENSSL_cleanse(&wrapped_identity, sizeof(wrapped_identity));
   return rc;
}

static kb_management_cert_result_t reconcile_once(kb_management_cert_lifecycle_t *lifecycle,
                                                  int64_t deadline_epoch,
                                                  kb_management_cert_active_t *out,
                                                  secret_arena_t *arena)
{
   if (lifecycle_now(lifecycle) >= deadline_epoch)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   live_binding_t live = {0};
   recovered_intent_t *intent = NULL;
   recovered_candidate_t *candidate = NULL;
   kb_management_cert_result_t rc = attest(lifecycle, &live);
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   pending_record_t pending = {0};
   int has_pending = 0;
   rc = read_pending(lifecycle, &pending, &has_pending);
   if (rc != KB_MANAGEMENT_CERT_OK)
      goto done;
   kb_management_cert_manifest_t current = {0};
   int has_current = 0;
   rc = read_current(lifecycle, &current, &has_current);
   if (rc != KB_MANAGEMENT_CERT_OK)
      goto done;
   if (!has_pending)
   {
      kb_management_cert_storage_result_t cleanup_sr =
          kb_management_cert_storage_cleanup_apply(&lifecycle->storage);
      if (cleanup_sr != KB_MANAGEMENT_STORAGE_OK && cleanup_sr != KB_MANAGEMENT_STORAGE_MISSING)
      {
         rc = storage_result(cleanup_sr);
         goto done;
      }
   }
   else
   {
      kb_management_cert_storage_result_t finish_sr =
          kb_management_cert_storage_cleanup_finish_intent(&lifecycle->storage, pending.encoded,
                                                           pending.encoded_len);
      if (finish_sr != KB_MANAGEMENT_STORAGE_OK && finish_sr != KB_MANAGEMENT_STORAGE_MISSING)
      {
         rc = storage_result(finish_sr);
         goto done;
      }
   }
   db2_management_client_active_t active = {0};
   intent = secret_arena_alloc(arena, sizeof(*intent));
   candidate = secret_arena_alloc(arena, sizeof(*candidate));
   if (!intent || !candidate)
   {
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      goto done;
   }
   db2_management_client_instance_result_t snapshot_dr =
       lifecycle->test_ops
           ? lifecycle->test_ops->snapshot(lifecycle->test_context, lifecycle->installation_id,
                                           &live.db, &active)
           : db2_management_client_instance_snapshot(lifecycle->installation_id, &live.db, &active);
   kb_management_cert_result_t snapshot_rc = db_result(snapshot_dr);
   if (snapshot_rc != KB_MANAGEMENT_CERT_OK && snapshot_rc != KB_MANAGEMENT_CERT_DENIED)
   {
      rc = snapshot_rc;
      goto done;
   }
   if (snapshot_rc == KB_MANAGEMENT_CERT_OK &&
       (!binding_matches(&live, active.binding_digest) ||
        strcmp(active.installation_id, lifecycle->installation_id)))
   {
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
      goto done;
   }

   if (snapshot_rc == KB_MANAGEMENT_CERT_OK && has_current && !has_pending &&
       !strcmp(current.operation_id, active.operation_id) &&
       current.generation == active.generation &&
       CRYPTO_memcmp(current.public_bundle_digest, active.public_bundle_digest, 32) == 0)
   {
      rc = recover_candidate(lifecycle, &live, active.operation_id, candidate, arena);
      if (rc == KB_MANAGEMENT_CERT_OK && candidate_matches_active(&candidate->view, &active))
      {
         int64_t now = lifecycle_now(lifecycle);
         if (active.cert_not_after_epoch - now > RENEWAL_WINDOW_SECONDS)
            active_public(&active, out);
         else
            rc = create_pending(lifecycle, &live, KB_MANAGEMENT_CERT_ISSUE_RENEWAL, &active,
                                &pending, intent, arena, deadline_epoch);
      }
      kb_management_cert_bundle_clear(&candidate->bundle);
      OPENSSL_cleanse(candidate, sizeof(*candidate));
      if (rc != KB_MANAGEMENT_CERT_OK || out->generation)
         goto done;
      has_pending = 1;
   }

   if (!has_pending)
   {
      if (snapshot_rc == KB_MANAGEMENT_CERT_OK)
      {
         rc = recover_candidate(lifecycle, &live, active.operation_id, candidate, arena);
         if (rc == KB_MANAGEMENT_CERT_OK)
            rc = promote_candidate(lifecycle, candidate, &active, NULL);
         if (rc == KB_MANAGEMENT_CERT_OK)
            active_public(&active, out);
         kb_management_cert_bundle_clear(&candidate->bundle);
         OPENSSL_cleanse(candidate, sizeof(*candidate));
         goto done;
      }
      if (has_current)
      {
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
         goto done;
      }
      rc = create_pending(lifecycle, &live, KB_MANAGEMENT_CERT_ISSUE_INITIAL, NULL, &pending,
                          intent, arena, deadline_epoch);
      if (rc != KB_MANAGEMENT_CERT_OK)
         goto done_intent;
      has_pending = 1;
   }
   else
   {
      if (strcmp(pending.pending.installation_id, lifecycle->installation_id) ||
          !binding_matches(&live, pending.pending.binding_digest))
      {
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
         goto done_intent;
      }
      rc = recover_intent(lifecycle, &live, &pending, intent, arena);
      if (rc != KB_MANAGEMENT_CERT_OK)
         goto done_intent;
   }

   if (snapshot_rc == KB_MANAGEMENT_CERT_OK &&
       !strcmp(active.operation_id, pending.pending.operation_id))
   {
      rc = recover_candidate(lifecycle, &live, active.operation_id, candidate, arena);
      if (rc == KB_MANAGEMENT_CERT_OK)
         rc = promote_candidate(lifecycle, candidate, &active, &pending);
      if (rc == KB_MANAGEMENT_CERT_OK)
         active_public(&active, out);
      kb_management_cert_bundle_clear(&candidate->bundle);
      OPENSSL_cleanse(candidate, sizeof(*candidate));
      goto done_intent;
   }

   db2_management_client_active_t *previous = snapshot_rc == KB_MANAGEMENT_CERT_OK ? &active : NULL;
   if (pending.pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL && previous)
   {
      rc = KB_MANAGEMENT_CERT_CONFLICT;
      goto done_intent;
   }
   if (pending.pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_RENEWAL &&
       (!previous || previous->generation == INT64_MAX ||
        previous->generation + 1 != pending.pending.generation ||
        strcmp(previous->replacement_lineage_id, pending.pending.lineage_id) ||
        strcmp(previous->authority_id, pending.pending.authority_id)))
   {
      rc = previous ? KB_MANAGEMENT_CERT_INTEGRITY : KB_MANAGEMENT_CERT_DENIED;
      goto done_intent;
   }
   db2_management_client_pending_t begun = {0};
   if (lifecycle_now(lifecycle) >= deadline_epoch)
   {
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      goto done_intent;
   }
   rc = begin_pending(lifecycle, &live, &pending, intent, previous, &begun);
   if (rc == DB_ACTIVE_RESULT)
   {
      db2_management_client_active_t activated = {0};
      snapshot_dr = lifecycle->test_ops ? lifecycle->test_ops->snapshot(lifecycle->test_context,
                                                                        lifecycle->installation_id,
                                                                        &live.db, &activated)
                                        : db2_management_client_instance_snapshot(
                                              lifecycle->installation_id, &live.db, &activated);
      rc = db_result(snapshot_dr);
      if (rc == KB_MANAGEMENT_CERT_OK &&
          strcmp(activated.operation_id, pending.pending.operation_id))
         rc = KB_MANAGEMENT_CERT_CONFLICT;
      if (rc == KB_MANAGEMENT_CERT_OK)
         rc = recover_candidate(lifecycle, &live, activated.operation_id, candidate, arena);
      if (rc == KB_MANAGEMENT_CERT_OK)
         rc = promote_candidate(lifecycle, candidate, &activated, &pending);
      if (rc == KB_MANAGEMENT_CERT_OK)
         active_public(&activated, out);
      kb_management_cert_bundle_clear(&candidate->bundle);
      OPENSSL_cleanse(candidate, sizeof(*candidate));
      OPENSSL_cleanse(&activated, sizeof(activated));
      goto done_intent;
   }
   if (rc == KB_MANAGEMENT_CERT_DENIED &&
       (begun.issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_EXPIRED ||
        begun.issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_QUARANTINED))
   {
      db2_management_client_active_t terminal_snapshot = {0};
      snapshot_dr =
          lifecycle->test_ops
              ? lifecycle->test_ops->snapshot(lifecycle->test_context, lifecycle->installation_id,
                                              &live.db, &terminal_snapshot)
              : db2_management_client_instance_snapshot(lifecycle->installation_id, &live.db,
                                                        &terminal_snapshot);
      kb_management_cert_result_t terminal_rc = db_result(snapshot_dr);
      int proven =
          pending.pending.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL
              ? terminal_rc == KB_MANAGEMENT_CERT_DENIED
              : terminal_rc == KB_MANAGEMENT_CERT_OK && previous &&
                    active_equal(previous, &terminal_snapshot) &&
                    strcmp(terminal_snapshot.operation_id, pending.pending.operation_id) != 0;
      if (terminal_rc == DB_RETRY_RESULT)
         rc = DB_RETRY_RESULT;
      else if (!proven)
         rc = terminal_rc == KB_MANAGEMENT_CERT_UNAVAILABLE ? KB_MANAGEMENT_CERT_UNAVAILABLE
                                                            : KB_MANAGEMENT_CERT_INTEGRITY;
      else
      {
         kb_management_cert_result_t prepare_rc = storage_result(
             kb_management_cert_storage_cleanup_prepare_terminal(&lifecycle->storage));
         if (prepare_rc != KB_MANAGEMENT_CERT_OK)
         {
            rc = prepare_rc;
            OPENSSL_cleanse(&terminal_snapshot, sizeof(terminal_snapshot));
            goto done_intent;
         }
         if (lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_BEFORE_TERMINAL_CLEAR))
         {
            rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
            OPENSSL_cleanse(&terminal_snapshot, sizeof(terminal_snapshot));
            goto done_intent;
         }
         kb_management_cert_result_t clear_rc =
             storage_result(kb_management_cert_storage_pending_clear_exact(
                 &lifecycle->storage, pending.encoded, pending.encoded_len));
         if (clear_rc == KB_MANAGEMENT_CERT_OK)
            clear_rc =
                storage_result(kb_management_cert_storage_cleanup_apply(&lifecycle->storage));
         rc = clear_rc == KB_MANAGEMENT_CERT_OK ? KB_MANAGEMENT_CERT_DENIED : clear_rc;
      }
      OPENSSL_cleanse(&terminal_snapshot, sizeof(terminal_snapshot));
      goto done_intent;
   }
   if (rc != KB_MANAGEMENT_CERT_OK)
      goto done_intent;
   if (lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_BEGIN))
   {
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      goto done_intent;
   }
   kb_management_cert_storage_result_t candidate_sr = kb_management_cert_storage_read(
       &lifecycle->storage, "candidate", pending.pending.operation_id, candidate->record,
       sizeof(candidate->record), &candidate->record_len);
   if (candidate_sr == KB_MANAGEMENT_STORAGE_MISSING)
      rc = issue_candidate(lifecycle, &live, &pending, intent, &begun, candidate, arena,
                           deadline_epoch);
   else if (candidate_sr == KB_MANAGEMENT_STORAGE_OK)
      rc = recover_candidate(lifecycle, &live, pending.pending.operation_id, candidate, arena);
   else
      rc = storage_result(candidate_sr);
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      db2_management_client_active_t activated = {0};
      if (lifecycle_now(lifecycle) >= deadline_epoch)
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      if (rc != KB_MANAGEMENT_CERT_OK)
      {
         OPENSSL_cleanse(&activated, sizeof(activated));
         goto candidate_done;
      }
      rc =
          storage_result(kb_management_cert_storage_cleanup_prepare_promotion(&lifecycle->storage));
      if (rc != KB_MANAGEMENT_CERT_OK)
      {
         OPENSSL_cleanse(&activated, sizeof(activated));
         goto candidate_done;
      }
      rc = activate_candidate(lifecycle, &live, &pending, &begun, candidate, &activated);
      if (rc == KB_MANAGEMENT_CERT_OK &&
          lifecycle_crash(lifecycle, KB_MANAGEMENT_CERT_CRASH_AFTER_ACTIVATE))
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      if (rc == KB_MANAGEMENT_CERT_OK)
         rc = promote_candidate(lifecycle, candidate, &activated, &pending);
      if (rc == KB_MANAGEMENT_CERT_OK)
         active_public(&activated, out);
      OPENSSL_cleanse(&activated, sizeof(activated));
   }
candidate_done:
   kb_management_cert_bundle_clear(&candidate->bundle);
   OPENSSL_cleanse(candidate, sizeof(*candidate));
   OPENSSL_cleanse(&begun, sizeof(begun));

done_intent:
done:
   if (intent)
   {
      kb_management_cert_key_material_clear(&intent->key);
      OPENSSL_cleanse(intent, sizeof(*intent));
   }
   if (candidate)
   {
      kb_management_cert_bundle_clear(&candidate->bundle);
      OPENSSL_cleanse(candidate, sizeof(*candidate));
   }
   OPENSSL_cleanse(&live, sizeof(live));
   OPENSSL_cleanse(&pending, sizeof(pending));
   OPENSSL_cleanse(&current, sizeof(current));
   OPENSSL_cleanse(&active, sizeof(active));
   return rc;
}

kb_management_cert_result_t
kb_management_cert_lifecycle_open(const kb_management_cert_config_t *config,
                                  kb_management_cert_lifecycle_t **out)
{
   if (out)
      *out = NULL;
   if (!config || !out || !exact_hex(config->installation_id, 32) ||
       !absolute_path(config->custodied_ca_dir) || !absolute_path(config->bundle_dir))
      return KB_MANAGEMENT_CERT_INVALID;
   if (!config->provider)
      return KB_MANAGEMENT_CERT_DISABLED;
   kb_workload_provider_kind_t kind = kb_workload_provider_kind(config->provider);
   if (kind == KB_WORKLOAD_PROVIDER_NONE || kind == KB_WORKLOAD_PROVIDER_TPM2_V1 ||
       kind == KB_WORKLOAD_PROVIDER_PKCS11_V1)
      return KB_MANAGEMENT_CERT_DISABLED;
   if (kind != KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1)
      return KB_MANAGEMENT_CERT_INVALID;

   kb_management_cert_lifecycle_t *lifecycle = calloc(1, sizeof(*lifecycle));
   if (!lifecycle)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   lifecycle->storage.dir_fd = -1;
   lifecycle->provider = config->provider;
   lifecycle->provider_kind = kind;
   memcpy(lifecycle->installation_id, config->installation_id, 33);
   memcpy(lifecycle->custodied_ca_dir, config->custodied_ca_dir,
          strlen(config->custodied_ca_dir) + 1);
   kb_management_cert_storage_result_t sr =
       kb_management_cert_storage_open(config->bundle_dir, &lifecycle->storage);
   if (sr != KB_MANAGEMENT_STORAGE_OK)
   {
      kb_management_cert_result_t rc = storage_result(sr);
      OPENSSL_cleanse(lifecycle, sizeof(*lifecycle));
      free(lifecycle);
      return rc;
   }
   if (pthread_mutex_init(&lifecycle->mutex, NULL) != 0)
   {
      kb_management_cert_storage_close(&lifecycle->storage);
      OPENSSL_cleanse(lifecycle, sizeof(*lifecycle));
      free(lifecycle);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   lifecycle->mutex_ready = 1;
   *out = lifecycle;
   return KB_MANAGEMENT_CERT_OK;
}

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
kb_management_cert_result_t kb_management_cert_lifecycle_open_for_test(
    const kb_management_cert_config_t *config, kb_workload_provider_kind_t kind, int directory_fd,
    const kb_management_cert_test_ops_t *ops, void *context, kb_management_cert_lifecycle_t **out)
{
   if (out)
      *out = NULL;
   if (!config || !out || directory_fd < 0 || !exact_hex(config->installation_id, 32) ||
       !absolute_path(config->custodied_ca_dir) || kind != KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 ||
       !ops || !ops->now || !ops->random || !ops->attest || !ops->wrap || !ops->unwrap ||
       !ops->preflight || !ops->begin_initial || !ops->begin_renewal || !ops->activate ||
       !ops->snapshot || !ops->crash)
      return KB_MANAGEMENT_CERT_INVALID;
   kb_management_cert_lifecycle_t *lifecycle = calloc(1, sizeof(*lifecycle));
   if (!lifecycle)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   lifecycle->storage.dir_fd = directory_fd;
   lifecycle->provider_kind = kind;
   lifecycle->test_ops = ops;
   lifecycle->test_context = context;
   memcpy(lifecycle->installation_id, config->installation_id, 33);
   memcpy(lifecycle->custodied_ca_dir, config->custodied_ca_dir,
          strlen(config->custodied_ca_dir) + 1);
   if (pthread_mutex_init(&lifecycle->mutex, NULL) != 0)
   {
      OPENSSL_cleanse(lifecycle, sizeof(*lifecycle));
      free(lifecycle);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   lifecycle->mutex_ready = 1;
   *out = lifecycle;
   return KB_MANAGEMENT_CERT_OK;
}
#endif

kb_management_cert_result_t kb_management_cert_reconcile(kb_management_cert_lifecycle_t *lifecycle,
                                                         int64_t deadline_epoch,
                                                         kb_management_cert_active_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!lifecycle || !out || deadline_epoch < 1)
      return KB_MANAGEMENT_CERT_INVALID;
   int old_cancel = 0;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel) != 0)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (pthread_mutex_lock(&lifecycle->mutex) != 0)
   {
      pthread_setcancelstate(old_cancel, NULL);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   db2_lease_begin();
   kb_management_cert_result_t rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   for (unsigned attempt = 0; attempt < 3; ++attempt)
   {
      secret_arena_t arena;
      if (secret_arena_open(lifecycle, &arena))
      {
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
         break;
      }
      rc = reconcile_once(lifecycle, deadline_epoch, out, &arena);
      secret_arena_close(&arena);
      if (rc != DB_RETRY_RESULT)
         break;
      memset(out, 0, sizeof(*out));
      int64_t now = lifecycle_now(lifecycle);
      if (now >= deadline_epoch)
         break;
      uint8_t jitter = 0;
      (void)lifecycle_random(lifecycle, &jitter, 1);
      unsigned delay_ms = (25U << attempt) + jitter % 17U;
      int64_t remaining_seconds = deadline_epoch - now;
      int64_t remaining_ms =
          remaining_seconds > INT64_MAX / 1000 ? INT64_MAX : remaining_seconds * 1000;
      if ((int64_t)delay_ms > remaining_ms)
         delay_ms = (unsigned)remaining_ms;
      struct timespec delay = {.tv_sec = delay_ms / 1000U,
                               .tv_nsec = (long)(delay_ms % 1000U) * 1000000L};
      nanosleep(&delay, NULL);
   }
   if (rc == DB_RETRY_RESULT)
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (rc != KB_MANAGEMENT_CERT_OK)
      memset(out, 0, sizeof(*out));
   db2_lease_end();
   pthread_mutex_unlock(&lifecycle->mutex);
   pthread_setcancelstate(old_cancel, NULL);
   return rc;
}

static kb_management_cert_result_t load_once(kb_management_cert_lifecycle_t *lifecycle,
                                             kb_management_cert_bundle_t *bundle,
                                             kb_management_cert_active_t *out, int *changed,
                                             secret_arena_t *arena)
{
   *changed = 0;
   live_binding_t live = {0};
   kb_management_cert_result_t rc = attest(lifecycle, &live);
   if (rc != KB_MANAGEMENT_CERT_OK)
      return rc;
   db2_management_client_active_t before = {0}, after = {0};
   db2_management_client_instance_result_t snapshot_dr =
       lifecycle->test_ops
           ? lifecycle->test_ops->snapshot(lifecycle->test_context, lifecycle->installation_id,
                                           &live.db, &before)
           : db2_management_client_instance_snapshot(lifecycle->installation_id, &live.db, &before);
   rc = db_result(snapshot_dr);
   if (rc != KB_MANAGEMENT_CERT_OK)
      goto done;
   kb_management_cert_manifest_t current = {0};
   int present = 0;
   rc = read_current(lifecycle, &current, &present);
   if (rc != KB_MANAGEMENT_CERT_OK)
      goto done;
   if (!present || strcmp(current.operation_id, before.operation_id) ||
       current.generation != before.generation ||
       CRYPTO_memcmp(current.public_bundle_digest, before.public_bundle_digest, 32))
   {
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
      goto done;
   }
   recovered_candidate_t *candidate = secret_arena_alloc(arena, sizeof(*candidate));
   if (!candidate)
   {
      rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
      goto done;
   }
   rc = recover_candidate(lifecycle, &live, current.operation_id, candidate, arena);
   if (rc == KB_MANAGEMENT_CERT_OK && !candidate_matches_active(&candidate->view, &before))
      rc = KB_MANAGEMENT_CERT_INTEGRITY;
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      snapshot_dr = lifecycle->test_ops
                        ? lifecycle->test_ops->snapshot(
                              lifecycle->test_context, lifecycle->installation_id, &live.db, &after)
                        : db2_management_client_instance_snapshot(lifecycle->installation_id,
                                                                  &live.db, &after);
      rc = db_result(snapshot_dr);
   }
   if (rc == KB_MANAGEMENT_CERT_OK && !active_equal(&before, &after))
   {
      *changed = 1;
      rc = KB_MANAGEMENT_CERT_CONFLICT;
   }
   if (rc == KB_MANAGEMENT_CERT_OK)
   {
      *bundle = candidate->bundle;
      memset(&candidate->bundle, 0, sizeof(candidate->bundle));
      active_public(&after, out);
   }
   kb_management_cert_bundle_clear(&candidate->bundle);
   OPENSSL_cleanse(candidate, sizeof(*candidate));
done:
   OPENSSL_cleanse(&live, sizeof(live));
   OPENSSL_cleanse(&before, sizeof(before));
   OPENSSL_cleanse(&after, sizeof(after));
   if (rc != KB_MANAGEMENT_CERT_OK)
   {
      kb_management_cert_bundle_clear(bundle);
      memset(out, 0, sizeof(*out));
   }
   return rc;
}

kb_management_cert_result_t
kb_management_cert_load_active(kb_management_cert_lifecycle_t *lifecycle,
                               kb_management_cert_bundle_t *bundle,
                               kb_management_cert_active_t *out)
{
   if (bundle)
      kb_management_cert_bundle_clear(bundle);
   if (out)
      memset(out, 0, sizeof(*out));
   if (!lifecycle || !bundle || !out)
      return KB_MANAGEMENT_CERT_INVALID;
   int old_cancel = 0;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel) != 0)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   if (pthread_mutex_lock(&lifecycle->mutex) != 0)
   {
      pthread_setcancelstate(old_cancel, NULL);
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   }
   db2_lease_begin();
   kb_management_cert_result_t rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
   cancel_output_t cancel_output = {.bundle = bundle, .active = out};
   pthread_cleanup_push(cancel_output_clear, &cancel_output);
   for (unsigned snapshot_attempt = 0; snapshot_attempt < 2; ++snapshot_attempt)
   {
      int changed = 0;
      for (unsigned db_attempt = 0; db_attempt < 3; ++db_attempt)
      {
         secret_arena_t arena;
         if (secret_arena_open(lifecycle, &arena))
         {
            rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
            break;
         }
         rc = load_once(lifecycle, bundle, out, &changed, &arena);
         secret_arena_close(&arena);
         if (rc != DB_RETRY_RESULT)
            break;
         struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)(25U << db_attempt) * 1000000L};
         nanosleep(&delay, NULL);
      }
      if (rc == DB_RETRY_RESULT)
      {
         rc = KB_MANAGEMENT_CERT_UNAVAILABLE;
         break;
      }
      if (!changed)
         break;
      if (snapshot_attempt == 1)
      {
         rc = KB_MANAGEMENT_CERT_INTEGRITY;
         kb_management_cert_bundle_clear(bundle);
         memset(out, 0, sizeof(*out));
         break;
      }
      struct timespec delay = {.tv_sec = 0, .tv_nsec = 25000000L};
      nanosleep(&delay, NULL);
   }
   db2_lease_end();
   pthread_mutex_unlock(&lifecycle->mutex);
   pthread_setcancelstate(old_cancel, NULL);
   pthread_cleanup_pop(0);
   return rc;
}

void kb_management_cert_bundle_clear(kb_management_cert_bundle_t *bundle)
{
   if (bundle)
      OPENSSL_cleanse(bundle, sizeof(*bundle));
}

void kb_management_cert_lifecycle_close(kb_management_cert_lifecycle_t *lifecycle)
{
   if (!lifecycle)
      return;
   if (lifecycle->mutex_ready)
      pthread_mutex_destroy(&lifecycle->mutex);
   kb_management_cert_storage_close(&lifecycle->storage);
   OPENSSL_cleanse(lifecycle, sizeof(*lifecycle));
   free(lifecycle);
}
