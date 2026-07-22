#include "kb/kb_management_runtime.h"
#include "kb/http/kb_http_servers.h"
#include "kb_mgmt_endpoint.h"
#include "kb/kb_mgmt_status_client.h"
#include "kb/kb_workload_helper_posix.h"
#include "kb_workload_provider.h"

#include <assert.h>
#include <fcntl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static kb_workload_result_t provider_result = KB_WORKLOAD_UNAVAILABLE;
static kb_management_cert_result_t lifecycle_result = KB_MANAGEMENT_CERT_OK;
static kb_management_cert_result_t reconcile_result = KB_MANAGEMENT_CERT_UNAVAILABLE;
static int register_calls;
static int unregister_calls;
static int provider_close_calls;
static pthread_mutex_t reconcile_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reconcile_cond = PTHREAD_COND_INITIALIZER;
static int block_reconcile;
static int reconcile_entered;

int kb_http_servers_health_register(kb_http_servers_health_handler_fn handler, void *ctx)
{
   (void)handler;
   (void)ctx;
   register_calls++;
   return 0;
}

int kb_http_servers_health_unregister(kb_http_servers_health_handler_fn handler, void *ctx)
{
   (void)handler;
   (void)ctx;
   unregister_calls++;
   return 0;
}

kb_workload_helper_result_t kb_workload_checked_root_file_open(const char *path,
                                                               int require_exec_elf, int *fd)
{
   (void)require_exec_elf;
   *fd = open(path, O_RDONLY | O_CLOEXEC);
   return *fd < 0 ? KB_WORKLOAD_HELPER_UNAVAILABLE : KB_WORKLOAD_HELPER_OK;
}

kb_workload_result_t kb_workload_provider_open(const kb_workload_provider_config_t *config,
                                               kb_workload_provider_t **out)
{
   (void)config;
   *out = provider_result == KB_WORKLOAD_OK ? (kb_workload_provider_t *)(uintptr_t)1 : NULL;
   return provider_result;
}

void kb_workload_provider_close(kb_workload_provider_t *provider)
{
   if (provider)
      provider_close_calls++;
}

kb_management_cert_result_t
kb_management_cert_lifecycle_open(const kb_management_cert_config_t *config,
                                  kb_management_cert_lifecycle_t **out)
{
   (void)config;
   *out = lifecycle_result == KB_MANAGEMENT_CERT_OK ? (kb_management_cert_lifecycle_t *)(uintptr_t)2
                                                    : NULL;
   return lifecycle_result;
}

kb_management_cert_result_t kb_management_cert_reconcile(kb_management_cert_lifecycle_t *lifecycle,
                                                         int64_t deadline,
                                                         kb_management_cert_active_t *active)
{
   (void)lifecycle;
   (void)deadline;
   (void)active;
   pthread_mutex_lock(&reconcile_mutex);
   if (block_reconcile)
   {
      reconcile_entered = 1;
      pthread_cond_broadcast(&reconcile_cond);
      while (block_reconcile)
         pthread_cond_wait(&reconcile_cond, &reconcile_mutex);
   }
   pthread_mutex_unlock(&reconcile_mutex);
   return reconcile_result;
}

void kb_management_cert_lifecycle_close(kb_management_cert_lifecycle_t *lifecycle)
{
   (void)lifecycle;
}

int kb_mgmt_endpoint_validate(const char *endpoint)
{
   return endpoint && strcmp(endpoint, "https://authority.example:443") == 0 ? 0 : -1;
}

kb_management_health_result_t
kb_management_health_exchange(const kb_management_health_request_t *request,
                              const kb_management_health_dependencies_t *deps)
{
   (void)request;
   (void)deps;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

kb_management_health_result_t
kb_management_health_snapshot_primary(void *ctx, const kb_principal_t *actor, int64_t team,
                                      const char *server, db2_server_snapshot_t *snapshot)
{
   (void)ctx;
   (void)actor;
   (void)team;
   (void)server;
   (void)snapshot;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

kb_management_health_result_t
kb_management_health_bundle_active(void *ctx, kb_management_cert_bundle_t *bundle,
                                   kb_management_cert_active_t *active)
{
   (void)ctx;
   (void)bundle;
   (void)active;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

void kb_management_health_bundle_cleanse(void *ctx, kb_management_cert_bundle_t *bundle)
{
   (void)ctx;
   (void)bundle;
}

kb_management_health_result_t
kb_management_health_server_open_production(void *ctx, const db2_server_snapshot_t *snapshot,
                                            const kb_management_cert_bundle_t *bundle,
                                            uint64_t deadline, void **out)
{
   (void)ctx;
   (void)snapshot;
   (void)bundle;
   (void)deadline;
   (void)out;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

kb_management_health_result_t kb_management_health_server_request_production(
    void *ctx, void *session, const char *method, const char *path, const char *body,
    const char *headers, uint64_t deadline, char *response, size_t cap, int *status)
{
   (void)ctx;
   (void)session;
   (void)method;
   (void)path;
   (void)body;
   (void)headers;
   (void)deadline;
   (void)response;
   (void)cap;
   (void)status;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

void kb_management_health_server_close_production(void *ctx, void *session)
{
   (void)ctx;
   (void)session;
}

kb_management_health_result_t
kb_mgmt_status_client_adapter(void *ctx, const kb_management_cert_bundle_t *bundle,
                              const char *request, size_t request_len, uint64_t deadline,
                              char *response, size_t cap, int *status)
{
   (void)ctx;
   (void)bundle;
   (void)request;
   (void)request_len;
   (void)deadline;
   (void)response;
   (void)cap;
   (void)status;
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

static void clear_packet(void)
{
   static const char *const names[] = {
       "AIMEE_KB_MGMT_INSTALLATION_ID",
       "AIMEE_KB_MGMT_CUSTODIED_CA_DIR",
       "AIMEE_KB_MGMT_BUNDLE_DIR",
       "AIMEE_KB_MGMT_WORKLOAD_HELPER",
       "AIMEE_KB_MGMT_WORKLOAD_JWKS",
       "AIMEE_KB_MGMT_WORKLOAD_PROOF_SPKI",
       "AIMEE_KB_MGMT_WORKLOAD_ISSUER",
       "AIMEE_KB_MGMT_WORKLOAD_AUDIENCE",
       "AIMEE_KB_MGMT_SERVER_CA_FILE",
       "AIMEE_KB_MGMT_STATUS_ENDPOINT",
       "AIMEE_KB_MGMT_STATUS_CA_FILE",
       "AIMEE_KB_MGMT_STATUS_LEAF_PIN",
       "AIMEE_KB_MGMT_STATUS_SECONDARY_LEAF_PIN",
       "AIMEE_MGMT_STATUS_KEY_ID",
       "AIMEE_MGMT_STATUS_PUBLIC_KEY",
   };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      unsetenv(names[i]);
}

static void write_ca(char path[64], int garbage)
{
   assert(snprintf(path, 64, "/tmp/aimee-p5b3b-ca-XXXXXX") > 0);
   int fd = mkstemp(path);
   assert(fd >= 0);
   EVP_PKEY_CTX *key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   EVP_PKEY *key = NULL;
   X509 *cert = X509_new();
   assert(key_ctx && cert && EVP_PKEY_keygen_init(key_ctx) == 1 &&
          EVP_PKEY_keygen(key_ctx, &key) == 1);
   assert(X509_set_version(cert, 2) == 1);
   assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
   assert(X509_gmtime_adj(X509_getm_notBefore(cert), -60));
   assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600));
   assert(X509_set_pubkey(cert, key) == 1);
   X509_NAME *name = X509_get_subject_name(cert);
   assert(name &&
          X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"runtime-test", -1, -1, 0) == 1);
   assert(X509_set_issuer_name(cert, name) == 1 && X509_sign(cert, key, NULL) > 0);
   BIO *bio = BIO_new_fd(fd, BIO_NOCLOSE);
   assert(bio && PEM_write_bio_X509(bio, cert) == 1 && BIO_flush(bio) == 1);
   BIO_free(bio);
   if (garbage)
      assert(write(fd, "garbage\n", 8) == 8);
   close(fd);
   X509_free(cert);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(key_ctx);
}

static void set_packet(const char *ca_path)
{
   setenv("AIMEE_KB_MGMT_INSTALLATION_ID", "00000000000000000000000000000000", 1);
   setenv("AIMEE_KB_MGMT_CUSTODIED_CA_DIR", "/tmp/custodied", 1);
   setenv("AIMEE_KB_MGMT_BUNDLE_DIR", "/tmp/bundles", 1);
   setenv("AIMEE_KB_MGMT_WORKLOAD_HELPER", "/tmp/helper", 1);
   setenv("AIMEE_KB_MGMT_WORKLOAD_JWKS", "/tmp/jwks", 1);
   setenv("AIMEE_KB_MGMT_WORKLOAD_PROOF_SPKI", "/tmp/proof", 1);
   setenv("AIMEE_KB_MGMT_WORKLOAD_ISSUER", "issuer", 1);
   setenv("AIMEE_KB_MGMT_WORKLOAD_AUDIENCE", "audience", 1);
   setenv("AIMEE_KB_MGMT_SERVER_CA_FILE", ca_path, 1);
   setenv("AIMEE_KB_MGMT_STATUS_ENDPOINT", "https://authority.example:443", 1);
   setenv("AIMEE_KB_MGMT_STATUS_CA_FILE", ca_path, 1);
   setenv("AIMEE_KB_MGMT_STATUS_LEAF_PIN",
          "1111111111111111111111111111111111111111111111111111111111111111", 1);
   setenv("AIMEE_MGMT_STATUS_KEY_ID", "status-key", 1);
   setenv("AIMEE_MGMT_STATUS_PUBLIC_KEY",
          "2222222222222222222222222222222222222222222222222222222222222222", 1);
}

static void *tick_thread(void *unused)
{
   (void)unused;
   kb_management_runtime_tick(INT64_MAX - 30);
   return NULL;
}

static void *stop_thread(void *unused)
{
   (void)unused;
   kb_management_runtime_stop();
   return NULL;
}

int main(void)
{
   char ca_path[64], garbage_path[64];
   write_ca(ca_path, 0);
   write_ca(garbage_path, 1);
   clear_packet();
   assert(kb_management_runtime_start() == 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   kb_management_runtime_stop();

   setenv("AIMEE_KB_MGMT_INSTALLATION_ID", "00000000000000000000000000000000", 1);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   clear_packet();

   setenv("AIMEE_KB_MGMT_INSTALLATION_ID", "", 1);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   clear_packet();

   set_packet(garbage_path);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   clear_packet();

   set_packet(ca_path);
   provider_result = KB_WORKLOAD_UNAVAILABLE;
   int registrations = register_calls;
   assert(kb_management_runtime_start() == 0);
   assert(register_calls == registrations + 1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_RETRY_WAIT);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_RETRY_WAIT);
   kb_management_runtime_stop();
   assert(unregister_calls >= 1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);

   provider_result = KB_WORKLOAD_OK;
   lifecycle_result = KB_MANAGEMENT_CERT_UNAVAILABLE;
   int closes = provider_close_calls;
   assert(kb_management_runtime_start() == 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_RETRY_WAIT);
   assert(provider_close_calls == closes + 1);
   kb_management_runtime_stop();

   provider_result = KB_WORKLOAD_INVALID;
   lifecycle_result = KB_MANAGEMENT_CERT_OK;
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);

   provider_result = KB_WORKLOAD_OK;
   lifecycle_result = KB_MANAGEMENT_CERT_INTEGRITY;
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);

   provider_result = KB_WORKLOAD_OK;
   lifecycle_result = KB_MANAGEMENT_CERT_OK;
   reconcile_result = KB_MANAGEMENT_CERT_INVALID;
   assert(kb_management_runtime_start() == 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_TERMINAL);
   kb_management_runtime_stop();

   reconcile_result = KB_MANAGEMENT_CERT_OK;
   assert(kb_management_runtime_start() == 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_READY);
   reconcile_result = KB_MANAGEMENT_CERT_UNAVAILABLE;
   kb_management_runtime_tick(INT64_MAX - 30);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_READY_DEGRADED);
   kb_management_runtime_stop();
   assert(kb_management_runtime_start() == 0);
   kb_management_runtime_stop();

   reconcile_result = KB_MANAGEMENT_CERT_OK;
   assert(kb_management_runtime_start() == 0);
   pthread_mutex_lock(&reconcile_mutex);
   block_reconcile = 1;
   reconcile_entered = 0;
   pthread_mutex_unlock(&reconcile_mutex);
   pthread_t ticker, stopper;
   assert(pthread_create(&ticker, NULL, tick_thread, NULL) == 0);
   pthread_mutex_lock(&reconcile_mutex);
   while (!reconcile_entered)
      pthread_cond_wait(&reconcile_cond, &reconcile_mutex);
   pthread_mutex_unlock(&reconcile_mutex);
   assert(pthread_create(&stopper, NULL, stop_thread, NULL) == 0);
   pthread_mutex_lock(&reconcile_mutex);
   block_reconcile = 0;
   pthread_cond_broadcast(&reconcile_cond);
   pthread_mutex_unlock(&reconcile_mutex);
   assert(pthread_join(ticker, NULL) == 0);
   assert(pthread_join(stopper, NULL) == 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   clear_packet();

   static const unsigned expected[] = {5, 10, 20, 40, 80, 160, 300, 300, 300};
   for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
      assert(kb_management_runtime_retry_seconds(i) == expected[i]);

   unlink(ca_path);
   unlink(garbage_path);
   puts("test_kb_management_runtime: ok");
   return 0;
}
