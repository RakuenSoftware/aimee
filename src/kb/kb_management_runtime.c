#define _POSIX_C_SOURCE 200809L
#include "kb_management_runtime.h"
#include "kb_management_action.h"
#include "modules/db2/c/management_action_journal.h"
#include "modules/db2/c/management_read_journal.h"

#include "kb/http/kb_http_servers.h"
#include "kb_mgmt_endpoint.h"
#include "kb_mgmt_status_client.h"
#include "kb_tls.h"
#include "kb_workload_helper_posix.h"
#include "kb_workload_provider.h"
#include "management_read.h"
#include "cJSON.h"
#include <aimee/core/connection/auth.h>

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
   int action_registered;
   int read_registered;
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
   char token_socket[108];
   char token_issuer[256];
   char token_kid[65];
   gid_t token_socket_gid;
   int token_ttl_seconds;
} runtime_t;

static runtime_t runtime = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
    .state = KB_MANAGEMENT_RUNTIME_DISABLED,
};

static const char *const required_env[] = {
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
    "AIMEE_MGMT_STATUS_KEY_ID",
    "AIMEE_MGMT_STATUS_PUBLIC_KEY",
    "AIMEE_KB_MGMT_TOKEN_AUTHORITY_SOCKET",
    "AIMEE_KB_MGMT_TOKEN_AUTHORITY_GID",
    "AIMEE_KB_MGMT_TOKEN_ISSUER",
    "AIMEE_KB_MGMT_TOKEN_KID",
    "AIMEE_KB_MGMT_TOKEN_TTL_SECONDS",
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

static int canonical_uint(const char *text, uint64_t max, uint64_t *out)
{
   if (!text || !*text || (text[0] == '0' && text[1]) || !out)
      return -1;
   uint64_t value = 0;
   for (const unsigned char *p = (const unsigned char *)text; *p; p++)
   {
      if (*p < '0' || *p > '9' || value > (max - (uint64_t)(*p - '0')) / 10U)
         return -1;
      value = value * 10U + (uint64_t)(*p - '0');
   }
   *out = value;
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

static kb_management_action_result_t runtime_action(void *unused, const kb_principal_t *actor,
                                                    int64_t team_id, const char *server_id,
                                                    const char *body, size_t body_len)
{
   (void)unused;
   kb_management_cert_lifecycle_t *lifecycle = NULL;
   kb_management_health_server_config_t server = {0};
   kb_mgmt_status_client_config_t authority = {0};
   kb_mgmt_token_authority_client_config_t token = {0};
   char status_key_id[sizeof(runtime.status_key_id)];
   char token_issuer[sizeof(runtime.token_issuer)], kid[sizeof(runtime.token_kid)];
   char installation[sizeof(runtime.installation_id)];
   unsigned char status_public_key[32];
   int ttl = 0;

   pthread_mutex_lock(&runtime.mutex);
   if ((runtime.state != KB_MANAGEMENT_RUNTIME_READY &&
        runtime.state != KB_MANAGEMENT_RUNTIME_READY_DEGRADED) ||
       !runtime.lifecycle)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return KB_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   lifecycle = runtime.lifecycle;
   server.server_ca_pem = runtime.server_ca;
   authority.endpoint = runtime.status_endpoint;
   authority.ca_pem = runtime.status_ca;
   authority.leaf_pin = runtime.status_leaf_pin;
   authority.secondary_leaf_pin =
       runtime.status_secondary_leaf_pin[0] ? runtime.status_secondary_leaf_pin : NULL;
   token.socket_path = runtime.token_socket;
   token.socket_gid = runtime.token_socket_gid;
   token.socket_mode = 0660;
   token.timeout_ms = KB_MGMT_TOKEN_AUTHORITY_IO_TIMEOUT_MS;
   memcpy(status_key_id, runtime.status_key_id, sizeof(status_key_id));
   memcpy(status_public_key, runtime.status_public_key, sizeof(status_public_key));
   memcpy(token_issuer, runtime.token_issuer, sizeof(token_issuer));
   memcpy(kid, runtime.token_kid, sizeof(kid));
   memcpy(installation, runtime.installation_id, sizeof(installation));
   ttl = runtime.token_ttl_seconds;
   pthread_mutex_unlock(&runtime.mutex);

   uint64_t now = monotonic_millis(NULL);
   if (now == UINT64_MAX || now > UINT64_MAX - 15000U)
      return KB_MANAGEMENT_ACTION_UNAVAILABLE;
   kb_management_action_request_t request = {.actor = actor,
                                             .team_id = team_id,
                                             .server_id = server_id,
                                             .body = body,
                                             .body_len = body_len,
                                             .deadline_millis = now + 15000U};
   kb_management_action_dependencies_t deps = {
       .operation_init = db2_management_action_operation_init,
       .intent_start = db2_management_action_intent_start,
       .outcome_append = db2_management_action_outcome_append,
       .snapshot = kb_management_health_snapshot_primary,
       .bundle_load = kb_management_health_bundle_active,
       .bundle_ctx = lifecycle,
       .bundle_clear = kb_management_health_bundle_cleanse,
       .server_open = kb_management_health_server_open_production,
       .server_ctx = &server,
       .server_request = kb_management_action_server_request_production,
       .server_close = kb_management_health_server_close_production,
       .authority_issue = kb_mgmt_status_client_adapter,
       .authority_ctx = &authority,
       .token_issue = kb_management_action_token_issue_production,
       .token_ctx = &token,
       .wall_seconds = wall_seconds,
       .monotonic_millis = monotonic_millis,
       .status_key_id = status_key_id,
       .status_public_key = status_public_key,
       .token_issuer = token_issuer,
       .kid = kid,
       .installation_id = installation,
       .ttl_seconds = ttl,
   };
   kb_management_action_result_t result = kb_management_action_execute(&request, &deps);
   OPENSSL_cleanse(status_public_key, sizeof(status_public_key));
   return result;
}

static void hex32_runtime(const uint8_t in[32], char out[65])
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
      out[i * 2] = digits[in[i] >> 4], out[i * 2 + 1] = digits[in[i] & 15];
   out[64] = 0;
}

static void nonce64_runtime(const unsigned char in[32], char out[44])
{
   static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t n = 0;
   for (size_t i = 0; i < 32; ++i)
   {
      acc = (acc << 8) | in[i];
      bits += 8;
      while (bits >= 6)
      {
         bits -= 6;
         out[n++] = abc[(acc >> bits) & 63];
         acc &= bits ? (1U << bits) - 1U : 0U;
      }
   }
   if (bits)
      out[n++] = abc[(acc << (6 - bits)) & 63];
   out[n] = 0;
}

static int read_snapshot_matches(const db2_server_snapshot_t *s,
                                 const db2_management_read_intent_t *i)
{
   return s && i && !strcmp(s->server_id, i->target_server_id) &&
          !kb_mgmt_endpoint_validate(s->endpoint) && !strcmp(s->status, "active") &&
          !strcmp(s->enrollment_state, "active") && !s->revoked_at[0] &&
          !strcmp(s->management_issuer, i->target_mgmt_issuer) &&
          !strcmp(s->management_serial_norm, i->target_mgmt_serial_norm) &&
          !strcmp(s->management_fingerprint, i->target_mgmt_fingerprint) &&
          s->revocation_generation == i->revocation_generation;
}

static int read_status_matches(const char *wire, const unsigned char nonce[32],
                               const kb_management_cert_active_t *active,
                               const db2_management_read_intent_t *intent, const char *key_id,
                               const unsigned char public_key[32], uint64_t now,
                               const char *purpose)
{
   kb_mgmt_status_t status;
   char fingerprint[65];
   memset(&status, 0, sizeof(status));
   hex32_runtime(active->fingerprint, fingerprint);
   int bad = kb_mgmt_status_from_json(wire, &status) || strcmp(status.key_id, key_id) ||
             CRYPTO_memcmp(status.nonce, nonce, 32) ||
             strcmp(status.caller_issuer, active->issuer) ||
             strcmp(status.caller_serial_norm, active->serial_norm) ||
             strcmp(status.caller_fingerprint, fingerprint) ||
             strcmp(status.target_server_id, intent->target_server_id) ||
             strcmp(status.target_mgmt_fingerprint, intent->target_mgmt_fingerprint) || !purpose ||
             strcmp(status.purpose, purpose) ||
             status.revocation_generation != (uint64_t)intent->revocation_generation ||
             kb_mgmt_status_validate(&status, now, (uint64_t)intent->revocation_generation) ||
             kb_mgmt_status_verify_signature(&status, public_key);
   OPENSSL_cleanse(&status, sizeof(status));
   return !bad;
}

static int read_projection_token(const char *s, size_t max, int model)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-' || (model && (c == ':' || c == '/' || c == '+'))))
         return 0;
   }
   return 1;
}

static int read_object_exact(const cJSON *object, const char *const *names, size_t count)
{
   if (!cJSON_IsObject(object) || !names || !count || count > 32)
      return 0;
   uint32_t seen = 0;
   size_t fields = 0;
   for (const cJSON *field = object->child; field; field = field->next)
   {
      size_t i = 0;
      while (i < count && (!field->string || strcmp(field->string, names[i])))
         ++i;
      if (i == count || (seen & (UINT32_C(1) << i)))
         return 0;
      seen |= UINT32_C(1) << i;
      ++fields;
   }
   return fields == count && seen == (UINT32_C(1) << count) - 1;
}

static int read_agents_projection_valid(const char *wire, size_t len, const char *server,
                                        int64_t team)
{
   if (!wire || !len || len > 32768 || memchr(wire, 0, len))
      return 0;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(wire, len, &end, 0);
   cJSON *sid = root ? cJSON_GetObjectItemCaseSensitive(root, "server_id") : NULL;
   cJSON *tid = root ? cJSON_GetObjectItemCaseSensitive(root, "team") : NULL;
   cJSON *agents = root ? cJSON_GetObjectItemCaseSensitive(root, "agents") : NULL;
   static const char *const root_fields[] = {"server_id", "team", "agents"};
   char team_token[48];
   int team_n = snprintf(team_token, sizeof(team_token), "\"team\":%lld", (long long)team);
   int valid = root && end == wire + len &&
               read_object_exact(root, root_fields, sizeof(root_fields) / sizeof(root_fields[0])) &&
               cJSON_IsString(sid) && !strcmp(cJSON_GetStringValue(sid), server) &&
               cJSON_IsNumber(tid) && tid->valuedouble == (double)team && cJSON_IsArray(agents) &&
               cJSON_GetArraySize(agents) <= SERVER_MGMT_READ_AGENT_MAX && team_n > 0 &&
               (size_t)team_n < sizeof(team_token) && strstr(wire, team_token);
   const char *previous = NULL;
   cJSON *agent = NULL;
   cJSON_ArrayForEach(agent, agents)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(agent, "name");
      cJSON *provider = cJSON_GetObjectItemCaseSensitive(agent, "provider");
      cJSON *model = cJSON_GetObjectItemCaseSensitive(agent, "model");
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(agent, "enabled");
      cJSON *delegate = cJSON_GetObjectItemCaseSensitive(agent, "delegate_available");
      cJSON *primary = cJSON_GetObjectItemCaseSensitive(agent, "primary_only");
      cJSON *parallel = cJSON_GetObjectItemCaseSensitive(agent, "max_parallel");
      static const char *const agent_fields[] = {
          "name",         "provider",    "model", "enabled", "delegate_available",
          "primary_only", "max_parallel"};
      const char *name_s = cJSON_IsString(name) ? cJSON_GetStringValue(name) : NULL;
      const char *provider_s = cJSON_IsString(provider) ? cJSON_GetStringValue(provider) : NULL;
      const char *model_s = cJSON_IsString(model) ? cJSON_GetStringValue(model) : NULL;
      if (!valid ||
          !read_object_exact(agent, agent_fields, sizeof(agent_fields) / sizeof(agent_fields[0])) ||
          !read_projection_token(name_s, 63, 0) || !read_projection_token(provider_s, 15, 0) ||
          !read_projection_token(model_s, 127, 1) || !cJSON_IsBool(enabled) ||
          !cJSON_IsBool(delegate) || !cJSON_IsBool(primary) || !cJSON_IsNumber(parallel) ||
          parallel->valuedouble < 0 || parallel->valuedouble > 1024 ||
          parallel->valuedouble != (double)parallel->valueint ||
          (previous && strcmp(previous, name_s) >= 0))
      {
         valid = 0;
         break;
      }
      previous = name_s;
   }
   cJSON_Delete(root);
   return valid;
}

static int read_config_projection_valid(const char *wire, size_t len, const char *server,
                                        int64_t team)
{
   if (!wire || !len || len > 32768 || memchr(wire, 0, len))
      return 0;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(wire, len, &end, 0);
   cJSON *sid = root ? cJSON_GetObjectItemCaseSensitive(root, "server_id") : NULL;
   cJSON *tid = root ? cJSON_GetObjectItemCaseSensitive(root, "team") : NULL;
   cJSON *config = root ? cJSON_GetObjectItemCaseSensitive(root, "config") : NULL;
   static const char *const root_fields[] = {"server_id", "team", "config"};
   static const char *const config_fields[] = {"mtls", "remote_writes", "client_transport",
                                               "cli_session_forwarding", "require_aimee_git"};
   cJSON *mtls = config ? cJSON_GetObjectItemCaseSensitive(config, "mtls") : NULL;
   cJSON *writes = config ? cJSON_GetObjectItemCaseSensitive(config, "remote_writes") : NULL;
   cJSON *transport = config ? cJSON_GetObjectItemCaseSensitive(config, "client_transport") : NULL;
   cJSON *forward =
       config ? cJSON_GetObjectItemCaseSensitive(config, "cli_session_forwarding") : NULL;
   cJSON *git = config ? cJSON_GetObjectItemCaseSensitive(config, "require_aimee_git") : NULL;
   const char *m = cJSON_IsString(mtls) ? cJSON_GetStringValue(mtls) : NULL;
   const char *w = cJSON_IsString(writes) ? cJSON_GetStringValue(writes) : NULL;
   const char *t = cJSON_IsString(transport) ? cJSON_GetStringValue(transport) : NULL;
   char team_token[48];
   int team_n = snprintf(team_token, sizeof(team_token), "\"team\":%lld", (long long)team);
   int valid =
       root && end == wire + len &&
       read_object_exact(root, root_fields, sizeof(root_fields) / sizeof(root_fields[0])) &&
       read_object_exact(config, config_fields, sizeof(config_fields) / sizeof(config_fields[0])) &&
       cJSON_IsString(sid) && !strcmp(cJSON_GetStringValue(sid), server) && cJSON_IsNumber(tid) &&
       tid->valuedouble == (double)team && m &&
       (!strcmp(m, "off") || !strcmp(m, "optional") || !strcmp(m, "required")) && w &&
       (!strcmp(w, "off") || !strcmp(w, "data") || !strcmp(w, "full")) && t &&
       (!strcmp(t, "socket") || !strcmp(t, "http") || !strcmp(t, "auto")) &&
       cJSON_IsBool(forward) && cJSON_IsBool(git) && team_n > 0 &&
       (size_t)team_n < sizeof(team_token) && strstr(wire, team_token);
   cJSON_Delete(root);
   return valid;
}

static kb_management_read_result_t runtime_read(void *unused, const kb_principal_t *actor,
                                                int64_t team_id, const char *server_id,
                                                server_mgmt_read_selector_t selector, char *out,
                                                size_t cap)
{
   (void)unused;
   const char *selector_name = server_mgmt_read_selector_name(selector);
   const char *purpose = server_mgmt_read_selector_purpose(selector);
   if (!actor || !actor->authenticated || team_id < 1 || !server_id || !selector_name || !purpose ||
       !out || cap < 2)
      return KB_MANAGEMENT_READ_INVALID;
   out[0] = 0;
   kb_management_cert_lifecycle_t *lifecycle = NULL;
   kb_management_health_server_config_t server_cfg = {0};
   kb_mgmt_status_client_config_t authority = {0};
   kb_mgmt_token_authority_client_config_t token = {0};
   char status_key_id[65], token_issuer[256], installation[33];
   unsigned char status_public_key[32];
   int ttl = 0;
   pthread_mutex_lock(&runtime.mutex);
   if ((runtime.state != KB_MANAGEMENT_RUNTIME_READY &&
        runtime.state != KB_MANAGEMENT_RUNTIME_READY_DEGRADED) ||
       !runtime.lifecycle)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return KB_MANAGEMENT_READ_UNAVAILABLE;
   }
   lifecycle = runtime.lifecycle;
   server_cfg.server_ca_pem = runtime.server_ca;
   authority.endpoint = runtime.status_endpoint;
   authority.ca_pem = runtime.status_ca;
   authority.leaf_pin = runtime.status_leaf_pin;
   authority.secondary_leaf_pin =
       runtime.status_secondary_leaf_pin[0] ? runtime.status_secondary_leaf_pin : NULL;
   token.socket_path = runtime.token_socket;
   token.socket_gid = runtime.token_socket_gid;
   token.socket_mode = 0660;
   token.timeout_ms = KB_MGMT_TOKEN_AUTHORITY_IO_TIMEOUT_MS;
   memcpy(status_key_id, runtime.status_key_id, sizeof(status_key_id));
   memcpy(status_public_key, runtime.status_public_key, sizeof(status_public_key));
   memcpy(token_issuer, runtime.token_issuer, sizeof(token_issuer));
   memcpy(installation, runtime.installation_id, sizeof(installation));
   ttl = runtime.token_ttl_seconds;
   pthread_mutex_unlock(&runtime.mutex);

   db2_server_snapshot_t snapshot = {0};
   kb_management_cert_bundle_t bundle = {0};
   kb_management_cert_active_t active = {0};
   db2_management_read_intent_t intent = {0};
   kb_mgmt_token_authority_output_t bearer = {0};
   unsigned char nonce[32] = {0};
   char challenge[512] = {0}, staple[KB_MGMT_STATUS_JSON_MAX + 1] = {0};
   char status_request[1024] = {0};
   char headers[KB_MGMT_TOKEN_WIRE_MAX + KB_MGMT_STATUS_JSON_MAX + 96] = {0};
   char *server_response = NULL;
   void *session = NULL;
   int loaded = 0, http_status = 0;
   kb_management_read_result_t result = KB_MANAGEMENT_READ_UNAVAILABLE;
   uint64_t mono = monotonic_millis(NULL);
   uint64_t deadline = mono > UINT64_MAX - 15000 ? UINT64_MAX : mono + 15000;
   kb_management_health_result_t hr =
       kb_management_health_snapshot_primary(NULL, actor, team_id, server_id, &snapshot);
   if (hr != KB_MANAGEMENT_HEALTH_OK)
   {
      result = hr == KB_MANAGEMENT_HEALTH_NOT_FOUND ? KB_MANAGEMENT_READ_NOT_FOUND
               : hr == KB_MANAGEMENT_HEALTH_DENIED  ? KB_MANAGEMENT_READ_DENIED
                                                    : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   loaded = 1;
   if (kb_management_health_bundle_active(lifecycle, &bundle, &active) != KB_MANAGEMENT_HEALTH_OK)
      goto done;
   if (kb_management_health_server_open_production(&server_cfg, &snapshot, &bundle, deadline,
                                                   &session) != KB_MANAGEMENT_HEALTH_OK)
      goto done;
   const char *challenge_path = selector == SERVER_MGMT_READ_SELECTOR_AGENTS
                                    ? "/v1/management/read/challenge"
                                    : "/v1/management/read/config/challenge";
   if (kb_management_action_server_request_production(
           NULL, session, "POST", challenge_path, "", NULL, deadline, challenge, sizeof(challenge),
           &http_status) != KB_MANAGEMENT_ACTION_SENT_RESPONSE ||
       http_status != 200)
      goto done;
   uint64_t expires = 0, now = wall_seconds(NULL);
   if (kb_management_read_challenge_decode(challenge, strlen(challenge), purpose, nonce,
                                           &expires) ||
       expires <= now || expires - now > 15)
   {
      result = KB_MANAGEMENT_READ_INTEGRITY;
      goto done;
   }
   uint64_t challenge_now = monotonic_millis(NULL);
   uint64_t challenge_deadline = challenge_now + (expires - now) * 1000;
   if (challenge_deadline < deadline)
      deadline = challenge_deadline;
   char external_path[256];
   if (server_mgmt_read_selector_path(selector, server_id, external_path, sizeof(external_path)) <
       0)
      goto done;
   int64_t publication_generation = 0;
   db2_management_read_result_t generation_result =
       db2_management_read_publication_generation(&publication_generation);
   if (generation_result != DB2_MANAGEMENT_READ_OK)
   {
      result = generation_result == DB2_MANAGEMENT_READ_DENIED ? KB_MANAGEMENT_READ_DENIED
               : generation_result == DB2_MANAGEMENT_READ_INTEGRITY
                   ? KB_MANAGEMENT_READ_INTEGRITY
                   : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   server_mgmt_read_digest_input_t digest_input = {.server_id = server_id,
                                                   .team_id = team_id,
                                                   .nonce = nonce,
                                                   .kb_issuer = active.issuer,
                                                   .kb_serial = active.serial_norm,
                                                   .server_issuer = snapshot.management_issuer,
                                                   .server_serial = snapshot.management_serial_norm,
                                                   .revocation_generation =
                                                       snapshot.revocation_generation,
                                                   .publication_generation = publication_generation,
                                                   .selector = selector};
   char digest[65];
   if (server_mgmt_read_digest(&digest_input, digest))
      goto done;
   db2_management_read_result_t jr =
       db2_management_read_intent_start(actor, team_id, server_id, selector, external_path, nonce,
                                        digest, token_issuer, installation, ttl, &intent);
   if (jr != DB2_MANAGEMENT_READ_OK)
   {
      result = jr == DB2_MANAGEMENT_READ_DENIED      ? KB_MANAGEMENT_READ_DENIED
               : jr == DB2_MANAGEMENT_READ_CONFLICT  ? KB_MANAGEMENT_READ_CONFLICT
               : jr == DB2_MANAGEMENT_READ_INTEGRITY ? KB_MANAGEMENT_READ_INTEGRITY
                                                     : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   char active_fp[65];
   hex32_runtime(active.fingerprint, active_fp);
   if (!read_snapshot_matches(&snapshot, &intent) || strcmp(active.installation_id, installation) ||
       strcmp(active.issuer, intent.local_cert_issuer) ||
       strcmp(active.serial_norm, intent.local_cert_serial_norm) ||
       strcmp(active_fp, intent.local_cert_fingerprint) || strcmp(digest, intent.request_sha256) ||
       intent.publication_generation != publication_generation)
   {
      result = KB_MANAGEMENT_READ_INTEGRITY;
      goto done;
   }
   if (monotonic_millis(NULL) >= deadline)
      goto done;
   kb_mgmt_token_authority_ipc_result_t token_result =
       kb_mgmt_token_authority_client_issue(&token, intent.correlation_id, intent.jti, &bearer);
   if (token_result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
   {
      result = token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED ? KB_MANAGEMENT_READ_DENIED
               : token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_CONFLICT ||
                       token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED ||
                       token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED
                   ? KB_MANAGEMENT_READ_CONFLICT
               : token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID ||
                       token_result == KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY
                   ? KB_MANAGEMENT_READ_INTEGRITY
                   : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   char encoded[44];
   nonce64_runtime(nonce, encoded);
   int n = snprintf(status_request, sizeof(status_request),
                    "{\"nonce\":\"%s\",\"target\":\"%s\",\"target_mgmt_fp\":\"%s\","
                    "\"purpose\":\"%s\"}",
                    encoded, server_id, intent.target_mgmt_fingerprint, purpose);
   if (n < 0 || (size_t)n >= sizeof(status_request))
      goto done;
   kb_management_health_result_t status_result =
       kb_mgmt_status_client_adapter(&authority, &bundle, status_request, (size_t)n, deadline,
                                     staple, sizeof(staple), &http_status);
   if (status_result != KB_MANAGEMENT_HEALTH_OK)
      goto done;
   if (http_status != 200 || !read_status_matches(staple, nonce, &active, &intent, status_key_id,
                                                  status_public_key, wall_seconds(NULL), purpose))
   {
      result = KB_MANAGEMENT_READ_INTEGRITY;
      goto done;
   }
   db2_server_snapshot_t current = {0};
   hr = kb_management_health_snapshot_primary(NULL, actor, team_id, server_id, &current);
   if (hr != KB_MANAGEMENT_HEALTH_OK || !read_snapshot_matches(&current, &intent) ||
       strcmp(current.endpoint, snapshot.endpoint))
   {
      result = hr == KB_MANAGEMENT_HEALTH_OK ? KB_MANAGEMENT_READ_INTEGRITY
                                             : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   if (monotonic_millis(NULL) >= deadline)
      goto done;
   char authorization[KB_MGMT_TOKEN_WIRE_MAX + 8] = "";
   if (aimee_core_bearer_value(authorization, sizeof(authorization), bearer.jwt) != 0)
      goto done;
   n = snprintf(headers, sizeof(headers), "Authorization: %s\r\nX-Aimee-Management-Status: %s\r\n",
                authorization, staple);
   if (n < 0 || (size_t)n >= sizeof(headers))
      goto done;
   server_response = calloc(32769, 1);
   if (!server_response)
      goto done;
   const char *server_path = selector == SERVER_MGMT_READ_SELECTOR_AGENTS
                                 ? "/v1/management/read/agents"
                                 : "/v1/management/read/config";
   if (kb_management_action_server_request_production(
           NULL, session, "GET", server_path, "", headers, deadline, server_response, 32769,
           &http_status) != KB_MANAGEMENT_ACTION_SENT_RESPONSE)
      goto done;
   size_t response_len = strnlen(server_response, 32769);
   if (http_status != 200 || response_len == 32769 ||
       !(selector == SERVER_MGMT_READ_SELECTOR_AGENTS
             ? read_agents_projection_valid(server_response, response_len, server_id, team_id)
             : read_config_projection_valid(server_response, response_len, server_id, team_id)))
   {
      result = http_status == 403                         ? KB_MANAGEMENT_READ_DENIED
               : http_status == 409                       ? KB_MANAGEMENT_READ_CONFLICT
               : http_status == 502 || http_status == 200 ? KB_MANAGEMENT_READ_INTEGRITY
                                                          : KB_MANAGEMENT_READ_UNAVAILABLE;
      goto done;
   }
   if (response_len >= cap)
      goto done;
   memcpy(out, server_response, response_len + 1);
   result = KB_MANAGEMENT_READ_OK;
done:
   if (session)
      kb_management_health_server_close_production(NULL, session);
   if (loaded)
      kb_management_health_bundle_cleanse(NULL, &bundle);
   OPENSSL_cleanse(&bundle, sizeof(bundle));
   OPENSSL_cleanse(&active, sizeof(active));
   OPENSSL_cleanse(&bearer, sizeof(bearer));
   OPENSSL_cleanse(status_public_key, sizeof(status_public_key));
   if (server_response)
   {
      OPENSSL_cleanse(server_response, 32769);
      free(server_response);
   }
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
   memset(runtime.token_socket, 0, sizeof(runtime.token_socket));
   memset(runtime.token_issuer, 0, sizeof(runtime.token_issuer));
   memset(runtime.token_kid, 0, sizeof(runtime.token_kid));
   runtime.token_socket_gid = 0;
   runtime.token_ttl_seconds = 0;
}

int kb_management_runtime_start(void)
{
   pthread_mutex_lock(&runtime.mutex);
   int inactive = runtime.state == KB_MANAGEMENT_RUNTIME_DISABLED && !runtime.configured &&
                  !runtime.registered && !runtime.action_registered && !runtime.read_registered &&
                  !runtime.provider && !runtime.lifecycle;
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
   char gid_text[32], ttl_text[8];
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
       copy_env(public_hex, sizeof(public_hex), required_env[13]) ||
       copy_env(runtime.token_socket, sizeof(runtime.token_socket), required_env[14]) ||
       copy_env(gid_text, sizeof(gid_text), required_env[15]) ||
       copy_env(runtime.token_issuer, sizeof(runtime.token_issuer), required_env[16]) ||
       copy_env(runtime.token_kid, sizeof(runtime.token_kid), required_env[17]) ||
       copy_env(ttl_text, sizeof(ttl_text), required_env[18]))
      goto invalid;
   if (secondary && *secondary)
   {
      if (strnlen(secondary, sizeof(runtime.status_secondary_leaf_pin)) != 64)
         goto invalid;
      memcpy(runtime.status_secondary_leaf_pin, secondary, 65);
   }
   uint64_t gid_value = 0, ttl_value = 0;
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
       !valid_ca_pem(runtime.server_ca) || !valid_ca_pem(runtime.status_ca) ||
       strcmp(runtime.token_socket, KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH) ||
       canonical_uint(gid_text, (uint64_t)(gid_t)-1, &gid_value) ||
       canonical_uint(ttl_text, 90, &ttl_value) || ttl_value < 1 ||
       !printable(runtime.token_issuer, 255) || !token(runtime.token_kid, 64))
      goto invalid;
   runtime.token_socket_gid = (gid_t)gid_value;
   runtime.token_ttl_seconds = (int)ttl_value;

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
   if (kb_http_servers_action_register(runtime_action, NULL))
      goto invalid_registered;
   pthread_mutex_lock(&runtime.mutex);
   runtime.action_registered = 1;
   pthread_mutex_unlock(&runtime.mutex);
   if (kb_http_servers_read_register(runtime_read, NULL))
      goto invalid_action_registered;
   pthread_mutex_lock(&runtime.mutex);
   runtime.read_registered = 1;
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

invalid_action_registered:
   (void)kb_http_servers_action_unregister(runtime_action, NULL);
   pthread_mutex_lock(&runtime.mutex);
   runtime.action_registered = 0;
   pthread_mutex_unlock(&runtime.mutex);

invalid_registered:
   (void)kb_http_servers_health_unregister(runtime_health, NULL);
   pthread_mutex_lock(&runtime.mutex);
   runtime.registered = 0;
   pthread_mutex_unlock(&runtime.mutex);

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
   int action_registered = runtime.action_registered;
   int read_registered = runtime.read_registered;
   runtime.registered = 0;
   runtime.action_registered = 0;
   runtime.read_registered = 0;
   pthread_mutex_unlock(&runtime.mutex);
   if (read_registered)
      (void)kb_http_servers_read_unregister(runtime_read, NULL);
   if (action_registered)
      (void)kb_http_servers_action_unregister(runtime_action, NULL);
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
