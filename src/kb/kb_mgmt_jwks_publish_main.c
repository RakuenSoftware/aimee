/* Offline, owner-only P5-C2b signed-JWKS publisher.  Its database writer,
 * custody decrypt/sign closure, and provider-CAS seam must never enter an
 * online binary or installation target. */
#include "kb_mgmt_jwks_publication.h"
#include "kb_mgmt_offline_hardening.h"
#include "management_jwks_publication.h"
#include "vault_custody_kms.h"
#include "vault_crypto.h"
#include "vault_internal.h"
#include "vault_server_key.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PUBLISH_CLOCK_SKEW_SECONDS 300u
#define PUBLISH_MAX_LIFETIME       86400u

typedef struct
{
   db2_management_jwks_publication_ctx_t db; /* Must remain first: shared callback opaque. */
} publisher_ctx_t;

typedef struct
{
   const db2_management_jwks_admission_t *admission;
   const kb_mgmt_root_record_t *manifest;
   const uint8_t *payload;
   size_t payload_len;
   uint8_t *signature;
   kb_mgmt_jwks_result_t result;
} sign_call_t;

typedef struct
{
   uint8_t dek[VAULT_DEK_LEN];
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   uint8_t seed[32];
} sign_arena_t;

enum
{
   EXIT_USAGE = 64,
   EXIT_CONFIGURATION = 65,
   EXIT_HARDENING = 66,
   EXIT_DATABASE = 67,
   EXIT_CUSTODY = 68,
   EXIT_CONFLICT = 69,
   EXIT_RETRY = 70,
   EXIT_SEALED = 71,
   EXIT_INTEGRITY = 72,
   EXIT_OUTPUT = 73,
};

static void fixed_error(const char *error_class)
{
   (void)fprintf(stderr, "aimee-kb-jwks-publish: %s\n", error_class);
}

static const char *exit_class(int value)
{
   switch (value)
   {
   case EXIT_USAGE:
      return "usage";
   case EXIT_CONFIGURATION:
      return "configuration";
   case EXIT_HARDENING:
      return "hardening";
   case EXIT_DATABASE:
      return "database";
   case EXIT_CUSTODY:
      return "custody";
   case EXIT_CONFLICT:
      return "conflict";
   case EXIT_RETRY:
      return "retry";
   case EXIT_SEALED:
      return "sealed";
   case EXIT_OUTPUT:
      return "output";
   default:
      return "integrity";
   }
}

static int fixed_text(const char *value, size_t max)
{
   if (!value || !*value || strnlen(value, max + 1) > max)
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static char *copy_env(const char *name, size_t max)
{
   const char *value = getenv(name);
   return fixed_text(value, max) ? strdup(value) : NULL;
}

static int canonical_time(const char *value, int64_t *out)
{
   if (!value || !out || !*value || (value[0] == '0' && value[1]))
      return -1;
   uint64_t n = 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
   {
      if (*p < '0' || *p > '9' || n > (uint64_t)(KB_MGMT_JWKS_TIME_MAX - (*p - '0')) / 10)
         return -1;
      n = n * 10 + (uint64_t)(*p - '0');
   }
   *out = (int64_t)n;
   return 0;
}

static int root_owned_fixed_file(const char *path, int executable)
{
   struct stat st;
   char parent[4096];
   if (!path || path[0] != '/' || strlen(path) >= sizeof(parent) || lstat(path, &st) != 0 ||
       !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022) ||
       (executable && !(st.st_mode & 0111)))
      return 0;
   memcpy(parent, path, strlen(path) + 1);
   char *slash = strrchr(parent, '/');
   if (!slash)
      return 0;
   if (slash == parent)
      parent[1] = '\0';
   else
      *slash = '\0';
   for (;;)
   {
      if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
         return 0;
      if (parent[0] == '/' && parent[1] == '\0')
         return 1;
      slash = strrchr(parent, '/');
      if (!slash)
         return 0;
      if (slash == parent)
         parent[1] = '\0';
      else
         *slash = '\0';
   }
}

static int silence_stderr(int *saved)
{
   int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
   if (!saved || nullfd < 0)
      return -1;
   *saved = dup(STDERR_FILENO);
   int ok = *saved >= 0 && dup2(nullfd, STDERR_FILENO) >= 0;
   (void)close(nullfd);
   if (!ok)
   {
      if (*saved >= 0)
         (void)close(*saved);
      *saved = -1;
      return -1;
   }
   return 0;
}

static int restore_stderr(int *saved)
{
   if (!saved || *saved < 0)
      return -1;
   int rc = dup2(*saved, STDERR_FILENO);
   int close_rc = close(*saved);
   *saved = -1;
   return rc < 0 || close_rc != 0 ? -1 : 0;
}

static int public_identity_digest(const char *path, uint8_t digest[32])
{
   uint8_t key[32], extra;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   ssize_t n = read(fd, key, sizeof(key));
   ssize_t tail = n == (ssize_t)sizeof(key) ? read(fd, &extra, 1) : -1;
   int close_rc = close(fd);
   unsigned int digest_len = 0;
   int ok = n == (ssize_t)sizeof(key) && tail == 0 && close_rc == 0 &&
            EVP_Digest(key, sizeof(key), digest, &digest_len, EVP_sha256(), NULL) == 1 &&
            digest_len == 32;
   OPENSSL_cleanse(key, sizeof(key));
   if (!ok)
      OPENSSL_cleanse(digest, 32);
   return ok ? 0 : -1;
}

static void hex_encode(const uint8_t value[32], char out[65])
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[i * 2] = digits[value[i] >> 4];
      out[i * 2 + 1] = digits[value[i] & 15];
   }
   out[64] = '\0';
}

static int key_use_id(uint64_t generation, uint64_t seal_epoch, const char *candidate_id,
                      const uint8_t payload_digest[32], char out[65])
{
   static const char domain[] = "aimee.management.jwks.manifest.use.v1\n";
   uint8_t generation_be[8], seal_epoch_be[8], digest[32];
   for (unsigned i = 0; i < 8; ++i)
   {
      generation_be[i] = (uint8_t)(generation >> (56 - i * 8));
      seal_epoch_be[i] = (uint8_t)(seal_epoch >> (56 - i * 8));
   }
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && candidate_id && strlen(candidate_id) == 64 && payload_digest && out &&
            EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(md, generation_be, sizeof(generation_be)) == 1 &&
            EVP_DigestUpdate(md, seal_epoch_be, sizeof(seal_epoch_be)) == 1 &&
            EVP_DigestUpdate(md, candidate_id, 64) == 1 &&
            EVP_DigestUpdate(md, payload_digest, 32) == 1 &&
            EVP_DigestFinal_ex(md, digest, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   if (ok)
      hex_encode(digest, out);
   else if (out)
      OPENSSL_cleanse(out, 65);
   OPENSSL_cleanse(digest, sizeof(digest));
   return ok ? 0 : -1;
}

static sign_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   long page = mapped ? sysconf(_SC_PAGESIZE) : -1;
   if (page <= 0)
      return NULL;
   size_t size = (sizeof(sign_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *value = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (value == MAP_FAILED)
      return NULL;
   if (mlock(value, size) || madvise(value, size, MADV_DONTDUMP) ||
       madvise(value, size, MADV_WIPEONFORK))
   {
      OPENSSL_cleanse(value, size);
      (void)munlock(value, size);
      (void)munmap(value, size);
      return NULL;
   }
   *mapped = size;
   return value;
#else
   (void)mapped;
   return NULL;
#endif
}

static void arena_free(sign_arena_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#endif
}

static int sign_with_kek(const uint8_t kek[VAULT_KEK_LEN], void *opaque)
{
   sign_call_t *call = opaque;
   sign_arena_t *arena = NULL;
   size_t mapped = 0, aad_len = 0;
   arena = arena_new(&mapped);
   if (!arena)
      return -1;
   const kb_mgmt_root_envelope_t *envelope = &call->admission->envelope;
   int ok =
       envelope->version == 2 && envelope->ciphertext_len == 32 &&
       !kb_mgmt_root_aad(KB_MGMT_ROOT_MANIFEST, 2, arena->aad, sizeof(arena->aad), &aad_len) &&
       !vault_dek_unwrap(kek, envelope->wrapped_dek, arena->dek) &&
       !vault_secret_decrypt(arena->dek, arena->aad, aad_len, envelope->nonce, envelope->ciphertext,
                             envelope->ciphertext_len, envelope->tag, arena->seed) &&
       !kb_mgmt_jwks_ed25519_sign(arena->seed, call->payload, call->payload_len, call->signature) &&
       !kb_mgmt_jwks_ed25519_verify(call->manifest->public_key, call->payload, call->payload_len,
                                    call->signature);
   arena_free(arena, mapped);
   call->result = ok ? KB_MGMT_JWKS_FRESH : KB_MGMT_JWKS_INTEGRITY;
   return ok ? 0 : -1;
}

static kb_mgmt_jwks_result_t protected_sign(void *opaque, const kb_mgmt_root_record_t *manifest,
                                            uint64_t generation, const char *candidate_id,
                                            const uint8_t payload_digest[32],
                                            const uint8_t *payload, size_t payload_len,
                                            uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN])
{
   publisher_ctx_t *ctx = opaque;
   db2_management_jwks_admission_t admission;
   uint8_t live_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t live_attestation_len = 0;
   uint64_t live_version = 0;
   char use_id[65];
   OPENSSL_cleanse(&admission, sizeof(admission));
   OPENSSL_cleanse(live_attestation, sizeof(live_attestation));
   OPENSSL_cleanse(use_id, sizeof(use_id));
   if (!ctx || !manifest || manifest->kind != KB_MGMT_ROOT_MANIFEST ||
       manifest->phase != KB_MGMT_ROOT_FINAL || manifest->public_key_len != 32 ||
       !manifest->hwm2_attestation_len ||
       manifest->hwm2_attestation_len > KB_MGMT_ROOT_ATTEST_MAX || generation != 1 ||
       !candidate_id || !payload_digest || !payload || !payload_len || !signature)
      return KB_MGMT_JWKS_INTEGRITY;
   if (vault_hwm_read(manifest->custody_key_id, &live_version, live_attestation,
                      sizeof(live_attestation), &live_attestation_len))
   {
      OPENSSL_cleanse(live_attestation, sizeof(live_attestation));
      return KB_MGMT_JWKS_RETRY;
   }
   if (live_version != 2 || live_attestation_len != manifest->hwm2_attestation_len ||
       CRYPTO_memcmp(live_attestation, manifest->hwm2_attestation, live_attestation_len) ||
       vault_hwm_verify(manifest->custody_key_id, live_version, live_attestation,
                        live_attestation_len) ||
       !manifest->seal_epoch ||
       key_use_id(generation, manifest->seal_epoch, candidate_id, payload_digest, use_id))
   {
      OPENSSL_cleanse(live_attestation, sizeof(live_attestation));
      return KB_MGMT_JWKS_INTEGRITY;
   }
   OPENSSL_cleanse(live_attestation, sizeof(live_attestation));
   kb_mgmt_jwks_db_result_t dr = db2_management_jwks_manifest_key_admit(
       &ctx->db, use_id, generation, candidate_id, manifest, payload_digest, &admission);
   OPENSSL_cleanse(use_id, sizeof(use_id));
   if (dr != KB_MGMT_JWKS_DB_OK)
      return dr == KB_MGMT_JWKS_DB_SEALED      ? KB_MGMT_JWKS_SEALED
             : dr == KB_MGMT_JWKS_DB_CONFLICT  ? KB_MGMT_JWKS_CONFLICT
             : dr == KB_MGMT_JWKS_DB_INTEGRITY ? KB_MGMT_JWKS_INTEGRITY
                                               : KB_MGMT_JWKS_RETRY;
   if (admission.seal_epoch != manifest->seal_epoch ||
       admission.envelope.version != manifest->v2.version ||
       admission.envelope.ciphertext_len != manifest->v2.ciphertext_len ||
       admission.envelope.ciphertext_len > sizeof(admission.envelope.ciphertext) ||
       CRYPTO_memcmp(admission.envelope.wrapped_dek, manifest->v2.wrapped_dek,
                     sizeof(admission.envelope.wrapped_dek)) ||
       CRYPTO_memcmp(admission.envelope.nonce, manifest->v2.nonce,
                     sizeof(admission.envelope.nonce)) ||
       CRYPTO_memcmp(admission.envelope.ciphertext, manifest->v2.ciphertext,
                     admission.envelope.ciphertext_len) ||
       CRYPTO_memcmp(admission.envelope.tag, manifest->v2.tag, sizeof(admission.envelope.tag)) ||
       admission.hwm_attestation_len != manifest->hwm2_attestation_len ||
       CRYPTO_memcmp(admission.hwm_attestation, manifest->hwm2_attestation,
                     admission.hwm_attestation_len))
   {
      OPENSSL_cleanse(&admission, sizeof(admission));
      return KB_MGMT_JWKS_INTEGRITY;
   }
   vault_maintenance_guard_t *guard = NULL;
   sign_call_t call = {.admission = &admission,
                       .manifest = manifest,
                       .payload = payload,
                       .payload_len = payload_len,
                       .signature = signature,
                       .result = KB_MGMT_JWKS_RETRY};
   if (vault_maintenance_guard_begin(&guard) != VAULT_MAINTENANCE_OK)
      goto done;
   if (vault_maintenance_guard_sync_primary_epoch(guard, admission.seal_epoch) !=
           VAULT_MAINTENANCE_OK ||
       vault_maintenance_guard_unseal(guard, NULL, 0) != VAULT_MAINTENANCE_OK)
      goto guard_done;
   int rc = vault_maintenance_guard_with_active_kek(guard, sign_with_kek, &call);
   if (rc == VAULT_MAINTENANCE_SEALED)
      call.result = KB_MGMT_JWKS_SEALED;
   else if (rc != VAULT_MAINTENANCE_OK && call.result == KB_MGMT_JWKS_FRESH)
      call.result = KB_MGMT_JWKS_RETRY;
guard_done:
   if (vault_maintenance_guard_end(&guard) != VAULT_MAINTENANCE_OK)
      call.result = KB_MGMT_JWKS_RETRY;
done:
   OPENSSL_cleanse(&admission, sizeof(admission));
   if (call.result != KB_MGMT_JWKS_FRESH)
      OPENSSL_cleanse(signature, KB_MGMT_JWKS_SIGNATURE_LEN);
   return call.result;
}

static kb_mgmt_jwks_hwm_result_t hwm_read_cb(void *opaque, const char *id, uint64_t *version,
                                             uint8_t *attestation, size_t cap, size_t *len)
{
   (void)opaque;
   return vault_hwm_read(id, version, attestation, cap, len) == 0 ? KB_MGMT_JWKS_HWM_OK
                                                                  : KB_MGMT_JWKS_HWM_RETRY;
}

static kb_mgmt_jwks_hwm_result_t hwm_cas_cb(void *opaque, const char *id, uint64_t expected,
                                            uint64_t next, uint8_t *attestation, size_t cap,
                                            size_t *len)
{
   (void)opaque;
   return vault_hwm_cas(id, expected, next, attestation, cap, len) == 0 ? KB_MGMT_JWKS_HWM_OK
                                                                        : KB_MGMT_JWKS_HWM_RETRY;
}

static int hwm_verify_cb(void *opaque, const char *id, uint64_t version, const uint8_t *attestation,
                         size_t len)
{
   (void)opaque;
   return vault_hwm_verify(id, version, attestation, len);
}

static int publish_output(const char *artifact, size_t len)
{
   return artifact && len && len < KB_MGMT_JWKS_ENVELOPE_MAX &&
                  fwrite(artifact, 1, len, stdout) == len && fputc('\n', stdout) != EOF &&
                  fflush(stdout) == 0
              ? 0
              : -1;
}

int main(int argc, char **argv)
{
   int export_only = argc == 2 && strcmp(argv[1], "--export-public") == 0;
   kb_mgmt_jwks_config_t config;
   memset(&config, 0, sizeof(config));
   if (!export_only)
   {
      if (argc != 5 || strcmp(argv[1], "--valid-from") || strcmp(argv[3], "--valid-until") ||
          canonical_time(argv[2], &config.valid_from) ||
          canonical_time(argv[4], &config.valid_until))
      {
         fixed_error("usage");
         return EXIT_USAGE;
      }
      struct timespec now;
      if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0)
      {
         fixed_error("configuration");
         return EXIT_CONFIGURATION;
      }
      config.now = now.tv_sec;
      config.clock_skew_seconds = PUBLISH_CLOCK_SKEW_SECONDS;
      config.maximum_lifetime_seconds = PUBLISH_MAX_LIFETIME;
   }
   const char *harden_failure = kb_mgmt_offline_harden_process();
   if (harden_failure)
   {
      fixed_error(harden_failure);
      return EXIT_HARDENING;
   }

   int exit_code = 0, saved_stderr = -1, stderr_silenced = 0, db_open = 0;
   int provider_unsealed = 0;
   publisher_ctx_t publisher;
   kb_mgmt_jwks_callbacks_t callbacks;
   char artifact[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t artifact_len = 0;
   uint8_t identity_digest[32];
   memset(&publisher, 0, sizeof(publisher));
   memset(&callbacks, 0, sizeof(callbacks));
   memset(artifact, 0, sizeof(artifact));
   memset(identity_digest, 0, sizeof(identity_digest));

   char *db_url = copy_env("AIMEE_KB_JWKS_PUBLISH_DSN", 4096);
   char *helper = copy_env("AIMEE_VAULT_KMS_HELPER", 128);
   char *kek_id = copy_env("AIMEE_VAULT_KMS_KEY_ID", KB_MGMT_ROOT_CUSTODY_ID_MAX);
   char *hwm_public = copy_env("AIMEE_VAULT_KMS_HWM_PUBKEY", 4096);
   char *hwm_domain = copy_env("AIMEE_VAULT_KMS_HWM_DOMAIN", 128);
   if (!db_url || !helper || !kek_id || !hwm_public || !hwm_domain ||
       !root_owned_fixed_file(helper, 1) || !root_owned_fixed_file(hwm_public, 0) ||
       public_identity_digest(hwm_public, identity_digest))
   {
      exit_code = EXIT_CONFIGURATION;
      goto done;
   }
   if (clearenv() || setenv("AIMEE_VAULT_KMS_HELPER", helper, 1) ||
       setenv("AIMEE_VAULT_KMS_KEY_ID", kek_id, 1) ||
       setenv("AIMEE_VAULT_KMS_HWM_PUBKEY", hwm_public, 1) ||
       setenv("AIMEE_VAULT_KMS_HWM_DOMAIN", hwm_domain, 1))
   {
      exit_code = EXIT_HARDENING;
      goto done;
   }
   if (silence_stderr(&saved_stderr))
   {
      exit_code = EXIT_HARDENING;
      goto done;
   }
   stderr_silenced = 1;
   char db_error[256] = "";
   if (db2_management_jwks_publication_open(&publisher.db, db_url, db_error, sizeof(db_error)))
   {
      OPENSSL_cleanse(db_error, sizeof(db_error));
      exit_code = EXIT_DATABASE;
      goto provider_done;
   }
   db_open = 1;
   if (db2_management_jwks_publication_set_provider_binding(&publisher.db, helper, hwm_domain,
                                                            identity_digest) ||
       db2_management_jwks_publication_bind(&publisher.db, &callbacks))
   {
      OPENSSL_cleanse(db_error, sizeof(db_error));
      exit_code = EXIT_DATABASE;
      goto provider_done;
   }
   OPENSSL_cleanse(db_error, sizeof(db_error));
   callbacks.ctx = &publisher;
   callbacks.hwm_read = hwm_read_cb;
   callbacks.hwm_cas = hwm_cas_cb;
   callbacks.hwm_verify = hwm_verify_cb;
   callbacks.protected_sign = protected_sign;
   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0))
   {
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   provider_unsealed = 1;
   if (vault_custody_kms_hwm_refresh() || vault_is_sealed() != 0 || !vault_custody_kms_hwm_ready())
   {
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   kb_mgmt_jwks_result_t result =
       export_only
           ? kb_mgmt_jwks_export(&callbacks, artifact, sizeof(artifact), &artifact_len)
           : kb_mgmt_jwks_publish(&config, &callbacks, artifact, sizeof(artifact), &artifact_len);
   if (vault_seal())
   {
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   provider_unsealed = 0;
   switch (result)
   {
   case KB_MGMT_JWKS_FRESH:
      if (export_only)
         exit_code = EXIT_INTEGRITY;
      break;
   case KB_MGMT_JWKS_RECOVERED:
      if (export_only)
         exit_code = EXIT_INTEGRITY;
      break;
   case KB_MGMT_JWKS_CONVERGED:
      break;
   case KB_MGMT_JWKS_RETRY:
      exit_code = EXIT_RETRY;
      break;
   case KB_MGMT_JWKS_SEALED:
      exit_code = EXIT_SEALED;
      break;
   case KB_MGMT_JWKS_CONFLICT:
      exit_code = EXIT_CONFLICT;
      break;
   case KB_MGMT_JWKS_INTEGRITY:
   default:
      exit_code = EXIT_INTEGRITY;
      break;
   }
   if (!exit_code && artifact_len && publish_output(artifact, artifact_len))
      exit_code = EXIT_OUTPUT;

provider_done:
   if (provider_unsealed && vault_seal())
      exit_code = EXIT_CUSTODY;
   if (db_open)
      db2_management_jwks_publication_close(&publisher.db);
   if (stderr_silenced && restore_stderr(&saved_stderr))
      exit_code = EXIT_HARDENING;
done:
   if (exit_code)
      fixed_error(exit_class(exit_code));
   OPENSSL_cleanse(artifact, sizeof(artifact));
   OPENSSL_cleanse(identity_digest, sizeof(identity_digest));
   OPENSSL_cleanse(&callbacks, sizeof(callbacks));
   OPENSSL_cleanse(&publisher, sizeof(publisher));
   char *private_values[] = {db_url, helper, kek_id, hwm_public, hwm_domain};
   for (size_t i = 0; i < sizeof(private_values) / sizeof(private_values[0]); ++i)
   {
      if (private_values[i])
      {
         OPENSSL_cleanse(private_values[i], strlen(private_values[i]));
         free(private_values[i]);
      }
   }
   if (munlockall())
   {
      if (!exit_code)
         fixed_error("hardening");
      exit_code = EXIT_HARDENING;
   }
   return exit_code;
}
