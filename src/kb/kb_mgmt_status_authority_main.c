/* Dedicated online management-status authority. This file intentionally owns
 * main() and is linked only by the hand-maintained authority target. */
#include "kb_mgmt_status_authority.h"
#include "kb_mgmt_status_custody.h"
#include "kb_mgmt_status_listener.h"
#include "management_status_key.h"
#include "management_status_runtime.h"
#include "vault_custody_kms.h"
#include "vault_internal.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
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

typedef struct
{
   db2_management_status_runtime_t lookup;
   db2_management_status_key_ctx_t key;
   kb_mgmt_status_custody_t custody;
} status_worker_t;

typedef struct
{
   status_worker_t workers[KB_MGMT_STATUS_LISTENER_WORKERS];
   char custody_key_id[601];
   char wire_key_id[65];
} status_service_t;

static int g_status_kms_ready;

/* The ordinary KB policy object is deliberately excluded from this narrowly
 * linked process. Its sole status-specific predicate is reproduced here and
 * is true only after exact KMS startup validation below. */
int kb_vault_management_status_keys_allowed(void)
{
   return g_status_kms_ready && vault_is_sealed() == 0 && vault_custody_kms_hwm_ready();
}

static void fixed_error(const char *kind)
{
   (void)fprintf(stderr, "aimee-kb-status-authority: %s\n", kind);
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

static int root_owned_file(const char *path, int executable)
{
   struct stat st;
   if (!path || path[0] != '/' || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
       (st.st_mode & 022) || (executable && !(st.st_mode & 0111)))
      return 0;
   char parent[PATH_MAX];
   if (strlen(path) >= sizeof(parent))
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

static int process_harden(void)
{
   struct rlimit none = {0, 0};
   struct rlimit files = {128, 128};
   (void)umask(077);
   return setrlimit(RLIMIT_CORE, &none) != 0 || setrlimit(RLIMIT_NOFILE, &files) != 0 ||
                  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 ||
                  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
              ? -1
              : 0;
}

static int wire_id(const unsigned char public_key[32], char out[65])
{
   static const char hex[] = "0123456789abcdef";
   static const char prefix[] = "p5-status-v1-";
   unsigned char digest[32];
   unsigned int n = 0;
   if (EVP_Digest(public_key, 32, digest, &n, EVP_sha256(), NULL) != 1 || n != 32)
      return -1;
   memcpy(out, prefix, sizeof(prefix) - 1);
   size_t offset = sizeof(prefix) - 1;
   for (size_t i = 0; i < 16; ++i)
   {
      out[offset++] = hex[digest[i] >> 4];
      out[offset++] = hex[digest[i] & 15];
   }
   out[offset] = 0;
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static int lookup_cb(const char *issuer, const char *serial, const char *fingerprint,
                     const char *target, const char *purpose, int64_t *generation,
                     char *target_fingerprint, size_t target_fingerprint_len, void *opaque)
{
   db2_management_status_runtime_t *db = opaque;
   int rc =
       db2_management_status_runtime_lookup(db, issuer, serial, fingerprint, target, purpose,
                                            generation, target_fingerprint, target_fingerprint_len);
   if (rc == DB2_MANAGEMENT_STATUS_RUNTIME_OK)
      return KB_MGMT_STATUS_CALLBACK_OK;
   if (rc == DB2_MANAGEMENT_STATUS_RUNTIME_DENIED)
      return KB_MGMT_STATUS_CALLBACK_DENIED;
   if (rc == DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY)
      return KB_MGMT_STATUS_CALLBACK_INTEGRITY;
   return KB_MGMT_STATUS_CALLBACK_UNAVAILABLE;
}

static int sign_cb(kb_mgmt_status_t *status, void *opaque)
{
   kb_mgmt_status_custody_result_t rc = kb_mgmt_status_custody_sign(status, opaque);
   if (rc == KB_MGMT_STATUS_CUSTODY_OK)
      return KB_MGMT_STATUS_CALLBACK_OK;
   if (rc == KB_MGMT_STATUS_CUSTODY_CONFLICT)
      return KB_MGMT_STATUS_CALLBACK_CONFLICT;
   return rc == KB_MGMT_STATUS_CUSTODY_INTEGRITY ? KB_MGMT_STATUS_CALLBACK_INTEGRITY
                                                 : KB_MGMT_STATUS_CALLBACK_UNAVAILABLE;
}

static kb_mgmt_status_listener_result_t
handle_request(size_t worker_id, const kb_mgmt_status_peer_t *peer, const char *body,
               size_t body_len, char *response, size_t response_cap, void *opaque)
{
   status_service_t *service = opaque;
   kb_mgmt_status_request_t request;
   kb_mgmt_status_t status;
   memset(&request, 0, sizeof(request));
   memset(&status, 0, sizeof(status));
   if (!service || worker_id >= KB_MGMT_STATUS_LISTENER_WORKERS || !peer || !response)
      return KB_MGMT_STATUS_LISTENER_UNAVAILABLE;
   kb_mgmt_status_authority_result_t result =
       kb_mgmt_status_request_from_json(body, body_len, &request);
   if (result == KB_MGMT_STATUS_AUTHORITY_OK)
   {
      time_t now = time(NULL);
      if (now <= 0)
         result = KB_MGMT_STATUS_AUTHORITY_UNAVAILABLE;
      else
      {
         status_worker_t *worker = &service->workers[worker_id];
         result = kb_mgmt_status_authority_issue(
             &request, peer->issuer, peer->serial_norm, peer->fingerprint, service->wire_key_id,
             (uint64_t)now, lookup_cb, &worker->lookup, sign_cb, &worker->custody, &status);
         if (result == KB_MGMT_STATUS_AUTHORITY_OK &&
             kb_mgmt_status_to_json(&status, response, response_cap) != 0)
            result = KB_MGMT_STATUS_AUTHORITY_UNAVAILABLE;
      }
   }
   OPENSSL_cleanse(&request, sizeof(request));
   OPENSSL_cleanse(&status, sizeof(status));
   if (result != KB_MGMT_STATUS_AUTHORITY_OK)
      OPENSSL_cleanse(response, response_cap);
   switch (result)
   {
   case KB_MGMT_STATUS_AUTHORITY_OK:
      return KB_MGMT_STATUS_LISTENER_OK;
   case KB_MGMT_STATUS_AUTHORITY_INVALID:
      return KB_MGMT_STATUS_LISTENER_INVALID;
   case KB_MGMT_STATUS_AUTHORITY_DENIED:
      return KB_MGMT_STATUS_LISTENER_DENIED;
   case KB_MGMT_STATUS_AUTHORITY_CONFLICT:
      return KB_MGMT_STATUS_LISTENER_CONFLICT;
   default:
      return KB_MGMT_STATUS_LISTENER_UNAVAILABLE;
   }
}

static void service_close(status_service_t *service)
{
   if (!service)
      return;
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
   {
      if (service->workers[i].lookup.transaction_active)
         (void)db2_management_status_runtime_startup_end(&service->workers[i].lookup, 0);
      else if (service->workers[i].key.transaction_active)
         (void)db2_management_status_key_guard_end(&service->workers[i].key, 0);
      db2_management_status_key_ctx_close(&service->workers[i].key);
      db2_management_status_runtime_close(&service->workers[i].lookup);
   }
   OPENSSL_cleanse(service, sizeof(*service));
}

static int same_startup(const db2_management_status_runtime_startup_t *a,
                        const db2_management_status_runtime_startup_t *b)
{
   return a->seal_epoch == b->seal_epoch && a->sealed == b->sealed && a->enabled == b->enabled &&
          a->version == b->version && strcmp(a->custody_key_id, b->custody_key_id) == 0 &&
          strcmp(a->wire_key_id, b->wire_key_id) == 0 &&
          CRYPTO_memcmp(a->public_key, b->public_key, sizeof(a->public_key)) == 0 &&
          a->hwm_attestation_len == b->hwm_attestation_len &&
          CRYPTO_memcmp(a->hwm_attestation, b->hwm_attestation, a->hwm_attestation_len) == 0;
}

static int service_open(status_service_t *service, const char *dsn, const char *configured_key)
{
   char error[256] = "";
   int failure_class = -1;
   db2_management_status_runtime_startup_t baseline;
   memset(service, 0, sizeof(*service));
   memset(&baseline, 0, sizeof(baseline));
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
      if (db2_management_status_runtime_open(&service->workers[i].lookup, dsn, error,
                                             sizeof(error)) != 0 ||
          db2_management_status_key_ctx_borrow_hardened(&service->workers[i].key,
                                                        &service->workers[i].lookup) != 0)
         goto fail;

   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
   {
      db2_management_status_runtime_startup_t current;
      memset(&current, 0, sizeof(current));
      if (db2_management_status_runtime_startup_begin(&service->workers[i].lookup, &current) != 0)
      {
         failure_class = -2;
         goto fail;
      }
      int valid = current.enabled && current.version == 2 && current.seal_epoch > 0 &&
                  !current.sealed && strcmp(current.custody_key_id, configured_key) == 0 &&
                  current.wire_key_id[0] && current.hwm_attestation_len == 64 &&
                  (i == 0 || same_startup(&baseline, &current));
      if (i == 0)
         memcpy(&baseline, &current, sizeof(baseline));
      OPENSSL_cleanse(&current, sizeof(current));
      if (!valid)
      {
         failure_class = -2;
         goto fail;
      }
   }
   char derived[65];
   uint64_t hwm_version = 0;
   unsigned char attestation[512];
   size_t attestation_len = 0;
   if (wire_id(baseline.public_key, derived) != 0 || strcmp(derived, baseline.wire_key_id) != 0)
   {
      failure_class = -31;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (vault_hwm_read(configured_key, &hwm_version, attestation, sizeof(attestation),
                      &attestation_len) != 0)
   {
      failure_class = -32;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (hwm_version != 2)
   {
      failure_class = hwm_version == 0 ? -3310 : (hwm_version == 1 ? -3311 : -3312);
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (attestation_len != baseline.hwm_attestation_len)
   {
      failure_class = -330;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (vault_hwm_verify(configured_key, hwm_version, attestation, attestation_len) != 0)
   {
      failure_class = -332;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (CRYPTO_memcmp(attestation, baseline.hwm_attestation, attestation_len) != 0)
   {
      failure_class = -333;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   if (vault_primary_epoch_initialize((uint64_t)baseline.seal_epoch) != VAULT_MAINTENANCE_OK)
   {
      failure_class = -34;
      OPENSSL_cleanse(derived, sizeof(derived));
      OPENSSL_cleanse(attestation, sizeof(attestation));
      goto fail;
   }
   /* Release the mutually consistent startup snapshots only after the KMS,
    * public/wire binding, signed HWM, and durable seal epoch all agree. */
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
      if (db2_management_status_runtime_startup_end(&service->workers[i].lookup, 1) != 0)
      {
         failure_class = -4;
         goto fail;
      }
   snprintf(service->custody_key_id, sizeof(service->custody_key_id), "%s",
            baseline.custody_key_id);
   snprintf(service->wire_key_id, sizeof(service->wire_key_id), "%s", baseline.wire_key_id);
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
   {
      service->workers[i].custody.custody_key_id = service->custody_key_id;
      service->workers[i].custody.database = &service->workers[i].key;
   }
   OPENSSL_cleanse(error, sizeof(error));
   OPENSSL_cleanse(&baseline, sizeof(baseline));
   OPENSSL_cleanse(derived, sizeof(derived));
   OPENSSL_cleanse(attestation, sizeof(attestation));
   return 0;
fail:
   OPENSSL_cleanse(error, sizeof(error));
   OPENSSL_cleanse(&baseline, sizeof(baseline));
   service_close(service);
   return failure_class;
}

static char *copy_env(const char *name, size_t max)
{
   const char *value = getenv(name);
   if (!fixed_text(value, max))
      return NULL;
   return strdup(value);
}

int main(int argc, char **argv)
{
   (void)argv;
   if (argc != 1)
   {
      fixed_error("usage");
      return 64;
   }
   if (process_harden() != 0)
   {
      fixed_error("hardening");
      return 65;
   }
   char *dsn = copy_env("AIMEE_KB_STATUS_DSN", 4096);
   char *ca = copy_env("AIMEE_KB_STATUS_TLS_CA", PATH_MAX - 1);
   char *cert = copy_env("AIMEE_KB_STATUS_TLS_CERT", PATH_MAX - 1);
   char *key = copy_env("AIMEE_KB_STATUS_TLS_KEY", PATH_MAX - 1);
   char *helper = copy_env("AIMEE_VAULT_KMS_HELPER", PATH_MAX - 1);
   char *kms_id = copy_env("AIMEE_VAULT_KMS_KEY_ID", 600);
   char *hwm_public = copy_env("AIMEE_VAULT_KMS_HWM_PUBKEY", PATH_MAX - 1);
   char *hwm_domain = copy_env("AIMEE_VAULT_KMS_HWM_DOMAIN", 256);
   const char *host_env = getenv("AIMEE_KB_STATUS_BIND");
   const char *port_env = getenv("AIMEE_KB_STATUS_PORT");
   char host[256] = "0.0.0.0";
   char *end = NULL;
   errno = 0;
   unsigned long port_value = port_env ? strtoul(port_env, &end, 10) : 8444;
   if (host_env && fixed_text(host_env, sizeof(host) - 1))
      snprintf(host, sizeof(host), "%s", host_env);
   if (!dsn || !ca || !cert || !key || !helper || !kms_id || !hwm_public || !hwm_domain ||
       !root_owned_file(ca, 0) || !root_owned_file(cert, 0) || !root_owned_file(key, 0) ||
       !root_owned_file(helper, 1) || !root_owned_file(hwm_public, 0) || errno || !end || *end ||
       port_value > UINT16_MAX)
   {
      fixed_error("configuration");
      return 66;
   }
   /* Minimize the process environment before the first provider helper fork.
    * TLS and database code consume the private copies above, not getenv(). */
   if (clearenv() != 0 || setenv("AIMEE_VAULT_KMS_HELPER", helper, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_KEY_ID", kms_id, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_PUBKEY", hwm_public, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_DOMAIN", hwm_domain, 1) != 0)
   {
      fixed_error("hardening");
      return 65;
   }
   SSL_CTX *tls = kb_mgmt_status_listener_tls_ctx(ca, cert, key);
   if (!tls)
   {
      fixed_error("tls");
      return 67;
   }
   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0) != 0 || vault_custody_kms_hwm_refresh() != 0 || vault_is_sealed() ||
       !vault_custody_kms_hwm_ready())
   {
      SSL_CTX_free(tls);
      fixed_error("custody");
      return 68;
   }
   status_service_t service;
   int service_rc = service_open(&service, dsn, kms_id);
   if (service_rc != 0)
   {
      (void)vault_seal();
      SSL_CTX_free(tls);
      fixed_error(service_rc == -1      ? "database"
                  : service_rc == -2    ? "startup-state"
                  : service_rc == -31   ? "startup-binding"
                  : service_rc == -32   ? "startup-provider"
                  : service_rc == -330  ? "startup-attestation-length"
                  : service_rc == -3310 ? "startup-attestation-version-zero"
                  : service_rc == -3311 ? "startup-attestation-version-old"
                  : service_rc == -3312 ? "startup-attestation-version-new"
                  : service_rc == -332  ? "startup-attestation-signature"
                  : service_rc == -333  ? "startup-attestation-binding"
                  : service_rc == -34   ? "startup-epoch"
                                        : "startup-transaction");
      return 69;
   }
   g_status_kms_ready = 1;

   OPENSSL_cleanse(dsn, strlen(dsn));
   free(dsn);
   free(ca);
   free(cert);
   free(key);
   free(helper);
   free(kms_id);
   free(hwm_public);
   free(hwm_domain);

   sigset_t signals;
   sigemptyset(&signals);
   sigaddset(&signals, SIGINT);
   sigaddset(&signals, SIGTERM);
   if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0)
   {
      fixed_error("signal");
      return 70;
   }
   kb_mgmt_status_listener_config_t config = {.bind_address = host,
                                              .port = (uint16_t)port_value,
                                              .tls = tls,
                                              .handle = handle_request,
                                              .opaque = &service};
   if (kb_mgmt_status_listener_start(&config) != 0)
   {
      g_status_kms_ready = 0;
      service_close(&service);
      (void)vault_seal();
      SSL_CTX_free(tls);
      fixed_error("listen");
      return 71;
   }
   int received = 0;
   (void)sigwait(&signals, &received);
   kb_mgmt_status_listener_stop();
   g_status_kms_ready = 0;
   service_close(&service);
   int seal_rc = vault_seal();
   SSL_CTX_free(tls);
   return seal_rc == 0 ? 0 : 72;
}
