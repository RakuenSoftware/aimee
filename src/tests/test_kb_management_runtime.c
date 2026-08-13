#include "kb/kb_management_runtime.h"
#include "kb/http/kb_http_servers.h"
#include "kb_mgmt_endpoint.h"
#include "kb/kb_mgmt_status_client.h"
#include "kb/kb_workload_helper_posix.h"
#include "kb_workload_provider.h"
#include "kb_mgmt_status.h"
#include "db2/management_read_journal.h"
#include "management_read.h"

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
#include <time.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static kb_workload_result_t provider_result = KB_WORKLOAD_UNAVAILABLE;
static kb_management_cert_result_t lifecycle_result = KB_MANAGEMENT_CERT_OK;
static kb_management_cert_result_t reconcile_result = KB_MANAGEMENT_CERT_UNAVAILABLE;
static int register_calls;
static int unregister_calls;
static int read_register_calls;
static int read_unregister_calls;
static int provider_close_calls;
static pthread_mutex_t reconcile_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reconcile_cond = PTHREAD_COND_INITIALIZER;
static int block_reconcile;
static int reconcile_entered;
static int read_fixture_enabled;
static int read_sequence;
static server_mgmt_read_selector_t read_selector;
static int read_response_mode;
static kb_mgmt_token_authority_ipc_result_t token_issue_result = KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
static kb_http_servers_read_handler_fn captured_read_handler;
static unsigned char read_status_private_key[32];

db2_management_read_result_t db2_management_read_publication_generation(int64_t *out)
{
   if (!read_fixture_enabled)
      return DB2_MANAGEMENT_READ_UNAVAILABLE;
   assert(out);
   *out = 9;
   return DB2_MANAGEMENT_READ_OK;
}

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

int kb_http_servers_action_register(kb_http_servers_action_handler_fn handler, void *ctx)
{
   (void)handler;
   (void)ctx;
   return 0;
}

int kb_http_servers_action_unregister(kb_http_servers_action_handler_fn handler, void *ctx)
{
   (void)handler;
   (void)ctx;
   return 0;
}

int kb_http_servers_read_register(kb_http_servers_read_handler_fn handler, void *ctx)
{
   (void)ctx;
   captured_read_handler = handler;
   read_register_calls++;
   return 0;
}

int kb_http_servers_read_unregister(kb_http_servers_read_handler_fn handler, void *ctx)
{
   (void)handler;
   (void)ctx;
   read_unregister_calls++;
   captured_read_handler = NULL;
   return 0;
}

db2_management_read_result_t
db2_management_read_intent_start(const kb_principal_t *actor, int64_t team, const char *server,
                                 server_mgmt_read_selector_t selector, const char *path,
                                 const uint8_t nonce[32], const char *digest, const char *issuer,
                                 const char *installation, int ttl,
                                 db2_management_read_intent_t *out)
{
   if (read_fixture_enabled)
   {
      assert(actor && actor->authenticated && team == 7 && !strcmp(server, "srv-1"));
      assert((selector == SERVER_MGMT_READ_SELECTOR_AGENTS ||
              selector == SERVER_MGMT_READ_SELECTOR_CONFIG) &&
             ((selector == SERVER_MGMT_READ_SELECTOR_AGENTS &&
               !strcmp(path, "/v1/servers/srv-1/agents")) ||
              (selector == SERVER_MGMT_READ_SELECTOR_CONFIG &&
               !strcmp(path, "/v1/servers/srv-1/config"))) &&
             nonce[0] == 0);
      read_selector = selector;
      assert(!strcmp(digest, "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"));
      assert(!strcmp(issuer, "https://issuer.example") &&
             !strcmp(installation, "00000000000000000000000000000000") && ttl == 90);
      memset(out, 0, sizeof(*out));
      memset(out->correlation_id, 'c', 64);
      memset(out->jti, 'e', 64);
      out->team_id = team;
      snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", server);
      snprintf(out->request_sha256, sizeof(out->request_sha256), "%s", digest);
      snprintf(out->local_cert_issuer, sizeof(out->local_cert_issuer), "/CN=kb-ca");
      snprintf(out->local_cert_serial_norm, sizeof(out->local_cert_serial_norm), "01af");
      memset(out->local_cert_fingerprint, 'a', 64);
      snprintf(out->target_mgmt_issuer, sizeof(out->target_mgmt_issuer), "/CN=server-ca");
      snprintf(out->target_mgmt_serial_norm, sizeof(out->target_mgmt_serial_norm), "10be");
      memset(out->target_mgmt_fingerprint, 'b', 64);
      out->revocation_generation = 7;
      out->publication_generation = 9;
      return DB2_MANAGEMENT_READ_OK;
   }
   return DB2_MANAGEMENT_READ_UNAVAILABLE;
}

int server_mgmt_read_digest(const server_mgmt_read_digest_input_t *input, char out[65])
{
   if (read_fixture_enabled)
   {
      assert(input && !strcmp(input->server_id, "srv-1") && input->team_id == 7 &&
             input->publication_generation == 9);
      memset(out, 'd', 64);
      out[64] = 0;
      return 0;
   }
   if (out)
      out[0] = 0;
   return -1;
}

const char *server_mgmt_read_selector_name(server_mgmt_read_selector_t selector)
{
   return selector == SERVER_MGMT_READ_SELECTOR_AGENTS   ? "agents"
          : selector == SERVER_MGMT_READ_SELECTOR_CONFIG ? "config"
                                                         : NULL;
}

const char *server_mgmt_read_selector_purpose(server_mgmt_read_selector_t selector)
{
   return selector == SERVER_MGMT_READ_SELECTOR_AGENTS   ? "management.read.v1"
          : selector == SERVER_MGMT_READ_SELECTOR_CONFIG ? "management.read.config.v1"
                                                         : NULL;
}

int server_mgmt_read_selector_path(server_mgmt_read_selector_t selector, const char *server,
                                   char *out, size_t cap)
{
   const char *name = server_mgmt_read_selector_name(selector);
   int n = name && server && out ? snprintf(out, cap, "/v1/servers/%s/%s", server, name) : -1;
   return n > 0 && (size_t)n < cap ? n : -1;
}

int kb_management_read_challenge_decode(const char *raw, size_t len, const char *purpose,
                                        unsigned char nonce[32], uint64_t *expires)
{
   if (read_fixture_enabled)
   {
      assert(raw && len && purpose && nonce && expires);
      memset(nonce, 0, 32);
      *expires = (uint64_t)time(NULL) + 10;
      return 0;
   }
   return -1;
}

kb_mgmt_token_authority_ipc_result_t
kb_mgmt_token_authority_client_issue(const kb_mgmt_token_authority_client_config_t *config,
                                     const char *correlation, const char *jti,
                                     kb_mgmt_token_authority_output_t *out)
{
   (void)config;
   (void)correlation;
   (void)jti;
   if (read_fixture_enabled)
   {
      assert(read_sequence == 0 && correlation[0] == 'c' && jti[0] == 'e');
      if (token_issue_result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
         return token_issue_result;
      read_sequence = 1;
      snprintf(out->jwt, sizeof(out->jwt), "signed-read-token");
      return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   }
   return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
}

db2_management_action_result_t db2_management_action_operation_init(
    int64_t team, const char *server, db2_management_action_capability_t cap,
    const uint8_t digest[32], const char *issuer, const char *kid, int ttl,
    const char *installation, db2_management_action_operation_t *out)
{
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}
db2_management_action_result_t
db2_management_action_intent_start(const kb_principal_t *p,
                                   const db2_management_action_operation_t *op,
                                   db2_management_action_intent_t *out)
{
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}
db2_management_action_result_t
db2_management_action_outcome_append(const kb_principal_t *p,
                                     const db2_management_action_outcome_operation_t *op,
                                     db2_management_action_outcome_t *out)
{
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}
kb_management_action_transport_t kb_management_action_server_request_production(
    void *ctx, void *session, const char *method, const char *path, const char *body,
    const char *headers, uint64_t deadline, char *response, size_t cap, int *status)
{
   if (read_fixture_enabled && (!strcmp(path, "/v1/management/read/challenge") ||
                                !strcmp(path, "/v1/management/read/config/challenge")))
   {
      assert(!strcmp(method, "POST") && session == (void *)0x3456 && !headers);
      snprintf(response, cap, "challenge");
      *status = 200;
      return KB_MANAGEMENT_ACTION_SENT_RESPONSE;
   }
   if (read_fixture_enabled &&
       (!strcmp(path, "/v1/management/read/agents") || !strcmp(path, "/v1/management/read/config")))
   {
      assert(!strcmp(method, "GET") && session == (void *)0x3456 && headers &&
             strstr(headers, "Authorization: Bearer signed-read-token\r\n") &&
             strstr(headers, "X-Aimee-Management-Status: {") && read_sequence == 2);
      const char *projection =
          read_selector == SERVER_MGMT_READ_SELECTOR_CONFIG
              ? "{\"server_id\":\"srv-1\",\"team\":7,\"config\":{\"mtls\":\"required\","
                "\"remote_writes\":\"data\",\"client_transport\":\"auto\","
                "\"cli_session_forwarding\":true,\"require_aimee_git\":false}}"
          : read_response_mode == 1
              ? "{\"server_id\":\"srv-1\",\"server_id\":\"srv-1\",\"agents\":[],\"team\":7}"
          : read_response_mode == 2
              ? "{\"server_id\":\"srv-1\",\"team\":7,\"agents\":[{\"name\":\"agent-a\","
                "\"name\":\"agent-a\",\"model\":\"gpt-5\",\"enabled\":true,"
                "\"delegate_available\":false,\"primary_only\":false,\"max_parallel\":1}]}"
              : "{\"server_id\":\"srv-1\",\"team\":7,\"agents\":[{\"name\":\"agent-a\","
                "\"provider\":\"openai\",\"model\":\"gpt-5\",\"enabled\":true,"
                "\"delegate_available\":false,\"primary_only\":false,\"max_parallel\":1}]}";
      assert(snprintf(response, cap, "%s", projection) > 0);
      read_sequence = 3;
      *status = 200;
      return KB_MANAGEMENT_ACTION_SENT_RESPONSE;
   }
   return KB_MANAGEMENT_ACTION_NOT_SENT;
}
kb_mgmt_token_authority_ipc_result_t
kb_management_action_token_issue_production(void *ctx, const char *correlation, const char *jti,
                                            kb_mgmt_token_authority_output_t *out)
{
   return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
}
kb_management_action_result_t
kb_management_action_execute(const kb_management_action_request_t *request,
                             const kb_management_action_dependencies_t *deps)
{
   return KB_MANAGEMENT_ACTION_UNAVAILABLE;
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
   if (read_fixture_enabled)
   {
      memset(snapshot, 0, sizeof(*snapshot));
      snprintf(snapshot->server_id, sizeof(snapshot->server_id), "%s", server);
      snprintf(snapshot->endpoint, sizeof(snapshot->endpoint), "https://authority.example:443");
      snprintf(snapshot->status, sizeof(snapshot->status), "active");
      snprintf(snapshot->enrollment_state, sizeof(snapshot->enrollment_state), "active");
      snprintf(snapshot->management_issuer, sizeof(snapshot->management_issuer), "/CN=server-ca");
      snprintf(snapshot->management_serial_norm, sizeof(snapshot->management_serial_norm), "10be");
      memset(snapshot->management_fingerprint, 'b', 64);
      snapshot->management_fingerprint[64] = 0;
      snapshot->revocation_generation = 7;
      return KB_MANAGEMENT_HEALTH_OK;
   }
   return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

kb_management_health_result_t
kb_management_health_bundle_active(void *ctx, kb_management_cert_bundle_t *bundle,
                                   kb_management_cert_active_t *active)
{
   (void)ctx;
   (void)bundle;
   if (read_fixture_enabled)
   {
      memset(bundle, 0, sizeof(*bundle));
      memset(active, 0, sizeof(*active));
      snprintf(active->installation_id, sizeof(active->installation_id),
               "00000000000000000000000000000000");
      snprintf(active->issuer, sizeof(active->issuer), "/CN=kb-ca");
      snprintf(active->serial_norm, sizeof(active->serial_norm), "01af");
      memset(active->fingerprint, 0xaa, sizeof(active->fingerprint));
      return KB_MANAGEMENT_HEALTH_OK;
   }
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
   if (read_fixture_enabled)
   {
      *out = (void *)0x3456;
      return KB_MANAGEMENT_HEALTH_OK;
   }
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
   if (read_fixture_enabled)
   {
      assert(read_sequence == 1);
      kb_mgmt_status_t proof = {.version = 1,
                                .issued_at = (uint64_t)time(NULL),
                                .expires_at = (uint64_t)time(NULL) + 5,
                                .revocation_generation = 7};
      snprintf(proof.key_id, sizeof(proof.key_id), "status-key");
      snprintf(proof.caller_issuer, sizeof(proof.caller_issuer), "/CN=kb-ca");
      snprintf(proof.caller_serial_norm, sizeof(proof.caller_serial_norm), "01af");
      memset(proof.caller_fingerprint, 'a', 64);
      snprintf(proof.target_server_id, sizeof(proof.target_server_id), "srv-1");
      memset(proof.target_mgmt_fingerprint, 'b', 64);
      snprintf(proof.purpose, sizeof(proof.purpose), "%s",
               server_mgmt_read_selector_purpose(read_selector));
      assert(!kb_mgmt_status_sign(&proof, read_status_private_key));
      assert(!kb_mgmt_status_to_json(&proof, response, cap));
      *status = 200;
      read_sequence = 2;
      return KB_MANAGEMENT_HEALTH_OK;
   }
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
       "AIMEE_KB_MGMT_TOKEN_AUTHORITY_SOCKET",
       "AIMEE_KB_MGMT_TOKEN_AUTHORITY_GID",
       "AIMEE_KB_MGMT_TOKEN_ISSUER",
       "AIMEE_KB_MGMT_TOKEN_KID",
       "AIMEE_KB_MGMT_TOKEN_TTL_SECONDS",
   };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      unsetenv(names[i]);
}

static void write_ca(char path[256], int garbage)
{
   /* Widened from 64 with the callers': the path now carries TMPDIR, and the
    * old `> 0` check passes on TRUNCATION too, which would hand mkstemp a
    * chopped template and fail somewhere less obvious. Check it fits. */
   int n = snprintf(path, 256, "%s/aimee-p5b3b-ca-XXXXXX", platform_tmpdir());
   assert(n > 0 && n < 256);
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

static void set_packet(const char *ca_path, const char *status_public_hex)
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
   setenv("AIMEE_MGMT_STATUS_PUBLIC_KEY", status_public_hex, 1);
   setenv("AIMEE_KB_MGMT_TOKEN_AUTHORITY_SOCKET", KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH, 1);
   setenv("AIMEE_KB_MGMT_TOKEN_AUTHORITY_GID", "1000", 1);
   setenv("AIMEE_KB_MGMT_TOKEN_ISSUER", "https://issuer.example", 1);
   setenv("AIMEE_KB_MGMT_TOKEN_KID", "kid-1", 1);
   setenv("AIMEE_KB_MGMT_TOKEN_TTL_SECONDS", "90", 1);
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
   char ca_path[256], garbage_path[256];
   char status_public_hex[65];
   unsigned char status_public_key[32];
   EVP_PKEY_CTX *status_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   EVP_PKEY *status_key = NULL;
   size_t key_len = 32;
   assert(status_ctx && EVP_PKEY_keygen_init(status_ctx) == 1 &&
          EVP_PKEY_keygen(status_ctx, &status_key) == 1 &&
          EVP_PKEY_get_raw_private_key(status_key, read_status_private_key, &key_len) == 1 &&
          key_len == 32);
   key_len = 32;
   assert(EVP_PKEY_get_raw_public_key(status_key, status_public_key, &key_len) == 1 &&
          key_len == 32);
   for (size_t i = 0; i < 32; ++i)
      assert(snprintf(status_public_hex + i * 2, 3, "%02x", status_public_key[i]) == 2);
   EVP_PKEY_free(status_key);
   EVP_PKEY_CTX_free(status_ctx);
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

   set_packet(garbage_path, status_public_hex);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_DISABLED);
   clear_packet();

   set_packet(ca_path, status_public_hex);
   provider_result = KB_WORKLOAD_UNAVAILABLE;
   int registrations = register_calls;
   assert(kb_management_runtime_start() == 0);
   assert(register_calls == registrations + 1);
   assert(read_register_calls > 0);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_RETRY_WAIT);
   assert(kb_management_runtime_start() == -1);
   assert(kb_management_runtime_state() == KB_MANAGEMENT_RUNTIME_RETRY_WAIT);
   kb_management_runtime_stop();
   assert(unregister_calls >= 1);
   assert(read_unregister_calls >= 1);
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
   assert(captured_read_handler);
   kb_principal_t read_actor = {.authenticated = 1};
   char read_out[1024];
   read_fixture_enabled = 1;
   const struct
   {
      kb_mgmt_token_authority_ipc_result_t authority;
      kb_management_read_result_t read;
   } token_failures[] = {
       {KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED, KB_MANAGEMENT_READ_DENIED},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_CONFLICT, KB_MANAGEMENT_READ_CONFLICT},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED, KB_MANAGEMENT_READ_CONFLICT},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED, KB_MANAGEMENT_READ_CONFLICT},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID, KB_MANAGEMENT_READ_INTEGRITY},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY, KB_MANAGEMENT_READ_INTEGRITY},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED, KB_MANAGEMENT_READ_UNAVAILABLE},
       {KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS, KB_MANAGEMENT_READ_UNAVAILABLE},
   };
   for (size_t i = 0; i < sizeof(token_failures) / sizeof(token_failures[0]); ++i)
   {
      token_issue_result = token_failures[i].authority;
      read_sequence = 0;
      assert(captured_read_handler(NULL, &read_actor, 7, "srv-1", SERVER_MGMT_READ_SELECTOR_AGENTS,
                                   read_out, sizeof(read_out)) == token_failures[i].read);
      assert(read_sequence == 0);
   }
   token_issue_result = KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   read_sequence = 0;
   kb_management_read_result_t read_rc = captured_read_handler(
       NULL, &read_actor, 7, "srv-1", SERVER_MGMT_READ_SELECTOR_AGENTS, read_out, sizeof(read_out));
   assert(read_rc == KB_MANAGEMENT_READ_OK);
   assert(read_sequence == 3); /* token authority precedes status proof and dispatch */
   assert(strstr(read_out, "\"name\":\"agent-a\""));
   read_sequence = 0;
   assert(captured_read_handler(NULL, &read_actor, 7, "srv-1", SERVER_MGMT_READ_SELECTOR_CONFIG,
                                read_out, sizeof(read_out)) == KB_MANAGEMENT_READ_OK);
   assert(read_sequence == 3 && strstr(read_out, "\"client_transport\":\"auto\""));
   for (read_response_mode = 1; read_response_mode <= 2; ++read_response_mode)
   {
      read_sequence = 0;
      memset(read_out, 'x', sizeof(read_out));
      assert(captured_read_handler(NULL, &read_actor, 7, "srv-1", SERVER_MGMT_READ_SELECTOR_AGENTS,
                                   read_out, sizeof(read_out)) == KB_MANAGEMENT_READ_INTEGRITY);
      assert(read_sequence == 3 && read_out[0] == 0);
   }
   read_response_mode = 0;
   read_fixture_enabled = 0;
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
