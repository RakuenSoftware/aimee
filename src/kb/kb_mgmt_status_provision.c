#include "kb_mgmt_status_provision.h"

#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#define STATUS_PRINCIPAL "org:p5-status"
#define STATUS_AGENT     "management"
#define STATUS_CRED      "ed25519"

typedef struct
{
   uint8_t seed[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN];
   uint8_t plaintext[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN];
   uint8_t dek[VAULT_DEK_LEN];
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
} provision_secret_arena_t;

typedef enum
{
   SECRET_BUILD = 1,
   SECRET_RECOVER = 2,
} secret_action_t;

typedef struct
{
   secret_action_t action;
   kb_mgmt_status_provision_record_t *record;
   kb_mgmt_status_provision_result_t result;
} secret_call_t;

static provision_secret_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   if (!mapped)
      return NULL;
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0)
      return NULL;
   size_t n = (sizeof(provision_secret_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return NULL;
   if (mlock(p, n) || madvise(p, n, MADV_DONTDUMP) || madvise(p, n, MADV_WIPEONFORK))
   {
      OPENSSL_cleanse(p, n);
      (void)munlock(p, n);
      (void)munmap(p, n);
      return NULL;
   }
   *mapped = n;
   return p;
#else
   (void)mapped;
   return NULL;
#endif
}

static void arena_free(provision_secret_arena_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#else
   (void)mapped;
#endif
}

static int sha256_parts(const void *a, size_t an, const void *b, size_t bn,
                        uint8_t out[KB_MGMT_STATUS_PROVISION_DIGEST_LEN])
{
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
            (!an || EVP_DigestUpdate(ctx, a, an) == 1) &&
            (!bn || EVP_DigestUpdate(ctx, b, bn) == 1) && EVP_DigestFinal_ex(ctx, out, &n) == 1 &&
            n == KB_MGMT_STATUS_PROVISION_DIGEST_LEN;
   EVP_MD_CTX_free(ctx);
   if (!ok)
      OPENSSL_cleanse(out, KB_MGMT_STATUS_PROVISION_DIGEST_LEN);
   return ok ? 0 : -1;
}

int kb_mgmt_status_provision_wire_id(const uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN],
                                     char out[KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX + 1])
{
   static const char hex[] = "0123456789abcdef";
   static const char prefix[] = "p5-status-v1-";
   uint8_t digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
   if (!public_key || !out ||
       sha256_parts(public_key, KB_MGMT_STATUS_PROVISION_PUBLIC_LEN, NULL, 0, digest) != 0)
      return -1;
   memcpy(out, prefix, sizeof(prefix) - 1);
   size_t off = sizeof(prefix) - 1;
   for (size_t i = 0; i < 16; ++i)
   {
      out[off++] = hex[digest[i] >> 4];
      out[off++] = hex[digest[i] & 15];
   }
   out[off] = 0;
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

int kb_mgmt_status_provision_bootstrap_id(const char *custody_key_id,
                                          char out[KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN + 1])
{
   static const char domain[] = "aimee-p5-status-bootstrap-v1|";
   static const char hex[] = "0123456789abcdef";
   uint8_t digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
   if (!custody_key_id || !custody_key_id[0] ||
       strlen(custody_key_id) > KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX || !out ||
       sha256_parts(domain, sizeof(domain) - 1, custody_key_id, strlen(custody_key_id), digest))
      return -1;
   for (size_t i = 0; i < sizeof(digest); ++i)
   {
      out[i * 2] = hex[digest[i] >> 4];
      out[i * 2 + 1] = hex[digest[i] & 15];
   }
   out[KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN] = 0;
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

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
      out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static int digest_field(EVP_MD_CTX *ctx, const void *value, size_t len)
{
   if (len > UINT32_MAX)
      return -1;
   uint8_t encoded[4];
   put_u32be(encoded, (uint32_t)len);
   return EVP_DigestUpdate(ctx, encoded, sizeof(encoded)) == 1 &&
                  (!len || EVP_DigestUpdate(ctx, value, len) == 1)
              ? 0
              : -1;
}

int kb_mgmt_status_provision_envelope_digest(const kb_mgmt_status_provision_envelope_t *envelope,
                                             uint8_t out[KB_MGMT_STATUS_PROVISION_DIGEST_LEN])
{
   static const char domain[] = "aimee.management.status.envelope.v1";
   uint8_t version[8], format = 1, aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   if (!envelope || !out || (envelope->version != 1 && envelope->version != 2) ||
       envelope->ciphertext_len != KB_MGMT_STATUS_PROVISION_PUBLIC_LEN ||
       vault_aad_build_v2(STATUS_PRINCIPAL, STATUS_AGENT, STATUS_CRED, envelope->version, aad,
                          sizeof(aad), &aad_len) != 0)
      return -1;
   put_u64be(version, (uint64_t)envelope->version);
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(ctx, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(ctx, &format, sizeof(format)) == 1 &&
            EVP_DigestUpdate(ctx, version, sizeof(version)) == 1 &&
            digest_field(ctx, aad, aad_len) == 0 &&
            digest_field(ctx, envelope->wrapped_dek, sizeof(envelope->wrapped_dek)) == 0 &&
            digest_field(ctx, envelope->nonce, sizeof(envelope->nonce)) == 0 &&
            digest_field(ctx, envelope->ciphertext, envelope->ciphertext_len) == 0 &&
            digest_field(ctx, envelope->tag, sizeof(envelope->tag)) == 0 &&
            EVP_DigestFinal_ex(ctx, out, &n) == 1 && n == KB_MGMT_STATUS_PROVISION_DIGEST_LEN;
   EVP_MD_CTX_free(ctx);
   OPENSSL_cleanse(aad, sizeof(aad));
   if (!ok)
      OPENSSL_cleanse(out, KB_MGMT_STATUS_PROVISION_DIGEST_LEN);
   return ok ? 0 : -1;
}

static int public_from_seed(const uint8_t seed[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN],
                            uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN])
{
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed,
                                                KB_MGMT_STATUS_PROVISION_PUBLIC_LEN);
   size_t n = KB_MGMT_STATUS_PROVISION_PUBLIC_LEN;
   int ok = key && EVP_PKEY_get_raw_public_key(key, public_key, &n) == 1 &&
            n == KB_MGMT_STATUS_PROVISION_PUBLIC_LEN;
   EVP_PKEY_free(key);
   if (!ok)
      OPENSSL_cleanse(public_key, KB_MGMT_STATUS_PROVISION_PUBLIC_LEN);
   return ok ? 0 : -1;
}

static int set_public_binding(kb_mgmt_status_provision_record_t *record, const uint8_t seed[32],
                              int compare)
{
   uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN], digest[32];
   char wire[KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX + 1];
   int rc = public_from_seed(seed, public_key) ||
                    sha256_parts(public_key, sizeof(public_key), NULL, 0, digest) ||
                    kb_mgmt_status_provision_wire_id(public_key, wire)
                ? -1
                : 0;
   if (!rc && compare)
      rc = CRYPTO_memcmp(record->public_key, public_key, sizeof(public_key)) ||
                   CRYPTO_memcmp(record->public_key_digest, digest, sizeof(digest)) ||
                   strcmp(record->wire_key_id, wire)
               ? -1
               : 0;
   else if (!rc)
   {
      memcpy(record->public_key, public_key, sizeof(public_key));
      memcpy(record->public_key_digest, digest, sizeof(digest));
      snprintf(record->wire_key_id, sizeof(record->wire_key_id), "%s", wire);
   }
   OPENSSL_cleanse(public_key, sizeof(public_key));
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(wire, sizeof(wire));
   return rc;
}

static int encrypt_seed(const uint8_t kek[VAULT_KEK_LEN], provision_secret_arena_t *arena,
                        int64_t version, kb_mgmt_status_provision_envelope_t *envelope)
{
   size_t aad_len = 0;
   memset(envelope, 0, sizeof(*envelope));
   envelope->version = version;
   envelope->ciphertext_len = sizeof(arena->seed);
   OPENSSL_cleanse(arena->dek, sizeof(arena->dek));
   OPENSSL_cleanse(arena->aad, sizeof(arena->aad));
   if (vault_aad_build_v2(STATUS_PRINCIPAL, STATUS_AGENT, STATUS_CRED, version, arena->aad,
                          sizeof(arena->aad), &aad_len) ||
       vault_crypto_random(arena->dek, sizeof(arena->dek)) ||
       vault_secret_encrypt(arena->dek, arena->aad, aad_len, arena->seed, sizeof(arena->seed),
                            envelope->nonce, envelope->ciphertext, envelope->tag) ||
       vault_dek_wrap(kek, arena->dek, envelope->wrapped_dek))
   {
      OPENSSL_cleanse(envelope, sizeof(*envelope));
      return -1;
   }
   return 0;
}

static int secret_callback(const uint8_t kek[VAULT_KEK_LEN], void *opaque)
{
   secret_call_t *call = opaque;
   size_t mapped = 0;
   provision_secret_arena_t *arena = arena_new(&mapped);
   if (!arena)
   {
      call->result = KB_MGMT_STATUS_PROVISION_RETRY;
      return -1;
   }
   int rc = -1;
   if (call->action == SECRET_BUILD)
   {
      if (!vault_crypto_random(arena->seed, sizeof(arena->seed)) &&
          !set_public_binding(call->record, arena->seed, 0) &&
          !encrypt_seed(kek, arena, 1, &call->record->v1) &&
          !encrypt_seed(kek, arena, 2, &call->record->v2) &&
          !kb_mgmt_status_provision_envelope_digest(&call->record->v1, call->record->v1_digest) &&
          !kb_mgmt_status_provision_envelope_digest(&call->record->v2, call->record->v2_digest))
         rc = 0;
      else
         call->result = KB_MGMT_STATUS_PROVISION_RETRY;
   }
   else if (call->action == SECRET_RECOVER)
   {
      size_t aad_len = 0;
      if (!vault_aad_build_v2(STATUS_PRINCIPAL, STATUS_AGENT, STATUS_CRED, 2, arena->aad,
                              sizeof(arena->aad), &aad_len) &&
          !vault_dek_unwrap(kek, call->record->v2.wrapped_dek, arena->dek) &&
          !vault_secret_decrypt(arena->dek, arena->aad, aad_len, call->record->v2.nonce,
                                call->record->v2.ciphertext, call->record->v2.ciphertext_len,
                                call->record->v2.tag, arena->plaintext) &&
          !set_public_binding(call->record, arena->plaintext, 1))
         rc = 0;
      else
         call->result = KB_MGMT_STATUS_PROVISION_INTEGRITY;
   }
   else
      call->result = KB_MGMT_STATUS_PROVISION_INTEGRITY;
   arena_free(arena, mapped);
   return rc;
}

static kb_mgmt_status_provision_result_t db_result(kb_mgmt_status_provision_db_result_t rc)
{
   switch (rc)
   {
   case KB_MGMT_STATUS_PROVISION_DB_SEALED:
      return KB_MGMT_STATUS_PROVISION_SEALED;
   case KB_MGMT_STATUS_PROVISION_DB_CONFLICT:
      return KB_MGMT_STATUS_PROVISION_CONFLICT;
   case KB_MGMT_STATUS_PROVISION_DB_INTEGRITY:
      return KB_MGMT_STATUS_PROVISION_INTEGRITY;
   case KB_MGMT_STATUS_PROVISION_DB_RETRY:
   default:
      return KB_MGMT_STATUS_PROVISION_RETRY;
   }
}

static int fixed_record(const kb_mgmt_status_provision_record_t *record, const char *custody_key_id)
{
   uint8_t digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
   char bootstrap_id[KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN + 1];
   size_t custody_len = strlen(custody_key_id);
   if (kb_mgmt_status_provision_bootstrap_id(custody_key_id, bootstrap_id) ||
       (record->phase != KB_MGMT_STATUS_PROVISION_STAGED &&
        record->phase != KB_MGMT_STATUS_PROVISION_ENABLED) ||
       !record->seal_epoch ||
       strnlen(record->bootstrap_id, sizeof(record->bootstrap_id)) !=
           KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN ||
       strcmp(record->bootstrap_id, bootstrap_id) ||
       strnlen(record->custody_key_id, sizeof(record->custody_key_id)) != custody_len ||
       CRYPTO_memcmp(record->custody_key_id, custody_key_id, custody_len + 1) ||
       strnlen(record->wire_key_id, sizeof(record->wire_key_id)) < 1 ||
       strnlen(record->wire_key_id, sizeof(record->wire_key_id)) >
           KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX ||
       record->hwm1_attestation_len != 64 ||
       vault_hwm_verify(custody_key_id, 1, record->hwm1_attestation,
                        record->hwm1_attestation_len) ||
       (record->phase == KB_MGMT_STATUS_PROVISION_ENABLED &&
        (record->hwm2_attestation_len != 64 ||
         vault_hwm_verify(custody_key_id, 2, record->hwm2_attestation,
                          record->hwm2_attestation_len))) ||
       record->v1.version != 1 || record->v2.version != 2 ||
       kb_mgmt_status_provision_envelope_digest(&record->v1, digest) ||
       CRYPTO_memcmp(digest, record->v1_digest, sizeof(digest)) ||
       kb_mgmt_status_provision_envelope_digest(&record->v2, digest) ||
       CRYPTO_memcmp(digest, record->v2_digest, sizeof(digest)))
   {
      OPENSSL_cleanse(digest, sizeof(digest));
      OPENSSL_cleanse(bootstrap_id, sizeof(bootstrap_id));
      return 0;
   }
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(bootstrap_id, sizeof(bootstrap_id));
   return 1;
}

static kb_mgmt_status_provision_result_t
protected_secret_action(kb_mgmt_status_provision_record_t *record, secret_action_t action)
{
   vault_maintenance_guard_t *guard = NULL;
   secret_call_t call = {
       .action = action, .record = record, .result = KB_MGMT_STATUS_PROVISION_RETRY};
   int rc = vault_maintenance_guard_begin(&guard);
   if (rc != VAULT_MAINTENANCE_OK)
      return KB_MGMT_STATUS_PROVISION_RETRY;
   if (vault_maintenance_guard_sync_primary_epoch(guard, record->seal_epoch) !=
           VAULT_MAINTENANCE_OK ||
       vault_maintenance_guard_unseal(guard, NULL, 0) != VAULT_MAINTENANCE_OK)
      goto done;
   rc = vault_maintenance_guard_with_active_kek(guard, secret_callback, &call);
   if (rc == VAULT_MAINTENANCE_SEALED)
      call.result = KB_MGMT_STATUS_PROVISION_SEALED;
   else if (rc == VAULT_MAINTENANCE_OK)
      call.result = KB_MGMT_STATUS_PROVISION_FRESH;
done:
   if (vault_maintenance_guard_end(&guard) != VAULT_MAINTENANCE_OK)
      return KB_MGMT_STATUS_PROVISION_RETRY;
   return call.result;
}

static kb_mgmt_status_provision_result_t hwm_read_verified(const char *key_id, uint64_t *version,
                                                           uint8_t att[512], size_t *att_len)
{
   if (vault_hwm_read(key_id, version, att, KB_MGMT_STATUS_PROVISION_ATTEST_MAX, att_len) ||
       !*version || *att_len < 1 || *att_len > KB_MGMT_STATUS_PROVISION_ATTEST_MAX ||
       vault_hwm_verify(key_id, *version, att, *att_len))
   {
      OPENSSL_cleanse(att, KB_MGMT_STATUS_PROVISION_ATTEST_MAX);
      *version = 0;
      *att_len = 0;
      return KB_MGMT_STATUS_PROVISION_RETRY;
   }
   return KB_MGMT_STATUS_PROVISION_FRESH;
}

static kb_mgmt_status_provision_result_t advance_hwm(const char *key_id, uint64_t version,
                                                     uint8_t att[512], size_t *att_len)
{
   if (version == 2)
      return KB_MGMT_STATUS_PROVISION_FRESH;
   if (version != 1)
      return KB_MGMT_STATUS_PROVISION_INTEGRITY;
   OPENSSL_cleanse(att, KB_MGMT_STATUS_PROVISION_ATTEST_MAX);
   *att_len = 0;
   if (!vault_hwm_cas(key_id, 1, 2, att, KB_MGMT_STATUS_PROVISION_ATTEST_MAX, att_len) &&
       *att_len && *att_len <= KB_MGMT_STATUS_PROVISION_ATTEST_MAX &&
       !vault_hwm_verify(key_id, 2, att, *att_len))
      return KB_MGMT_STATUS_PROVISION_FRESH;

   uint64_t reread = 0;
   OPENSSL_cleanse(att, KB_MGMT_STATUS_PROVISION_ATTEST_MAX);
   *att_len = 0;
   kb_mgmt_status_provision_result_t rc = hwm_read_verified(key_id, &reread, att, att_len);
   if (rc != KB_MGMT_STATUS_PROVISION_FRESH)
      return rc;
   return reread == 2
              ? KB_MGMT_STATUS_PROVISION_FRESH
              : (reread == 1 ? KB_MGMT_STATUS_PROVISION_RETRY : KB_MGMT_STATUS_PROVISION_INTEGRITY);
}

static kb_mgmt_status_provision_result_t
provision_impl(const char *custody_key_id, const kb_mgmt_status_provision_db_t *database,
               kb_mgmt_status_provision_output_t *output)
{
   if (output)
      OPENSSL_cleanse(output, sizeof(*output));
   if (!custody_key_id || !custody_key_id[0] ||
       strlen(custody_key_id) > KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX || !database ||
       !database->inspect || !database->stage || !database->prepare_activation ||
       !database->finalize || !output)
      return KB_MGMT_STATUS_PROVISION_INTEGRITY;

   kb_mgmt_status_provision_record_t record;
   memset(&record, 0, sizeof(record));
   uint8_t att[KB_MGMT_STATUS_PROVISION_ATTEST_MAX] = {0};
   size_t att_len = 0;
   uint64_t version = 0;
   kb_mgmt_status_provision_result_t rc = KB_MGMT_STATUS_PROVISION_RETRY;
   kb_mgmt_status_provision_db_result_t drc =
       database->inspect(database->ctx, custody_key_id, &record);
   if (drc != KB_MGMT_STATUS_PROVISION_DB_OK)
   {
      rc = db_result(drc);
      goto done;
   }
   int fresh = record.phase == KB_MGMT_STATUS_PROVISION_EMPTY;
   if ((fresh && !record.seal_epoch) || (!fresh && !fixed_record(&record, custody_key_id)))
   {
      rc = KB_MGMT_STATUS_PROVISION_INTEGRITY;
      goto done;
   }

   rc = hwm_read_verified(custody_key_id, &version, att, &att_len);
   if (rc != KB_MGMT_STATUS_PROVISION_FRESH || (fresh && version != 1) ||
       (record.phase == KB_MGMT_STATUS_PROVISION_ENABLED && version != 2) || version > 2)
   {
      if (rc == KB_MGMT_STATUS_PROVISION_FRESH)
         rc = KB_MGMT_STATUS_PROVISION_INTEGRITY;
      goto done;
   }

   if (record.phase == KB_MGMT_STATUS_PROVISION_ENABLED)
   {
      rc = protected_secret_action(&record, SECRET_RECOVER);
      if (rc == KB_MGMT_STATUS_PROVISION_FRESH)
         rc = KB_MGMT_STATUS_PROVISION_CONFLICT;
      goto done;
   }

   if (fresh)
   {
      snprintf(record.custody_key_id, sizeof(record.custody_key_id), "%s", custody_key_id);
      if (kb_mgmt_status_provision_bootstrap_id(custody_key_id, record.bootstrap_id))
      {
         rc = KB_MGMT_STATUS_PROVISION_RETRY;
         goto done;
      }
      record.phase = KB_MGMT_STATUS_PROVISION_STAGED;
      if (att_len != 64)
      {
         rc = KB_MGMT_STATUS_PROVISION_INTEGRITY;
         goto done;
      }
      memcpy(record.hwm1_attestation, att, att_len);
      record.hwm1_attestation_len = att_len;
      rc = protected_secret_action(&record, SECRET_BUILD);
      if (rc != KB_MGMT_STATUS_PROVISION_FRESH)
         goto done;
      drc = database->stage(database->ctx, &record);
      if (drc != KB_MGMT_STATUS_PROVISION_DB_OK)
      {
         rc = db_result(drc);
         goto done;
      }
   }
   else
   {
      rc = protected_secret_action(&record, SECRET_RECOVER);
      if (rc != KB_MGMT_STATUS_PROVISION_FRESH)
         goto done;
   }

   drc = database->prepare_activation(database->ctx, &record);
   if (drc != KB_MGMT_STATUS_PROVISION_DB_OK)
   {
      rc = db_result(drc);
      goto done;
   }

   rc = advance_hwm(custody_key_id, version, att, &att_len);
   if (rc != KB_MGMT_STATUS_PROVISION_FRESH)
      goto done;
   drc = database->finalize(database->ctx, &record, att, att_len);
   if (drc != KB_MGMT_STATUS_PROVISION_DB_OK)
   {
      rc = db_result(drc);
      goto done;
   }
   if (fresh)
   {
      snprintf(output->custody_key_id, sizeof(output->custody_key_id), "%s", custody_key_id);
      snprintf(output->wire_key_id, sizeof(output->wire_key_id), "%s", record.wire_key_id);
      memcpy(output->public_key, record.public_key, sizeof(output->public_key));
      rc = KB_MGMT_STATUS_PROVISION_FRESH;
   }
   else
      rc = KB_MGMT_STATUS_PROVISION_RECOVERED;
done:
   if (rc != KB_MGMT_STATUS_PROVISION_FRESH)
      OPENSSL_cleanse(output, sizeof(*output));
   OPENSSL_cleanse(att, sizeof(att));
   OPENSSL_cleanse(&record, sizeof(record));
   return rc;
}

kb_mgmt_status_provision_result_t
kb_mgmt_status_provision(const char *custody_key_id, const kb_mgmt_status_provision_db_t *database,
                         kb_mgmt_status_provision_output_t *output)
{
   if (output)
      OPENSSL_cleanse(output, sizeof(*output));
   int prior = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior) != 0)
      return KB_MGMT_STATUS_PROVISION_RETRY;
   kb_mgmt_status_provision_result_t rc = provision_impl(custody_key_id, database, output);
   /* A pending cancellation is delivered only after provision_impl has erased
    * all transient envelopes, attestations, and protected key material. */
   (void)pthread_setcancelstate(prior, NULL);
   if (prior == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return rc;
}
