#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_lifecycle.h"

#include "kb_management_cert_storage.h"

#include <openssl/crypto.h>

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct kb_management_cert_lifecycle
{
   kb_workload_provider_t *provider;
   kb_workload_provider_kind_t provider_kind;
   char installation_id[33];
   char custodied_ca_dir[PATH_MAX];
   kb_management_cert_storage_t storage;
   pthread_mutex_t mutex;
   int mutex_ready;
};

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
      if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') ||
          (n == 2 && p[0] == '.' && p[1] == '.'))
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

kb_management_cert_result_t kb_management_cert_lifecycle_open(
    const kb_management_cert_config_t *config, kb_management_cert_lifecycle_t **out)
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

/* The durable codecs/storage/crypto foundation intentionally fails closed until
 * the next packet wires B2a+B2b transition orchestration. */
kb_management_cert_result_t kb_management_cert_reconcile(kb_management_cert_lifecycle_t *lifecycle,
                                                         int64_t deadline_epoch,
                                                         kb_management_cert_active_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!lifecycle || !out || deadline_epoch < 1)
      return KB_MANAGEMENT_CERT_INVALID;
   if (pthread_mutex_lock(&lifecycle->mutex) != 0)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   pthread_mutex_unlock(&lifecycle->mutex);
   return KB_MANAGEMENT_CERT_UNAVAILABLE;
}

kb_management_cert_result_t kb_management_cert_load_active(kb_management_cert_lifecycle_t *lifecycle,
                                                           kb_management_cert_bundle_t *bundle,
                                                           kb_management_cert_active_t *out)
{
   if (bundle)
      kb_management_cert_bundle_clear(bundle);
   if (out)
      memset(out, 0, sizeof(*out));
   if (!lifecycle || !bundle || !out)
      return KB_MANAGEMENT_CERT_INVALID;
   if (pthread_mutex_lock(&lifecycle->mutex) != 0)
      return KB_MANAGEMENT_CERT_UNAVAILABLE;
   pthread_mutex_unlock(&lifecycle->mutex);
   return KB_MANAGEMENT_CERT_UNAVAILABLE;
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
