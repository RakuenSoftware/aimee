#define _POSIX_C_SOURCE 200809L
#include "kb_management_runtime.h"

#include "kb/http/kb_http_servers.h"
#include "kb_mgmt_endpoint.h"
#include "kb_mgmt_status_client.h"
#include "kb_tls.h"
#include "kb_workload_helper_posix.h"
#include "kb_workload_provider.h"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CA_PEM_MAX 65536U

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t idle;
   kb_management_runtime_state_t state;
   int configured;
   int registered;
   int reconciling;
   int construction_complete;
   int had_success;
   unsigned retry_index;
   int64_t next_reconcile;
   kb_workload_provider_t *provider;
   kb_management_cert_lifecycle_t *lifecycle;
   char installation_id[KB_MANAGEMENT_CERT_INSTALLATION_ID_LEN + 1];
   char custodied_ca_dir[PATH_MAX];
   char bundle_dir[PATH_MAX];
   char helper_path[PATH_MAX];
   char jwks_path[PATH_MAX];
   char proof_spki_path[PATH_MAX];
   char issuer[601];
   char audience[601];
   char server_ca[CA_PEM_MAX + 1];
   char status_endpoint[512];
   char status_ca[CA_PEM_MAX + 1];
   char status_leaf_pin[65];
   char status_secondary_leaf_pin[65];
   char status_key_id[65];
   unsigned char status_public_key[32];
} runtime_t;

static runtime_t runtime = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
    .state = KB_MANAGEMENT_RUNTIME_DISABLED,
};

static const char *const required_env[] = {
    "AIMEE_KB_MGMT_INSTALLATION_ID", "AIMEE_KB_MGMT_CUSTODIED_CA_DIR",
    "AIMEE_KB_MGMT_BUNDLE_DIR",      "AIMEE_KB_MGMT_WORKLOAD_HELPER",
    "AIMEE_KB_MGMT_WORKLOAD_JWKS",   "AIMEE_KB_MGMT_WORKLOAD_PROOF_SPKI",
    "AIMEE_KB_MGMT_WORKLOAD_ISSUER", "AIMEE_KB_MGMT_WORKLOAD_AUDIENCE",
    "AIMEE_KB_MGMT_SERVER_CA_FILE",  "AIMEE_KB_MGMT_STATUS_ENDPOINT",
    "AIMEE_KB_MGMT_STATUS_CA_FILE",  "AIMEE_KB_MGMT_STATUS_LEAF_PIN",
    "AIMEE_MGMT_STATUS_KEY_ID",      "AIMEE_MGMT_STATUS_PUBLIC_KEY",
};

static int lower_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int token(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

static int printable(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if ((unsigned char)s[i] < 0x21 || (unsigned char)s[i] > 0x7e)
         return 0;
   return 1;
}

static int copy_env(char *out, size_t cap, const char *name)
{
   const char *value = getenv(name);
   size_t n = value ? strnlen(value, cap) : 0;
   if (!n || n >= cap)
      return -1;
   memcpy(out, value, n + 1);
   return 0;
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
      if (!n || n > NAME_MAX || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
         return 0;
      if (!slash)
         return 1;
      p = slash + 1;
   }
   return 0;
}

static int read_root_file(const char *path, char *out, size_t cap)
{
   int fd = -1;
   if (!out || cap < 2 || kb_workload_checked_root_file_open(path, 0, &fd) != KB_WORKLOAD_HELPER_OK)
      return -1;
   struct stat st;
   int rc = fstat(fd, &st);
   if (rc || st.st_size < 1 || (uintmax_t)st.st_size >= cap)
   {
      close(fd);
      return -1;
   }
   size_t used = 0, need = (size_t)st.st_size;
   while (used < need)
   {
      ssize_t n = read(fd, out + used, need - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
      {
         close(fd);
         OPENSSL_cleanse(out, cap);
         return -1;
      }
      used += (size_t)n;
   }
   char extra;
   do
      rc = (int)read(fd, &extra, 1);
   while (rc < 0 && errno == EINTR);
   close(fd);
   if (rc != 0)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   out[used] = 0;
   return 0;
}

static int valid_ca_pem(const char *pem)
{
   int count = 0;
   const char *p = pem;
   static const char begin[] = "-----BEGIN CERTIFICATE-----";
   static const char end[] = "-----END CERTIFICATE-----";
   while (*p)
   {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
         p++;
      if (!*p)
         return count > 0;
      if (strncmp(p, begin, sizeof(begin) - 1))
         return 0;
      const char *finish = strstr(p + sizeof(begin) - 1, end);
      if (!finish)
         return 0;
      finish += sizeof(end) - 1;
      size_t length = (size_t)(finish - p);
      if (length > INT_MAX)
         return 0;
      BIO *bio = BIO_new_mem_buf(p, (int)length);
      X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
      BIO_free(bio);
      ERR_clear_error();
      if (!cert)
         return 0;
      count++;
      X509_free(cert);
      p = finish;
   }
   return count > 0;
}

static int hex_key(const char *hex, unsigned char out[32])
{
   if (!lower_hex(hex, 64))
      return -1;
   for (size_t i = 0; i < 32; i++)
   {
      unsigned a = (unsigned char)hex[i * 2], b = (unsigned char)hex[i * 2 + 1];
      a = a <= '9' ? a - '0' : a - 'a' + 10;
      b = b <= '9' ? b - '0' : b - 'a' + 10;
      out[i] = (unsigned char)((a << 4) | b);
   }
   EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, out, 32);
   if (!key)
   {
      OPENSSL_cleanse(out, 32);
      return -1;
   }
   EVP_PKEY_free(key);
   return 0;
}

static uint64_t wall_seconds(void *unused)
{
   (void)unused;
   time_t now = time(NULL);
   return now < 0 ? 0 : (uint64_t)now;
}

static uint64_t monotonic_millis(void *unused)
{
   (void)unused;
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      return UINT64_MAX;
   return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static kb_management_health_result_t runtime_health(void *unused, const kb_principal_t *actor,
                                                    int64_t team_id, const char *server_id)
{
   (void)unused;
   kb_management_cert_lifecycle_t *lifecycle = NULL;
   kb_management_health_server_config_t server = {0};
   kb_mgmt_status_client_config_t authority = {0};
   char status_key_id[sizeof(runtime.status_key_id)];
   unsigned char status_public_key[32];

   pthread_mutex_lock(&runtime.mutex);
   if ((runtime.state != KB_MANAGEMENT_RUNTIME_READY &&
        runtime.state != KB_MANAGEMENT_RUNTIME_READY_DEGRADED) ||
       !runtime.lifecycle)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   }
   lifecycle = runtime.lifecycle;
   server.server_ca_pem = runtime.server_ca;
   authority.endpoint = runtime.status_endpoint;
   authority.ca_pem = runtime.status_ca;
   authority.leaf_pin = runtime.status_leaf_pin;
   authority.secondary_leaf_pin =
       runtime.status_secondary_leaf_pin[0] ? runtime.status_secondary_leaf_pin : NULL;
   memcpy(status_key_id, runtime.status_key_id, sizeof(status_key_id));
   memcpy(status_public_key, runtime.status_public_key, sizeof(status_public_key));
   /* The route registry owns the outer borrow. Unregister waits for this
    * callback, keeping these runtime-owned pointers and lifecycle live while
    * the singleton mutex is deliberately released across network I/O. */
   pthread_mutex_unlock(&runtime.mutex);

   uint64_t now = monotonic_millis(NULL);
   if (now == UINT64_MAX || now > UINT64_MAX - 15000U)
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   kb_management_health_request_t request = {
       .actor = actor,
       .team_id = team_id,
       .server_id = server_id,
       .deadline_millis = now + 15000U,
   };
   kb_management_health_dependencies_t deps = {
       .snapshot = kb_management_health_snapshot_primary,
       .bundle_load = kb_management_health_bundle_active,
       .bundle_ctx = lifecycle,
       .bundle_clear = kb_management_health_bundle_cleanse,
       .server_open = kb_management_health_server_open_production,
       .server_ctx = &server,
       .server_request = kb_management_health_server_request_production,
       .server_close = kb_management_health_server_close_production,
       .authority_issue = kb_mgmt_status_client_adapter,
       .authority_ctx = &authority,
       .wall_seconds = wall_seconds,
       .monotonic_millis = monotonic_millis,
       .status_key_id = status_key_id,
       .status_public_key = status_public_key,
   };
   kb_management_health_result_t result = kb_management_health_exchange(&request, &deps);
   OPENSSL_cleanse(status_public_key, sizeof(status_public_key));
   return result;
}

unsigned kb_management_runtime_retry_seconds(unsigned retry_index)
{
   static const unsigned delays[] = {5, 10, 20, 40, 80, 160, 300};
   return delays[retry_index < sizeof(delays) / sizeof(delays[0])
                     ? retry_index
                     : sizeof(delays) / sizeof(delays[0]) - 1];
}

static void schedule_failure(int64_t now, kb_management_cert_result_t result)
{
   pthread_mutex_lock(&runtime.mutex);
   runtime.reconciling = 0;
   pthread_cond_broadcast(&runtime.idle);
   if (runtime.state == KB_MANAGEMENT_RUNTIME_STOPPING)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return;
   }
   if (result == KB_MANAGEMENT_CERT_INTEGRITY || result == KB_MANAGEMENT_CERT_DENIED ||
       result == KB_MANAGEMENT_CERT_INVALID || result == KB_MANAGEMENT_CERT_DISABLED)
   {
      runtime.state = KB_MANAGEMENT_RUNTIME_TERMINAL;
      runtime.next_reconcile = INT64_MAX;
   }
   else
   {
      runtime.state = runtime.had_success ? KB_MANAGEMENT_RUNTIME_READY_DEGRADED
                                          : KB_MANAGEMENT_RUNTIME_RETRY_WAIT;
      unsigned delay = kb_management_runtime_retry_seconds(runtime.retry_index);
      if (runtime.retry_index < 6)
         runtime.retry_index++;
      runtime.next_reconcile = now > INT64_MAX - (int64_t)delay ? INT64_MAX : now + delay;
   }
   pthread_mutex_unlock(&runtime.mutex);
}

void kb_management_runtime_tick(int64_t now)
{
   kb_management_cert_lifecycle_t *lifecycle;
   pthread_mutex_lock(&runtime.mutex);
   if (!runtime.configured || runtime.state == KB_MANAGEMENT_RUNTIME_STOPPING ||
       runtime.state == KB_MANAGEMENT_RUNTIME_TERMINAL || runtime.reconciling || now < 0 ||
       now < runtime.next_reconcile)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return;
   }
   runtime.reconciling = 1;
   if (!runtime.had_success)
      runtime.state = KB_MANAGEMENT_RUNTIME_RECONCILING;
   lifecycle = runtime.lifecycle;
   pthread_mutex_unlock(&runtime.mutex);

   if (!lifecycle)
   {
      kb_workload_provider_config_t pc = {
          .kind = KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
          .helper_path = runtime.helper_path,
          .jwks_path = runtime.jwks_path,
          .proof_spki_path = runtime.proof_spki_path,
          .expected_issuer = runtime.issuer,
          .expected_audience = runtime.audience,
          .max_token_age_seconds = 300,
          .helper_timeout_ms = 5000,
      };
      kb_workload_provider_t *provider = NULL;
      kb_workload_result_t pr = kb_workload_provider_open(&pc, &provider);
      if (pr != KB_WORKLOAD_OK)
      {
         kb_management_cert_result_t mapped = pr == KB_WORKLOAD_INVALID ? KB_MANAGEMENT_CERT_INVALID
                                              : pr == KB_WORKLOAD_INTEGRITY
                                                  ? KB_MANAGEMENT_CERT_INTEGRITY
                                                  : KB_MANAGEMENT_CERT_UNAVAILABLE;
         schedule_failure(now, mapped);
         return;
      }
      kb_management_cert_config_t lc = {
          .provider = provider,
          .custodied_ca_dir = runtime.custodied_ca_dir,
          .bundle_dir = runtime.bundle_dir,
      };
      memcpy(lc.installation_id, runtime.installation_id, sizeof(lc.installation_id));
      kb_management_cert_result_t lr = kb_management_cert_lifecycle_open(&lc, &lifecycle);
      if (lr != KB_MANAGEMENT_CERT_OK)
      {
         kb_workload_provider_close(provider);
         schedule_failure(now, lr);
         return;
      }
      pthread_mutex_lock(&runtime.mutex);
      runtime.provider = provider;
      runtime.lifecycle = lifecycle;
      runtime.construction_complete = 1;
      pthread_mutex_unlock(&runtime.mutex);
   }

   kb_management_cert_active_t active;
   int64_t deadline = now > INT64_MAX - 30 ? INT64_MAX : now + 30;
   kb_management_cert_result_t result = kb_management_cert_reconcile(lifecycle, deadline, &active);
   OPENSSL_cleanse(&active, sizeof(active));
   if (result != KB_MANAGEMENT_CERT_OK)
   {
      schedule_failure(now, result);
      return;
   }
   pthread_mutex_lock(&runtime.mutex);
   runtime.reconciling = 0;
   pthread_cond_broadcast(&runtime.idle);
   if (runtime.state == KB_MANAGEMENT_RUNTIME_STOPPING)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return;
   }
   runtime.had_success = 1;
   runtime.retry_index = 0;
   runtime.state = KB_MANAGEMENT_RUNTIME_READY;
   runtime.next_reconcile = now > INT64_MAX - 60 ? INT64_MAX : now + 60;
   pthread_mutex_unlock(&runtime.mutex);
}

static void clear_configuration(void)
{
   OPENSSL_cleanse(runtime.server_ca, sizeof(runtime.server_ca));
   OPENSSL_cleanse(runtime.status_ca, sizeof(runtime.status_ca));
   OPENSSL_cleanse(runtime.status_public_key, sizeof(runtime.status_public_key));
   memset(runtime.installation_id, 0, sizeof(runtime.installation_id));
   memset(runtime.custodied_ca_dir, 0, sizeof(runtime.custodied_ca_dir));
   memset(runtime.bundle_dir, 0, sizeof(runtime.bundle_dir));
   memset(runtime.helper_path, 0, sizeof(runtime.helper_path));
   memset(runtime.jwks_path, 0, sizeof(runtime.jwks_path));
   memset(runtime.proof_spki_path, 0, sizeof(runtime.proof_spki_path));
   memset(runtime.issuer, 0, sizeof(runtime.issuer));
   memset(runtime.audience, 0, sizeof(runtime.audience));
   memset(runtime.status_endpoint, 0, sizeof(runtime.status_endpoint));
   memset(runtime.status_leaf_pin, 0, sizeof(runtime.status_leaf_pin));
   memset(runtime.status_secondary_leaf_pin, 0, sizeof(runtime.status_secondary_leaf_pin));
   memset(runtime.status_key_id, 0, sizeof(runtime.status_key_id));
}

int kb_management_runtime_start(void)
{
   pthread_mutex_lock(&runtime.mutex);
   int inactive = runtime.state == KB_MANAGEMENT_RUNTIME_DISABLED && !runtime.configured &&
                  !runtime.registered && !runtime.provider && !runtime.lifecycle;
   pthread_mutex_unlock(&runtime.mutex);
   if (!inactive)
      return -1;
   unsigned supplied = 0, present = 0;
   for (size_t i = 0; i < sizeof(required_env) / sizeof(required_env[0]); i++)
   {
      const char *v = getenv(required_env[i]);
      supplied += v != NULL;
      present += v && *v;
   }
   const char *secondary = getenv("AIMEE_KB_MGMT_STATUS_SECONDARY_LEAF_PIN");
   if (!supplied && !secondary)
      return 0;
   if (supplied != sizeof(required_env) / sizeof(required_env[0]) ||
       present != sizeof(required_env) / sizeof(required_env[0]) || (secondary && !*secondary))
      return -1;

   char server_ca_file[PATH_MAX], status_ca_file[PATH_MAX], public_hex[65];
   if (copy_env(runtime.installation_id, sizeof(runtime.installation_id), required_env[0]) ||
       copy_env(runtime.custodied_ca_dir, sizeof(runtime.custodied_ca_dir), required_env[1]) ||
       copy_env(runtime.bundle_dir, sizeof(runtime.bundle_dir), required_env[2]) ||
       copy_env(runtime.helper_path, sizeof(runtime.helper_path), required_env[3]) ||
       copy_env(runtime.jwks_path, sizeof(runtime.jwks_path), required_env[4]) ||
       copy_env(runtime.proof_spki_path, sizeof(runtime.proof_spki_path), required_env[5]) ||
       copy_env(runtime.issuer, sizeof(runtime.issuer), required_env[6]) ||
       copy_env(runtime.audience, sizeof(runtime.audience), required_env[7]) ||
       copy_env(server_ca_file, sizeof(server_ca_file), required_env[8]) ||
       copy_env(runtime.status_endpoint, sizeof(runtime.status_endpoint), required_env[9]) ||
       copy_env(status_ca_file, sizeof(status_ca_file), required_env[10]) ||
       copy_env(runtime.status_leaf_pin, sizeof(runtime.status_leaf_pin), required_env[11]) ||
       copy_env(runtime.status_key_id, sizeof(runtime.status_key_id), required_env[12]) ||
       copy_env(public_hex, sizeof(public_hex), required_env[13]))
      goto invalid;
   if (secondary && *secondary)
   {
      if (strnlen(secondary, sizeof(runtime.status_secondary_leaf_pin)) != 64)
         goto invalid;
      memcpy(runtime.status_secondary_leaf_pin, secondary, 65);
   }
   if (!lower_hex(runtime.installation_id, 32) || !absolute_path(runtime.custodied_ca_dir) ||
       !absolute_path(runtime.bundle_dir) || !absolute_path(runtime.helper_path) ||
       !absolute_path(runtime.jwks_path) || !absolute_path(runtime.proof_spki_path) ||
       !absolute_path(server_ca_file) || !absolute_path(status_ca_file) ||
       !printable(runtime.issuer, 600) || !printable(runtime.audience, 600) ||
       kb_mgmt_endpoint_validate(runtime.status_endpoint) ||
       !lower_hex(runtime.status_leaf_pin, 64) ||
       (runtime.status_secondary_leaf_pin[0] &&
        !lower_hex(runtime.status_secondary_leaf_pin, 64)) ||
       !token(runtime.status_key_id, 64) || hex_key(public_hex, runtime.status_public_key) ||
       read_root_file(server_ca_file, runtime.server_ca, sizeof(runtime.server_ca)) ||
       read_root_file(status_ca_file, runtime.status_ca, sizeof(runtime.status_ca)) ||
       !valid_ca_pem(runtime.server_ca) || !valid_ca_pem(runtime.status_ca))
      goto invalid;

   pthread_mutex_lock(&runtime.mutex);
   runtime.configured = 1;
   runtime.state = KB_MANAGEMENT_RUNTIME_RETRY_WAIT;
   runtime.next_reconcile = 0;
   pthread_mutex_unlock(&runtime.mutex);
   if (kb_http_servers_health_register(runtime_health, NULL))
      goto invalid_configured;
   pthread_mutex_lock(&runtime.mutex);
   runtime.registered = 1;
   pthread_mutex_unlock(&runtime.mutex);
   kb_management_runtime_tick((int64_t)wall_seconds(NULL));
   pthread_mutex_lock(&runtime.mutex);
   int static_failure =
       runtime.state == KB_MANAGEMENT_RUNTIME_TERMINAL && !runtime.construction_complete;
   pthread_mutex_unlock(&runtime.mutex);
   if (static_failure)
   {
      kb_management_runtime_stop();
      OPENSSL_cleanse(public_hex, sizeof(public_hex));
      return -1;
   }
   OPENSSL_cleanse(public_hex, sizeof(public_hex));
   return 0;

invalid_configured:
   pthread_mutex_lock(&runtime.mutex);
   runtime.configured = 0;
   runtime.state = KB_MANAGEMENT_RUNTIME_DISABLED;
   pthread_mutex_unlock(&runtime.mutex);
invalid:
   OPENSSL_cleanse(public_hex, sizeof(public_hex));
   clear_configuration();
   return -1;
}

void kb_management_runtime_stop(void)
{
   pthread_mutex_lock(&runtime.mutex);
   runtime.state = KB_MANAGEMENT_RUNTIME_STOPPING;
   int registered = runtime.registered;
   runtime.registered = 0;
   pthread_mutex_unlock(&runtime.mutex);
   if (registered)
      (void)kb_http_servers_health_unregister(runtime_health, NULL);

   pthread_mutex_lock(&runtime.mutex);
   while (runtime.reconciling)
      pthread_cond_wait(&runtime.idle, &runtime.mutex);
   kb_management_cert_lifecycle_t *lifecycle = runtime.lifecycle;
   kb_workload_provider_t *provider = runtime.provider;
   runtime.lifecycle = NULL;
   runtime.provider = NULL;
   runtime.configured = 0;
   runtime.reconciling = 0;
   runtime.construction_complete = 0;
   runtime.had_success = 0;
   runtime.retry_index = 0;
   runtime.next_reconcile = 0;
   pthread_mutex_unlock(&runtime.mutex);
   kb_management_cert_lifecycle_close(lifecycle);
   kb_workload_provider_close(provider);
   clear_configuration();
   pthread_mutex_lock(&runtime.mutex);
   runtime.state = KB_MANAGEMENT_RUNTIME_DISABLED;
   pthread_mutex_unlock(&runtime.mutex);
}

kb_management_runtime_state_t kb_management_runtime_state(void)
{
   pthread_mutex_lock(&runtime.mutex);
   kb_management_runtime_state_t state = runtime.state;
   pthread_mutex_unlock(&runtime.mutex);
   return state;
}
