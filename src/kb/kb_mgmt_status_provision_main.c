/* Offline, owner-only management-status key provisioner.  This translation unit
 * is intentionally separate from kb_main and must never be linked into a runtime
 * service. */
#include "kb_mgmt_status_provision.h"
#include "management_status_provision.h"
#include "vault_custody_kms.h"
#include "vault_internal.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
   db2_management_status_provision_ctx_t db;
} provision_db_ctx_t;

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
   (void)fprintf(stderr, "aimee-kb-status-provision: %s\n", error_class);
}

static int text_valid(const char *s, size_t max)
{
   if (!s || !*s || strnlen(s, max + 1) > max)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
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

/* Returns NULL on success, or a FIXED name for the step that failed — see the
 * note in kb_mgmt_token_roots_provision_main.c. One undifferentiated
 * "hardening" left an operator with nothing to change. The names are
 * compile-time constants naming a syscall and leak nothing. */
static const char *harden_process(void)
{
   struct rlimit no_core = {0, 0};
   (void)umask(077);
   if (setrlimit(RLIMIT_CORE, &no_core) != 0)
      return "hardening (core-dump limit)";
   if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
      return "hardening (dumpable)";
   if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
      return "hardening (no-new-privs)";
#ifdef PR_GET_DUMPABLE
   if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
      return "hardening (dumpable readback)";
#endif
   return NULL;
}

/* The KMS helper is trusted code, but its diagnostics are provider-controlled
 * and may contain opaque service output.  Suppress all library/helper stderr
 * while work is in flight, then restore it before emitting one fixed class. */
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

static int copy_envelope_to_db(db2_vault_key_use_envelope_t *dst,
                               const kb_mgmt_status_provision_envelope_t *src, uint64_t seal_epoch)
{
   if (!dst || !src || src->ciphertext_len != 32 || (src->version != 1 && src->version != 2) ||
       seal_epoch > INT64_MAX)
      return -1;
   memset(dst, 0, sizeof(*dst));
   dst->seal_epoch = (int64_t)seal_epoch;
   dst->version = src->version;
   memcpy(dst->wrapped_dek, src->wrapped_dek, sizeof(src->wrapped_dek));
   memcpy(dst->nonce, src->nonce, sizeof(src->nonce));
   memcpy(dst->ciphertext, src->ciphertext, src->ciphertext_len);
   dst->ciphertext_len = src->ciphertext_len;
   memcpy(dst->tag, src->tag, sizeof(src->tag));
   return 0;
}

static int copy_envelope_from_db(kb_mgmt_status_provision_envelope_t *dst,
                                 const db2_vault_key_use_envelope_t *src, int64_t version)
{
   if (!dst || !src || src->version != version || src->ciphertext_len != 32)
      return -1;
   memset(dst, 0, sizeof(*dst));
   dst->version = version;
   memcpy(dst->wrapped_dek, src->wrapped_dek, sizeof(dst->wrapped_dek));
   memcpy(dst->nonce, src->nonce, sizeof(dst->nonce));
   memcpy(dst->ciphertext, src->ciphertext, src->ciphertext_len);
   dst->ciphertext_len = src->ciphertext_len;
   memcpy(dst->tag, src->tag, sizeof(dst->tag));
   return 0;
}

static int db_to_core(const db2_management_status_provision_record_t *src,
                      kb_mgmt_status_provision_record_t *dst)
{
   if (!src || !dst || src->seal_epoch <= 0)
      return -1;
   memset(dst, 0, sizeof(*dst));
   dst->seal_epoch = (uint64_t)src->seal_epoch;
   if (strcmp(src->state, "empty") == 0)
   {
      if (src->enabled || src->from_version != 1 || src->to_version != 2 ||
          !text_valid(src->custody_key_id, KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX))
         return -1;
      dst->phase = KB_MGMT_STATUS_PROVISION_EMPTY;
      return 0;
   }
   int activated = strcmp(src->state, "activated") == 0;
   if ((!activated && strcmp(src->state, "staged") != 0 && strcmp(src->state, "probed") != 0 &&
        strcmp(src->state, "activating") != 0) ||
       src->enabled != activated || src->rotation_id <= 0 || src->from_version != 1 ||
       src->to_version != 2 || !text_valid(src->bootstrap_id, 64) ||
       !text_valid(src->custody_key_id, KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX) ||
       !text_valid(src->wire_key_id, KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX) ||
       src->v1.hwm_attestation_len != 64 || copy_envelope_from_db(&dst->v1, &src->v1, 1) != 0 ||
       copy_envelope_from_db(&dst->v2, &src->v2, 2) != 0)
      return -1;
   dst->phase = activated ? KB_MGMT_STATUS_PROVISION_ENABLED : KB_MGMT_STATUS_PROVISION_STAGED;
   memcpy(dst->bootstrap_id, src->bootstrap_id, sizeof(dst->bootstrap_id));
   memcpy(dst->custody_key_id, src->custody_key_id, sizeof(dst->custody_key_id));
   memcpy(dst->wire_key_id, src->wire_key_id, sizeof(dst->wire_key_id));
   memcpy(dst->public_key, src->public_key, sizeof(dst->public_key));
   memcpy(dst->public_key_digest, src->public_key_digest, sizeof(dst->public_key_digest));
   memcpy(dst->v1_digest, src->v1_envelope_digest, sizeof(dst->v1_digest));
   memcpy(dst->v2_digest, src->v2_envelope_digest, sizeof(dst->v2_digest));
   memcpy(dst->hwm1_attestation, src->v1.hwm_attestation, 64);
   dst->hwm1_attestation_len = 64;
   if (activated)
   {
      if (src->v2.hwm_attestation_len != 64)
         return -1;
      memcpy(dst->hwm2_attestation, src->v2.hwm_attestation, 64);
      dst->hwm2_attestation_len = 64;
   }
   return 0;
}

static int core_to_db(const kb_mgmt_status_provision_record_t *src,
                      db2_management_status_provision_record_t *dst)
{
   if (!src || !dst || src->phase != KB_MGMT_STATUS_PROVISION_STAGED || !src->seal_epoch ||
       src->seal_epoch > INT64_MAX || src->hwm1_attestation_len != 64)
      return -1;
   memset(dst, 0, sizeof(*dst));
   memcpy(dst->bootstrap_id, src->bootstrap_id, sizeof(dst->bootstrap_id));
   memcpy(dst->custody_key_id, src->custody_key_id, sizeof(dst->custody_key_id));
   memcpy(dst->wire_key_id, src->wire_key_id, sizeof(dst->wire_key_id));
   memcpy(dst->public_key, src->public_key, sizeof(dst->public_key));
   memcpy(dst->public_key_digest, src->public_key_digest, sizeof(dst->public_key_digest));
   memcpy(dst->v1_envelope_digest, src->v1_digest, sizeof(dst->v1_envelope_digest));
   memcpy(dst->v2_envelope_digest, src->v2_digest, sizeof(dst->v2_envelope_digest));
   dst->seal_epoch = (int64_t)src->seal_epoch;
   dst->from_version = 1;
   dst->to_version = 2;
   memcpy(dst->state, "staged", sizeof("staged"));
   if (copy_envelope_to_db(&dst->v1, &src->v1, src->seal_epoch) != 0 ||
       copy_envelope_to_db(&dst->v2, &src->v2, src->seal_epoch) != 0)
      return -1;
   memcpy(dst->v1.hwm_attestation, src->hwm1_attestation, 64);
   dst->v1.hwm_attestation_len = 64;
   return 0;
}

static kb_mgmt_status_provision_db_result_t inspect_cb(void *opaque, const char *custody_key_id,
                                                       kb_mgmt_status_provision_record_t *record)
{
   provision_db_ctx_t *ctx = opaque;
   db2_management_status_provision_record_t db_record;
   memset(&db_record, 0, sizeof(db_record));
   int rc = db2_management_status_provision_inspect(&ctx->db, custody_key_id, &db_record);
   if (rc != 0)
   {
      OPENSSL_cleanse(&db_record, sizeof(db_record));
      return KB_MGMT_STATUS_PROVISION_DB_RETRY;
   }
   rc = db_to_core(&db_record, record);
   OPENSSL_cleanse(&db_record, sizeof(db_record));
   return rc == 0 ? KB_MGMT_STATUS_PROVISION_DB_OK : KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
}

static kb_mgmt_status_provision_db_result_t
stage_cb(void *opaque, const kb_mgmt_status_provision_record_t *record)
{
   provision_db_ctx_t *ctx = opaque;
   db2_management_status_provision_record_t db_record;
   int64_t rotation_id = 0, seal_epoch = 0;
   if (core_to_db(record, &db_record) != 0)
      return KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
   int rc = db2_management_status_provision_stage(&ctx->db, &db_record, &rotation_id, &seal_epoch);
   OPENSSL_cleanse(&db_record, sizeof(db_record));
   if (rc != 0)
      return KB_MGMT_STATUS_PROVISION_DB_RETRY;
   return rotation_id > 0 && seal_epoch > 0 && (uint64_t)seal_epoch == record->seal_epoch
              ? KB_MGMT_STATUS_PROVISION_DB_OK
              : KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
}

static kb_mgmt_status_provision_db_result_t
prepare_cb(void *opaque, const kb_mgmt_status_provision_record_t *record)
{
   provision_db_ctx_t *ctx = opaque;
   int64_t rotation_id = 0, expected = 0, next = 0;
   if (!record || !text_valid(record->bootstrap_id, 64))
      return KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
   if (db2_management_status_provision_prepare_activation(&ctx->db, record->bootstrap_id,
                                                          &rotation_id, &expected, &next) != 0)
      return KB_MGMT_STATUS_PROVISION_DB_RETRY;
   return rotation_id > 0 && expected == 1 && next == 2 ? KB_MGMT_STATUS_PROVISION_DB_OK
                                                        : KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
}

static int fixed_final_record(const kb_mgmt_status_provision_record_t *expected,
                              const db2_management_status_provision_record_t *actual,
                              const uint8_t *attestation, size_t attestation_len)
{
   kb_mgmt_status_provision_record_t converted;
   memset(&converted, 0, sizeof(converted));
   int ok = attestation_len == 64 && actual->v2.hwm_attestation_len == 64 &&
            CRYPTO_memcmp(actual->v2.hwm_attestation, attestation, 64) == 0 &&
            db_to_core(actual, &converted) == 0 &&
            converted.phase == KB_MGMT_STATUS_PROVISION_ENABLED &&
            strcmp(converted.bootstrap_id, expected->bootstrap_id) == 0 &&
            strcmp(converted.custody_key_id, expected->custody_key_id) == 0 &&
            strcmp(converted.wire_key_id, expected->wire_key_id) == 0 &&
            converted.seal_epoch == expected->seal_epoch &&
            CRYPTO_memcmp(converted.public_key, expected->public_key, 32) == 0 &&
            CRYPTO_memcmp(converted.public_key_digest, expected->public_key_digest, 32) == 0 &&
            CRYPTO_memcmp(converted.v1_digest, expected->v1_digest, 32) == 0 &&
            CRYPTO_memcmp(converted.v2_digest, expected->v2_digest, 32) == 0;
   OPENSSL_cleanse(&converted, sizeof(converted));
   return ok;
}

static kb_mgmt_status_provision_db_result_t
finalize_cb(void *opaque, const kb_mgmt_status_provision_record_t *record,
            const uint8_t *hwm_attestation, size_t hwm_attestation_len)
{
   provision_db_ctx_t *ctx = opaque;
   db2_management_status_provision_record_t final_record;
   memset(&final_record, 0, sizeof(final_record));
   if (!record || !hwm_attestation || hwm_attestation_len != 64)
      return KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
   int rc = db2_management_status_provision_finalize(&ctx->db, record->bootstrap_id,
                                                     hwm_attestation, &final_record);
   if (rc != 0)
   {
      OPENSSL_cleanse(&final_record, sizeof(final_record));
      return KB_MGMT_STATUS_PROVISION_DB_RETRY;
   }
   int ok = fixed_final_record(record, &final_record, hwm_attestation, hwm_attestation_len);
   OPENSSL_cleanse(&final_record, sizeof(final_record));
   return ok ? KB_MGMT_STATUS_PROVISION_DB_OK : KB_MGMT_STATUS_PROVISION_DB_INTEGRITY;
}

static int base64url_public(const uint8_t public_key[32], char out[44])
{
   unsigned char encoded[45];
   int n = EVP_EncodeBlock(encoded, public_key, 32);
   if (n != 44 || encoded[43] != '=')
      return -1;
   for (int i = 0; i < 43; ++i)
   {
      if (encoded[i] == '+')
         encoded[i] = '-';
      else if (encoded[i] == '/')
         encoded[i] = '_';
   }
   memcpy(out, encoded, 43);
   out[43] = 0;
   OPENSSL_cleanse(encoded, sizeof(encoded));
   return 0;
}

static int json_string(FILE *stream, const char *s)
{
   if (fputc('"', stream) == EOF)
      return -1;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
   {
      if ((*p == '"' || *p == '\\') && fputc('\\', stream) == EOF)
         return -1;
      if (fputc(*p, stream) == EOF)
         return -1;
   }
   return fputc('"', stream) == EOF ? -1 : 0;
}

static int publish_fresh(const kb_mgmt_status_provision_output_t *output)
{
   char public_b64[44];
   if (!output || !text_valid(output->custody_key_id, KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX) ||
       !text_valid(output->wire_key_id, KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX) ||
       base64url_public(output->public_key, public_b64) != 0)
      return -1;
   int ok = fputs("{\"custody_key_id\":", stdout) >= 0 &&
            json_string(stdout, output->custody_key_id) == 0 &&
            fputs(",\"wire_key_id\":", stdout) >= 0 &&
            json_string(stdout, output->wire_key_id) == 0 &&
            fputs(",\"public_key_b64url\":\"", stdout) >= 0 && fputs(public_b64, stdout) >= 0 &&
            fputs("\"}\n", stdout) >= 0 && fflush(stdout) == 0;
   OPENSSL_cleanse(public_b64, sizeof(public_b64));
   return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
   (void)argv;
   if (argc != 1)
   {
      fixed_error("usage");
      return EXIT_USAGE;
   }
   const char *harden_failure = harden_process();
   if (harden_failure)
   {
      fixed_error(harden_failure);
      return EXIT_HARDENING;
   }

   const char *db_url = getenv("AIMEE_KB_STATUS_PROVISION_DSN");
   const char *helper = getenv("AIMEE_VAULT_KMS_HELPER");
   const char *custody_key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   const char *hwm_public = getenv("AIMEE_VAULT_KMS_HWM_PUBKEY");
   const char *hwm_domain = getenv("AIMEE_VAULT_KMS_HWM_DOMAIN");
   char hwm_message[512];
   int hwm_n = custody_key_id && hwm_domain
                   ? snprintf(hwm_message, sizeof(hwm_message), "aimee-hwm-v1|%s|%llu|%s",
                              custody_key_id, (unsigned long long)UINT64_MAX, hwm_domain)
                   : -1;
   OPENSSL_cleanse(hwm_message, sizeof(hwm_message));
   if (!text_valid(db_url, 4096) ||
       !text_valid(custody_key_id, KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX) ||
       !text_valid(hwm_domain, 256) || hwm_n < 0 || (size_t)hwm_n >= sizeof(hwm_message) ||
       !root_owned_fixed_file(helper, 1) || !root_owned_fixed_file(hwm_public, 0))
   {
      fixed_error("configuration");
      return EXIT_CONFIGURATION;
   }

   provision_db_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   char db_error[256] = "";
   int saved_stderr = -1;
   if (silence_stderr(&saved_stderr) != 0)
   {
      fixed_error("hardening");
      return EXIT_HARDENING;
   }
   if (db2_management_status_provision_open(&ctx.db, db_url, db_error, sizeof(db_error)) != 0)
   {
      OPENSSL_cleanse(db_error, sizeof(db_error));
      if (restore_stderr(&saved_stderr) != 0)
         return EXIT_HARDENING;
      fixed_error("database");
      return EXIT_DATABASE;
   }
   OPENSSL_cleanse(db_error, sizeof(db_error));
   /* The already-open libpq connection owns its copy.  Do not pass database
    * credentials through the environment of the subsequently executed helper. */
   if (unsetenv("AIMEE_KB_STATUS_PROVISION_DSN") != 0)
   {
      db2_management_status_provision_close(&ctx.db);
      if (restore_stderr(&saved_stderr) != 0)
         return EXIT_HARDENING;
      fixed_error("hardening");
      return EXIT_HARDENING;
   }

   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0) != 0 || vault_custody_kms_hwm_refresh() != 0 ||
       vault_is_sealed() != 0 || !vault_custody_kms_hwm_ready())
   {
      db2_management_status_provision_close(&ctx.db);
      if (restore_stderr(&saved_stderr) != 0)
         return EXIT_HARDENING;
      fixed_error("custody");
      return EXIT_CUSTODY;
   }

   kb_mgmt_status_provision_db_t seam = {.inspect = inspect_cb,
                                         .stage = stage_cb,
                                         .prepare_activation = prepare_cb,
                                         .finalize = finalize_cb,
                                         .ctx = &ctx};
   kb_mgmt_status_provision_output_t output;
   memset(&output, 0, sizeof(output));
   kb_mgmt_status_provision_result_t result =
       kb_mgmt_status_provision(custody_key_id, &seam, &output);
   int sealed = vault_seal();
   db2_management_status_provision_close(&ctx.db);
   if (restore_stderr(&saved_stderr) != 0)
   {
      OPENSSL_cleanse(&output, sizeof(output));
      return EXIT_HARDENING;
   }
   if (sealed != 0)
   {
      OPENSSL_cleanse(&output, sizeof(output));
      fixed_error("custody");
      return EXIT_CUSTODY;
   }

   int exit_code = 0;
   switch (result)
   {
   case KB_MGMT_STATUS_PROVISION_FRESH:
      if (publish_fresh(&output) != 0)
      {
         fixed_error("output");
         exit_code = EXIT_OUTPUT;
      }
      break;
   case KB_MGMT_STATUS_PROVISION_RECOVERED:
      break;
   case KB_MGMT_STATUS_PROVISION_CONFLICT:
      fixed_error("conflict");
      exit_code = EXIT_CONFLICT;
      break;
   case KB_MGMT_STATUS_PROVISION_RETRY:
      fixed_error("retry");
      exit_code = EXIT_RETRY;
      break;
   case KB_MGMT_STATUS_PROVISION_SEALED:
      fixed_error("sealed");
      exit_code = EXIT_SEALED;
      break;
   case KB_MGMT_STATUS_PROVISION_INTEGRITY:
   default:
      fixed_error("integrity");
      exit_code = EXIT_INTEGRITY;
      break;
   }
   OPENSSL_cleanse(&output, sizeof(output));
   return exit_code;
}
