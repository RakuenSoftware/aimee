/* Offline, owner-only management-token trust-root provisioner. This translation
 * unit and its private custody/DB adapter closure must never enter a runtime
 * service or installation target. */
#include "kb_mgmt_token_roots_provision.h"
#include "management_token_roots.h"
#include "vault_custody_kms.h"
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
   (void)fprintf(stderr, "aimee-kb-token-roots-provision: %s\n", error_class);
}

static const char *exit_error_class(int exit_code)
{
   switch (exit_code)
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

static int fixed_text(const char *s, size_t max)
{
   if (!s || !*s || strnlen(s, max + 1) > max)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static char *copy_env(const char *name, size_t max)
{
   const char *value = getenv(name);
   return fixed_text(value, max) ? strdup(value) : NULL;
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
      parent[1] = 0;
   else
      *slash = 0;
   for (;;)
   {
      if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
         return 0;
      if (parent[0] == '/' && parent[1] == 0)
         return 1;
      slash = strrchr(parent, '/');
      if (!slash)
         return 0;
      if (slash == parent)
         parent[1] = 0;
      else
         *slash = 0;
   }
}

static int harden_process(void)
{
   struct rlimit no_core = {0, 0};
   (void)umask(077);
   if (setrlimit(RLIMIT_CORE, &no_core) != 0 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 ||
       prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
      return -1;
#ifdef PR_GET_DUMPABLE
   if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
   {
      (void)munlockall();
      return -1;
   }
#endif
   return 0;
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

static int publish_bundle(const char *bundle, size_t bundle_len)
{
   return bundle && bundle_len && bundle_len < KB_MGMT_PUBLIC_BUNDLE_MAX &&
                  kb_mgmt_public_bundle_validate(bundle, bundle_len) == 0 &&
                  fwrite(bundle, 1, bundle_len, stdout) == bundle_len &&
                  fputc('\n', stdout) != EOF && fflush(stdout) == 0
              ? 0
              : -1;
}

int main(int argc, char **argv)
{
   int export_only = argc == 2 && strcmp(argv[1], "--export-public") == 0;
   if (argc != 1 && !export_only)
   {
      fixed_error("usage");
      return EXIT_USAGE;
   }
   if (harden_process() != 0)
   {
      fixed_error("hardening");
      return EXIT_HARDENING;
   }

   int exit_code = 0, saved_stderr = -1, db_open = 0, stderr_silenced = 0;
   int custody_unsealed = 0;
   db2_management_token_roots_ctx_t db_ctx;
   kb_mgmt_roots_db_t db;
   kb_mgmt_roots_config_t config;
   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_len = 0;
   memset(&db_ctx, 0, sizeof(db_ctx));
   memset(&db, 0, sizeof(db));
   memset(&config, 0, sizeof(config));
   memset(bundle, 0, sizeof(bundle));

   char *db_url = copy_env("AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN", 4096);
   char *helper = copy_env("AIMEE_VAULT_KMS_HELPER", 128);
   char *kek_id = copy_env("AIMEE_VAULT_KMS_KEY_ID", KB_MGMT_ROOT_CUSTODY_ID_MAX);
   char *hwm_public = copy_env("AIMEE_VAULT_KMS_HWM_PUBKEY", 4096);
   char *hwm_domain = copy_env("AIMEE_VAULT_KMS_HWM_DOMAIN", 128);
   char *token_id = copy_env("AIMEE_KB_TOKEN_ROOT_CUSTODY_ID", KB_MGMT_ROOT_CUSTODY_ID_MAX);
   char *manifest_id =
       copy_env("AIMEE_KB_JWKS_MANIFEST_ROOT_CUSTODY_ID", KB_MGMT_ROOT_CUSTODY_ID_MAX);
   char *publication_id =
       copy_env("AIMEE_KB_JWKS_PUBLICATION_HWM_CUSTODY_ID", KB_MGMT_ROOT_CUSTODY_ID_MAX);
   config.token_custody_key_id = token_id;
   config.manifest_custody_key_id = manifest_id;
   config.publication_custody_key_id = publication_id;
   config.publication_helper = helper;
   config.publication_verifier_domain = hwm_domain;
   if (!db_url || !helper || !kek_id || !hwm_public || !hwm_domain || !token_id || !manifest_id ||
       !publication_id || !root_owned_fixed_file(helper, 1) ||
       !root_owned_fixed_file(hwm_public, 0) ||
       public_identity_digest(hwm_public, config.publication_identity_digest) != 0)
   {
      fixed_error("configuration");
      exit_code = EXIT_CONFIGURATION;
      goto done;
   }

   /* The KMS provider resolves its helper configuration with getenv(). Keep
    * only that exact allowlist before any path can fork the helper; libpq and
    * the provisioner core use the private copies above. */
   if (clearenv() != 0 || setenv("AIMEE_VAULT_KMS_HELPER", helper, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_KEY_ID", kek_id, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_PUBKEY", hwm_public, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_DOMAIN", hwm_domain, 1) != 0)
   {
      fixed_error("hardening");
      exit_code = EXIT_HARDENING;
      goto done;
   }

   if (silence_stderr(&saved_stderr) != 0)
   {
      fixed_error("hardening");
      exit_code = EXIT_HARDENING;
      goto done;
   }
   stderr_silenced = 1;
   char db_error[256] = "";
   if (db2_management_token_roots_open(&db_ctx, db_url, db_error, sizeof(db_error)) != 0)
   {
      OPENSSL_cleanse(db_error, sizeof(db_error));
      exit_code = EXIT_DATABASE;
      goto provider_done;
   }
   db_open = 1;
   if (db2_management_token_roots_bind(&db_ctx, &db) != 0)
   {
      OPENSSL_cleanse(db_error, sizeof(db_error));
      exit_code = EXIT_DATABASE;
      goto provider_done;
   }
   OPENSSL_cleanse(db_error, sizeof(db_error));
   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0) != 0)
   {
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   custody_unsealed = 1;
   if (vault_custody_kms_hwm_refresh() != 0 || vault_is_sealed() != 0 ||
       !vault_custody_kms_hwm_ready())
   {
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   kb_mgmt_roots_result_t result =
       export_only
           ? kb_mgmt_token_roots_export(&config, &db, bundle, sizeof(bundle), &bundle_len)
           : kb_mgmt_token_roots_provision(&config, &db, bundle, sizeof(bundle), &bundle_len);
   if (vault_seal() != 0)
   {
      OPENSSL_cleanse(bundle, sizeof(bundle));
      bundle_len = 0;
      exit_code = EXIT_CUSTODY;
      goto provider_done;
   }
   custody_unsealed = 0;
   switch (result)
   {
   case KB_MGMT_ROOTS_FRESH:
      if (export_only)
         exit_code = EXIT_INTEGRITY;
      break;
   case KB_MGMT_ROOTS_FINAL:
      if (!export_only)
         bundle_len = 0;
      break;
   case KB_MGMT_ROOTS_RECOVERED:
      bundle_len = 0;
      break;
   case KB_MGMT_ROOTS_RETRY:
      exit_code = EXIT_RETRY;
      break;
   case KB_MGMT_ROOTS_SEALED:
      exit_code = EXIT_SEALED;
      break;
   case KB_MGMT_ROOTS_CONFLICT:
      exit_code = EXIT_CONFLICT;
      break;
   case KB_MGMT_ROOTS_INTEGRITY:
   default:
      exit_code = EXIT_INTEGRITY;
      break;
   }
   if (!exit_code && bundle_len && publish_bundle(bundle, bundle_len) != 0)
      exit_code = EXIT_OUTPUT;

provider_done:
   if (custody_unsealed && vault_seal() != 0)
      exit_code = EXIT_CUSTODY;
   if (db_open)
      db2_management_token_roots_close(&db_ctx);
   if (stderr_silenced && restore_stderr(&saved_stderr) != 0)
      exit_code = EXIT_HARDENING;
   if (exit_code)
      fixed_error(exit_error_class(exit_code));
done:
   OPENSSL_cleanse(bundle, sizeof(bundle));
   OPENSSL_cleanse(&config, sizeof(config));
   OPENSSL_cleanse(&db, sizeof(db));
   OPENSSL_cleanse(&db_ctx, sizeof(db_ctx));
   char *private_values[] = {db_url,     helper,   kek_id,      hwm_public,
                             hwm_domain, token_id, manifest_id, publication_id};
   for (size_t i = 0; i < sizeof(private_values) / sizeof(private_values[0]); ++i)
   {
      if (private_values[i])
      {
         OPENSSL_cleanse(private_values[i], strlen(private_values[i]));
         free(private_values[i]);
      }
   }
   if (munlockall() != 0)
   {
      if (!exit_code)
         fixed_error("hardening");
      exit_code = EXIT_HARDENING;
   }
   return exit_code;
}
