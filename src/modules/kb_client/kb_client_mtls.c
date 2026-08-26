/* kb_client_mtls.c: aimee-server's distributed-mode mTLS transport to a remote
 * aimee-kb. (distributed-mode-auth proposal, client integration.)
 *
 * When the server Vault holds an `AIMEE_KB_CONN` `aimee://` connection string,
 * the server enrolls
 * once (kb_tls_enroll: TOFU-pin the CA by fingerprint, generate a keypair + CSR,
 * redeem the token for a client cert), persists that identity atomically under
 * AIMEE_HOME, and then routes its /v1 kb calls over mutual TLS. Persistence is
 * mandatory because the enrollment token is single-use: keeping the certificate
 * only in process memory makes the next container restart permanently lose KB.
 * Selected at the top of the kb_client v1 transport (kb_client.c) ahead of the
 * HTTP-URL and Unix-socket transports. */
#include "kb_client_mtls.h"
#include "kb_enroll.h" /* connection-string parse (for host/port) */
#include "kb_identity_token.h"
#include "kb_pki.h"
#include "kb_tls.h" /* kb_tls_enroll / kb_tls_client_request */
#include "config.h"
#include "cJSON.h"
#include "log.h"
#include "runtime_secret.h"
#include "util.h"
#include <aimee/core/connection/auth.h>

#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The enrolled identity, established once on first use. Immutable after init
 * (guarded by g_lock during enrollment), so request-time reads need no lock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_enrolled = 0;
static char g_host[256];
static int g_port = 0;
static char g_ca[8192];
static char g_cert[8192];
static char g_key[8192];
static char g_identity_path_override[1024];
static char g_server_identity_path_override[1024];

#define KB_CLIENT_IDENTITY_MAX 32768
#define KB_CLIENT_HEADERS_MAX  16384
#define KB_CLIENT_PAM_USER_MAX 63
#define KB_CLIENT_PAM_PASS_MAX 1023
#define KB_CLIENT_CALLER_MAX   576

/* Only aimee-server links the request-context module. Other binaries reuse the
 * transport for service/background work and therefore have no caller context. */
extern const char *request_context_caller_subject(void) __attribute__((weak));
extern const char *request_context_caller_authorization(void) __attribute__((weak));

static int caller_subject_valid(const char *subject)
{
   if (!subject || !subject[0])
      return 0;
   size_t n = strnlen(subject, KB_CLIENT_CALLER_MAX + 1);
   if (!n || n > KB_CLIENT_CALLER_MAX)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)subject[i] < 0x20 || (unsigned char)subject[i] == 0x7f)
         return 0;
   return 1;
}

static int caller_authorization_valid(const char *jwt)
{
   if (!jwt || !jwt[0])
      return 0;
   size_t n = strnlen(jwt, KB_IDENTITY_TOKEN_WIRE_MAX + 1);
   if (!n || n > KB_IDENTITY_TOKEN_WIRE_MAX)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)jwt[i] <= 0x20 || (unsigned char)jwt[i] == 0x7f)
         return 0;
   return 1;
}

/* Read both independent application credentials for every request. Neither is
 * cached with the mTLS identity: bearer/service-token/OIDC/PAM rotation must take effect on
 * request N+1 even while the HTTP/TLS connection remains pooled. */
static int service_request_headers(char *out, size_t cap)
{
   char token[KB_TLS_BEARER_TOKEN_MAX + 1] = "";
   char value[KB_TLS_BEARER_TOKEN_MAX + 8] = "";
   char oidc[KB_TLS_BEARER_TOKEN_MAX + 1] = "";
   char oidc_value[KB_TLS_BEARER_TOKEN_MAX + 8] = "";
   char service_token[KB_TLS_BEARER_TOKEN_MAX + 1] = "";
   char service_value[KB_TLS_BEARER_TOKEN_MAX + 8] = "";
   char pam_user[KB_CLIENT_PAM_USER_MAX + 1] = "";
   char pam_pass[KB_CLIENT_PAM_PASS_MAX + 1] = "";
   char pam_pair[KB_CLIENT_PAM_USER_MAX + KB_CLIENT_PAM_PASS_MAX + 2] = "";
   char pam_b64[2048] = "";
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   int have = runtime_secret_get("AIMEE_KB_CLIENT_BEARER_TOKEN", token, sizeof(token));
   if (!have)
      have = runtime_secret_get("AIMEE_KB_API_BEARER_TOKEN", token, sizeof(token));
   if (!have)
      LOG_ERROR("kb_client", "server-to-KB request missing rotating bearer credential");
   int n = have && aimee_core_bearer_value(value, sizeof(value), token) == 0
               ? snprintf(out, cap, "Authorization: %s\r\n", value)
               : -1;
   int have_oidc = runtime_secret_get("AIMEE_KB_CLIENT_OIDC_TOKEN", oidc, sizeof(oidc));
   int have_service = runtime_secret_get("AIMEE_KB_SERVICE_IDENTITY_TOKEN", service_token,
                                         sizeof(service_token));
   int have_pam_user =
       runtime_secret_get("AIMEE_KB_CLIENT_PAM_USERNAME", pam_user, sizeof(pam_user));
   int have_pam_pass =
       runtime_secret_get("AIMEE_KB_CLIENT_PAM_PASSWORD", pam_pass, sizeof(pam_pass));
   if (n > 0 && (size_t)n < cap && have_oidc &&
       aimee_core_bearer_value(oidc_value, sizeof(oidc_value), oidc) == 0)
   {
      int added =
          snprintf(out + n, cap - (size_t)n, "X-Aimee-Service-Authorization: %s\r\n", oidc_value);
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   else if (n > 0 && (size_t)n < cap && have_service &&
            aimee_core_bearer_value(service_value, sizeof(service_value), service_token) == 0)
   {
      int added = snprintf(out + n, cap - (size_t)n,
                           "X-Aimee-Service-Authorization: %s\r\n", service_value);
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   else if (n > 0 && (size_t)n < cap && have_pam_user && have_pam_pass)
   {
      int pair_len = snprintf(pam_pair, sizeof(pam_pair), "%s:%s", pam_user, pam_pass);
      size_t encoded = pair_len > 0 && (size_t)pair_len < sizeof(pam_pair)
                           ? aimee_base64_encode((const unsigned char *)pam_pair, (size_t)pair_len,
                                                 pam_b64, sizeof(pam_b64))
                           : 0;
      int added = encoded > 0 ? snprintf(out + n, cap - (size_t)n,
                                         "X-Aimee-Service-Authorization: Basic %s\r\n", pam_b64)
                              : -1;
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   else
   {
      if (n > 0 && (size_t)n < cap)
         LOG_ERROR("kb_client",
                   "server-to-KB request missing third-layer identity (managed service token, "
                   "OIDC token, or complete PAM username/password pair)");
      n = -1;
   }
   char managed_server[128] = "";
   long long managed_team = 0;
   if (n > 0 && (size_t)n < cap &&
       kb_client_mtls_managed_metadata(managed_server, sizeof(managed_server), &managed_team) &&
       managed_server[0] && managed_team > 0)
   {
      int added =
          snprintf(out + n, cap - (size_t)n, "X-Aimee-Server-ID: %s\r\nX-Aimee-Team-ID: %lld\r\n",
                   managed_server, managed_team);
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   const char *caller_authorization =
       request_context_caller_authorization ? request_context_caller_authorization() : "";
   const char *caller = request_context_caller_subject ? request_context_caller_subject() : "";
   if (n > 0 && (size_t)n < cap && caller_authorization && caller_authorization[0])
   {
      int added =
          caller_authorization_valid(caller_authorization)
              ? snprintf(out + n, cap - (size_t)n, "X-Aimee-Caller-Authorization: Bearer %s\r\n",
                         caller_authorization)
              : -1;
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   else if (n > 0 && (size_t)n < cap && caller && caller[0])
   {
      int added = caller_subject_valid(caller)
                      ? snprintf(out + n, cap - (size_t)n, "X-Aimee-Caller-Subject: %s\r\n", caller)
                      : -1;
      n = added > 0 && (size_t)added < cap - (size_t)n ? n + added : -1;
   }
   runtime_secret_wipe(managed_server, sizeof(managed_server));
   runtime_secret_wipe(pam_b64, sizeof(pam_b64));
   runtime_secret_wipe(pam_pair, sizeof(pam_pair));
   runtime_secret_wipe(pam_pass, sizeof(pam_pass));
   runtime_secret_wipe(pam_user, sizeof(pam_user));
   runtime_secret_wipe(service_value, sizeof(service_value));
   runtime_secret_wipe(service_token, sizeof(service_token));
   runtime_secret_wipe(oidc_value, sizeof(oidc_value));
   runtime_secret_wipe(oidc, sizeof(oidc));
   runtime_secret_wipe(value, sizeof(value));
   runtime_secret_wipe(token, sizeof(token));
   if (n <= 0 || (size_t)n >= cap)
   {
      runtime_secret_wipe(out, cap);
      return -1;
   }
   return 0;
}

static int identity_path(char *out, size_t cap)
{
   const char *base = config_default_dir();
   int n = g_identity_path_override[0]
               ? snprintf(out, cap, "%s", g_identity_path_override)
               : snprintf(out, cap, "%s/kb-client-identity.json", base ? base : "");
   return n > 0 && (size_t)n < cap && out[0] == '/' ? 0 : -1;
}

/* The server-to-KB client identity and the thinclient-facing server identity
 * are different mTLS pairs. Server-to-KB use requires the listener identity to
 * exist and rejects key reuse even when both leaves have the right role EKU. */
static int identity_distinct_from_server(const char *client_cert_pem)
{
   char path[1024];
   const char *base = config_default_dir();
   int n = g_server_identity_path_override[0]
               ? snprintf(path, sizeof(path), "%s", g_server_identity_path_override)
               : snprintf(path, sizeof(path), "%s/tls/server.crt", base ? base : "");
   if (n <= 0 || (size_t)n >= sizeof(path) || path[0] != '/')
      return 0;

   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return 0; /* configured server->KB use requires the other pair to exist */
   struct stat st;
   int valid_file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                    st.st_size > 0 && st.st_size < KB_PKI_CERT_PEM_MAX;
   BIO *server_bio = valid_file ? BIO_new_fd(fd, BIO_NOCLOSE) : NULL;
   X509 *server_cert = server_bio ? PEM_read_bio_X509(server_bio, NULL, NULL, NULL) : NULL;
   BIO *client_bio = client_cert_pem ? BIO_new_mem_buf(client_cert_pem, -1) : NULL;
   X509 *client_cert = client_bio ? PEM_read_bio_X509(client_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *server_key = server_cert ? X509_get_pubkey(server_cert) : NULL;
   EVP_PKEY *client_key = client_cert ? X509_get_pubkey(client_cert) : NULL;
   /* EVP_PKEY_eq() returns -1 when the key algorithms differ (for example the
    * appliance's P-256 HTTPS key versus the managed KB identity's RSA key).
    * Different algorithms prove that the keys are distinct; treating -1 as a
    * validation failure made every such managed install silently ignore its
    * valid mTLS identity and fall back to the bootstrap HTTP endpoint. Keep the
    * same-algorithm case fail-closed when OpenSSL cannot compare the keys. */
   int server_type = server_key ? EVP_PKEY_get_base_id(server_key) : EVP_PKEY_NONE;
   int client_type = client_key ? EVP_PKEY_get_base_id(client_key) : EVP_PKEY_NONE;
   int distinct = server_key && client_key && server_type > 0 && client_type > 0 &&
                  (server_type != client_type || EVP_PKEY_eq(server_key, client_key) == 0);
   EVP_PKEY_free(server_key);
   EVP_PKEY_free(client_key);
   X509_free(server_cert);
   X509_free(client_cert);
   BIO_free(server_bio);
   BIO_free(client_bio);
   close(fd);
   return distinct;
}

static int identity_material_valid(const char *ca, const char *cert, const char *key)
{
   if (!ca || !cert || !key || !ca[0] || !cert[0] || !key[0] ||
       kb_pki_verify_client_cert(ca, cert) != 1 || !identity_distinct_from_server(cert))
      return 0;
   SSL_CTX *ctx = kb_tls_client_ctx(ca, cert, key);
   if (!ctx)
      return 0;
   SSL_CTX_free(ctx);
   return kb_tls_cert_expires_within(cert, 0) == 0;
}

static int identity_matches_connection(const kb_enroll_conn_t *connection, const char *ca)
{
   char fingerprint[KB_PKI_FP_HEX];
   return connection && ca && kb_pki_ca_fingerprint(ca, fingerprint, sizeof(fingerprint)) == 0 &&
          strcasecmp(fingerprint, connection->ca_sha256) == 0;
}

typedef struct
{
   int version;
   char host[256];
   int port;
   char server_id[128];
   long long team_id;
} identity_metadata_t;

static identity_metadata_t g_identity_metadata;
static kb_client_mtls_renew_fn g_renew_for_test;

static cJSON *identity_document_load(const char *path)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
       st.st_nlink != 1 || (st.st_mode & (S_IRWXG | S_IRWXO)) || st.st_size < 1 ||
       st.st_size >= KB_CLIENT_IDENTITY_MAX)
   {
      if (fd >= 0)
         close(fd);
      return NULL;
   }
   char *raw = calloc(1, (size_t)st.st_size + 1);
   size_t used = 0;
   while (raw && used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, raw + used, (size_t)st.st_size - used);
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   char extra = 0;
   ssize_t trailing = raw ? read(fd, &extra, 1) : -1;
   close(fd);
   if (!raw || used != (size_t)st.st_size || trailing != 0 || memchr(raw, '\0', used))
   {
      if (raw)
      {
         OPENSSL_cleanse(raw, (size_t)st.st_size + 1);
         free(raw);
      }
      return NULL;
   }
   const char *parse_end = NULL;
   cJSON *j = cJSON_ParseWithLengthOpts(raw, used + 1, &parse_end, 1);
   int parsed_all = parse_end == raw + used;
   OPENSSL_cleanse(raw, (size_t)st.st_size + 1);
   free(raw);
   if (!parsed_all)
   {
      cJSON_Delete(j);
      return NULL;
   }
   return j;
}

static int identity_load(const kb_enroll_conn_t *connection, char *ca, size_t ca_cap, char *cert,
                         size_t cert_cap, char *key, size_t key_cap, identity_metadata_t *metadata)
{
   if (metadata)
      memset(metadata, 0, sizeof(*metadata));
   char path[1024];
   if (identity_path(path, sizeof(path)) != 0)
      return -1;
   cJSON *j = identity_document_load(path);
   if (!j)
      return -1;
   cJSON *version = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
   cJSON *jca = j ? cJSON_GetObjectItemCaseSensitive(j, "ca") : NULL;
   cJSON *jcert = j ? cJSON_GetObjectItemCaseSensitive(j, "cert") : NULL;
   cJSON *jkey = j ? cJSON_GetObjectItemCaseSensitive(j, "key") : NULL;
   cJSON *state = j ? cJSON_GetObjectItemCaseSensitive(j, "state") : NULL;
   cJSON *host = j ? cJSON_GetObjectItemCaseSensitive(j, "host") : NULL;
   cJSON *port = j ? cJSON_GetObjectItemCaseSensitive(j, "port") : NULL;
   cJSON *server_id = j ? cJSON_GetObjectItemCaseSensitive(j, "server_id") : NULL;
   cJSON *team_id = j ? cJSON_GetObjectItemCaseSensitive(j, "team_id") : NULL;
   int is_v1 = cJSON_IsNumber(version) && version->valuedouble == 1;
   int is_v2 = cJSON_IsNumber(version) && version->valuedouble == 2 && cJSON_IsString(state) &&
               strcmp(state->valuestring, "ready") == 0 && cJSON_IsString(host) &&
               host->valuestring[0] &&
               strlen(host->valuestring) < sizeof(((identity_metadata_t *)0)->host) &&
               cJSON_IsNumber(port) && port->valuedouble >= 1 && port->valuedouble <= 65535 &&
               floor(port->valuedouble) == port->valuedouble && cJSON_IsString(server_id) &&
               server_id->valuestring[0] &&
               strlen(server_id->valuestring) < sizeof(((identity_metadata_t *)0)->server_id) &&
               cJSON_IsNumber(team_id) && team_id->valuedouble >= 1 &&
               team_id->valuedouble <= 9007199254740991.0 &&
               floor(team_id->valuedouble) == team_id->valuedouble;
   int endpoint_ok = is_v1 ? connection != NULL
                           : (!connection || (strcmp(connection->host, host->valuestring) == 0 &&
                                              connection->port == (int)port->valuedouble));
   int ok = (is_v1 || is_v2) && endpoint_ok && cJSON_IsString(jca) && cJSON_IsString(jcert) &&
            cJSON_IsString(jkey) && strlen(jca->valuestring) < ca_cap &&
            strlen(jcert->valuestring) < cert_cap && strlen(jkey->valuestring) < key_cap &&
            (!connection || identity_matches_connection(connection, jca->valuestring)) &&
            identity_material_valid(jca->valuestring, jcert->valuestring, jkey->valuestring);
   if (ok)
   {
      snprintf(ca, ca_cap, "%s", jca->valuestring);
      snprintf(cert, cert_cap, "%s", jcert->valuestring);
      snprintf(key, key_cap, "%s", jkey->valuestring);
      if (metadata)
      {
         metadata->version = is_v2 ? 2 : 1;
         if (is_v2)
         {
            snprintf(metadata->host, sizeof(metadata->host), "%s", host->valuestring);
            metadata->port = (int)port->valuedouble;
            snprintf(metadata->server_id, sizeof(metadata->server_id), "%s",
                     server_id->valuestring);
            metadata->team_id = (long long)team_id->valuedouble;
         }
      }
   }
   if (cJSON_IsString(jkey))
      OPENSSL_cleanse(jkey->valuestring, strlen(jkey->valuestring));
   cJSON_Delete(j);
   return ok ? 0 : -1;
}

static int json_replace_string(cJSON *object, const char *name, const char *value);

static int identity_save(const char *ca, const char *cert, const char *key,
                         const identity_metadata_t *metadata)
{
   char path[1024], temporary[1080];
   if (identity_path(path, sizeof(path)) != 0)
      return -1;
   int tn = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
   if (tn <= 0 || (size_t)tn >= sizeof(temporary))
      return -1;
   int is_v2 = metadata && metadata->version == 2;
   cJSON *j = is_v2 ? identity_document_load(path) : cJSON_CreateObject();
   cJSON *version = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
   cJSON *state = j ? cJSON_GetObjectItemCaseSensitive(j, "state") : NULL;
   cJSON *host = j ? cJSON_GetObjectItemCaseSensitive(j, "host") : NULL;
   cJSON *port = j ? cJSON_GetObjectItemCaseSensitive(j, "port") : NULL;
   cJSON *server_id = j ? cJSON_GetObjectItemCaseSensitive(j, "server_id") : NULL;
   cJSON *team_id = j ? cJSON_GetObjectItemCaseSensitive(j, "team_id") : NULL;
   int v2_matches =
       !is_v2 || (cJSON_IsNumber(version) && version->valuedouble == 2 && cJSON_IsString(state) &&
                  !strcmp(state->valuestring, "ready") && cJSON_IsString(host) &&
                  !strcmp(host->valuestring, metadata->host) && cJSON_IsNumber(port) &&
                  port->valuedouble == metadata->port && cJSON_IsString(server_id) &&
                  !strcmp(server_id->valuestring, metadata->server_id) && cJSON_IsNumber(team_id) &&
                  team_id->valuedouble == metadata->team_id);
   int updated =
       j && v2_matches && (is_v2 || cJSON_AddNumberToObject(j, "version", 1)) &&
       (is_v2 ? json_replace_string(j, "ca", ca) : cJSON_AddStringToObject(j, "ca", ca) != NULL) &&
       (is_v2 ? json_replace_string(j, "cert", cert)
              : cJSON_AddStringToObject(j, "cert", cert) != NULL) &&
       (is_v2 ? json_replace_string(j, "key", key)
              : cJSON_AddStringToObject(j, "key", key) != NULL);
   if (!updated)
   {
      cJSON_Delete(j);
      return -1;
   }
   char *raw = cJSON_PrintUnformatted(j);
   cJSON *jkey = cJSON_GetObjectItemCaseSensitive(j, "key");
   if (cJSON_IsString(jkey))
      OPENSSL_cleanse(jkey->valuestring, strlen(jkey->valuestring));
   cJSON_Delete(j);
   if (!raw)
      return -1;
   size_t length = strlen(raw);
   (void)unlink(temporary);
   int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   size_t used = 0;
   while (fd >= 0 && used < length)
   {
      ssize_t n = write(fd, raw + used, length - used);
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   int ok = fd >= 0 && used == length && fsync(fd) == 0 && fchmod(fd, 0600) == 0;
   if (fd >= 0)
   {
      if (close(fd) != 0)
         ok = 0;
      fd = -1;
   }
   OPENSSL_cleanse(raw, length);
   free(raw);
   if (!ok || rename(temporary, path) != 0)
   {
      (void)unlink(temporary);
      return -1;
   }
   return 0;
}

static int json_replace_string(cJSON *object, const char *name, const char *value)
{
   cJSON *replacement = cJSON_CreateString(value);
   if (!replacement)
      return 0;
   if (!cJSON_ReplaceItemInObjectCaseSensitive(object, name, replacement))
   {
      cJSON_Delete(replacement);
      return 0;
   }
   return 1;
}

#define KB_POOL_TOTAL_MAX   8
#define KB_POOL_IDLE_MAX    2
#define KB_POOL_WAITERS_MAX 64
#define KB_POOL_IDLE_SECS   30
#define KB_POOL_AGE_SECS    (10 * 60)
#define KB_POOL_REQ_MAX     1000

typedef struct
{
   kb_tls_client_conn_t *conn;
   unsigned generation;
   unsigned requests;
   time_t created_at;
   time_t last_used_at;
   int busy;
} kb_pool_entry_t;

static kb_pool_entry_t g_pool[KB_POOL_TOTAL_MAX];
static pthread_cond_t g_pool_cv = PTHREAD_COND_INITIALIZER;
static unsigned g_identity_generation = 1;
static int g_pool_waiters = 0;
static int g_pool_connecting = 0;
static unsigned long g_pool_borrow_exhausted_total = 0;
static SSL_CTX *g_pool_ctx = NULL;
static SSL_SESSION *g_pool_session = NULL;
static unsigned long g_pool_handshakes_total = 0;
static unsigned long g_pool_resumed_total = 0;
static int g_pool_enabled_last = -1;
static long g_renew_window_for_test = -1;

static void pool_close_entry_locked(kb_pool_entry_t *entry);

void kb_client_mtls_reset_for_test(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      if (g_pool[i].conn)
         pool_close_entry_locked(&g_pool[i]);
   SSL_CTX_free(g_pool_ctx);
   g_pool_ctx = NULL;
   SSL_SESSION_free(g_pool_session);
   g_pool_session = NULL;
   g_identity_generation++;
   g_enrolled = 0;
   g_host[0] = '\0';
   g_port = 0;
   g_ca[0] = '\0';
   g_cert[0] = '\0';
   memset(&g_identity_metadata, 0, sizeof(g_identity_metadata));
   g_renew_for_test = NULL;
   g_renew_window_for_test = -1;
   OPENSSL_cleanse(g_key, sizeof(g_key));
   pthread_cond_broadcast(&g_pool_cv);
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_set_identity_path_for_test(const char *absolute_path)
{
   pthread_mutex_lock(&g_lock);
   snprintf(g_identity_path_override, sizeof(g_identity_path_override), "%s",
            absolute_path ? absolute_path : "");
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_set_server_identity_path_for_test(const char *absolute_path)
{
   pthread_mutex_lock(&g_lock);
   snprintf(g_server_identity_path_override, sizeof(g_server_identity_path_override), "%s",
            absolute_path ? absolute_path : "");
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_set_renew_window_for_test(long seconds)
{
   pthread_mutex_lock(&g_lock);
   g_renew_window_for_test = seconds;
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_set_renew_for_test(kb_client_mtls_renew_fn renew)
{
   pthread_mutex_lock(&g_lock);
   g_renew_for_test = renew;
   pthread_mutex_unlock(&g_lock);
}

static int pool_feature_enabled(void)
{
   const char *override = getenv("AIMEE_TRANSPORT_KB_POOL_ENABLED");
   if (override)
      return strcmp(override, "1") == 0 || strcasecmp(override, "true") == 0;
   pthread_mutex_lock(&g_lock);
   int enabled = g_pool_enabled_last == 1;
   pthread_mutex_unlock(&g_lock);
   return enabled;
}

/* Apply a hot flag transition exactly once. Disabling closes idle entries now;
 * borrowed entries carry the old generation and drain when returned. */
static void pool_sync_enabled(int enabled)
{
   pthread_mutex_lock(&g_lock);
   if (g_pool_enabled_last != enabled)
   {
      g_pool_enabled_last = enabled;
      if (!enabled)
      {
         g_identity_generation++;
         SSL_CTX_free(g_pool_ctx);
         g_pool_ctx = NULL;
         SSL_SESSION_free(g_pool_session);
         g_pool_session = NULL;
         for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
            if (g_pool[i].conn && !g_pool[i].busy)
               pool_close_entry_locked(&g_pool[i]);
         pthread_cond_broadcast(&g_pool_cv);
      }
   }
   pthread_mutex_unlock(&g_lock);
}

static void pool_config_reapplier(void)
{
   pool_sync_enabled(config_transport_kb_pool_enabled());
}

void kb_client_mtls_pool_register_reload(void)
{
   pool_sync_enabled(config_transport_kb_pool_enabled());
   config_reload_register_reapplier(pool_config_reapplier);
}

static time_t monotonic_seconds(void)
{
   struct timespec ts;
   return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? ts.tv_sec : 0;
}

static void pool_close_entry_locked(kb_pool_entry_t *entry)
{
   kb_tls_client_conn_t *conn = entry->conn;
   memset(entry, 0, sizeof(*entry));
   /* SSL shutdown is bounded by the socket deadline. Keeping this under the
    * lock makes entry ownership simple; the pool contains at most eight. */
   kb_tls_client_conn_close(conn);
}

static void pool_expire_idle_locked(time_t now)
{
   for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      if (g_pool[i].conn && !g_pool[i].busy &&
          (g_pool[i].generation != g_identity_generation ||
           now - g_pool[i].last_used_at >= KB_POOL_IDLE_SECS ||
           now - g_pool[i].created_at >= KB_POOL_AGE_SECS || g_pool[i].requests >= KB_POOL_REQ_MAX))
         pool_close_entry_locked(&g_pool[i]);
}

static kb_pool_entry_t *pool_borrow(int *pool_error)
{
   if (pool_error)
      *pool_error = -1;
   pthread_mutex_lock(&g_lock);
   for (;;)
   {
      time_t now = monotonic_seconds();
      pool_expire_idle_locked(now);
      int total = 0;
      for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      {
         if (g_pool[i].conn)
         {
            total++;
            if (!g_pool[i].busy && g_pool[i].generation == g_identity_generation)
            {
               g_pool[i].busy = 1;
               pthread_mutex_unlock(&g_lock);
               return &g_pool[i];
            }
         }
      }

      if (total + g_pool_connecting < KB_POOL_TOTAL_MAX && !g_pool_connecting)
      {
         char host[sizeof(g_host)], ca[sizeof(g_ca)], cert[sizeof(g_cert)], key[sizeof(g_key)];
         int port = g_port;
         unsigned generation = g_identity_generation;
         snprintf(host, sizeof(host), "%s", g_host);
         snprintf(ca, sizeof(ca), "%s", g_ca);
         snprintf(cert, sizeof(cert), "%s", g_cert);
         snprintf(key, sizeof(key), "%s", g_key);
         if (!g_pool_ctx)
            g_pool_ctx = kb_tls_client_ctx(ca, cert, key);
         SSL_CTX *ctx = g_pool_ctx;
         if (!ctx || SSL_CTX_up_ref(ctx) != 1)
         {
            pthread_mutex_unlock(&g_lock);
            return NULL;
         }
         SSL_SESSION *session = g_pool_session;
         if (session && SSL_SESSION_up_ref(session) != 1)
            session = NULL;
         g_pool_connecting = 1;
         pthread_mutex_unlock(&g_lock);
         kb_tls_client_conn_t *conn = kb_tls_client_conn_open_session(host, port, ctx, session);
         SSL_SESSION_free(session);
         SSL_CTX_free(ctx);
         pthread_mutex_lock(&g_lock);
         g_pool_connecting = 0;
         g_pool_handshakes_total++;
         if (kb_tls_client_conn_session_reused(conn))
            g_pool_resumed_total++;
         pthread_cond_broadcast(&g_pool_cv);
         if (!conn || generation != g_identity_generation)
         {
            kb_tls_client_conn_close(conn);
            pthread_mutex_unlock(&g_lock);
            return NULL;
         }
         for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
            if (!g_pool[i].conn)
            {
               g_pool[i].conn = conn;
               g_pool[i].generation = generation;
               g_pool[i].created_at = monotonic_seconds();
               g_pool[i].last_used_at = g_pool[i].created_at;
               g_pool[i].busy = 1;
               pthread_mutex_unlock(&g_lock);
               return &g_pool[i];
            }
         kb_tls_client_conn_close(conn);
         pthread_mutex_unlock(&g_lock);
         return NULL;
      }

      if (g_pool_waiters >= KB_POOL_WAITERS_MAX)
      {
         g_pool_borrow_exhausted_total++;
         if (pool_error)
            *pool_error = KB_CLIENT_ERR_POOL_EXHAUSTED;
         pthread_mutex_unlock(&g_lock);
         return NULL;
      }
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 30;
      g_pool_waiters++;
      int wait_rc = pthread_cond_timedwait(&g_pool_cv, &g_lock, &deadline);
      g_pool_waiters--;
      if (wait_rc == ETIMEDOUT)
      {
         g_pool_borrow_exhausted_total++;
         if (pool_error)
            *pool_error = KB_CLIENT_ERR_POOL_EXHAUSTED;
         pthread_mutex_unlock(&g_lock);
         return NULL;
      }
   }
}

static void pool_return(kb_pool_entry_t *entry, int reusable)
{
   pthread_mutex_lock(&g_lock);
   entry->requests++;
   entry->last_used_at = monotonic_seconds();
   if (reusable)
   {
      SSL_SESSION *session = kb_tls_client_conn_get1_session(entry->conn);
      if (session)
      {
         SSL_SESSION_free(g_pool_session);
         g_pool_session = session;
      }
   }
   int idle = 0;
   for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      if (g_pool[i].conn && !g_pool[i].busy)
         idle++;
   if (!reusable || entry->generation != g_identity_generation ||
       entry->requests >= KB_POOL_REQ_MAX ||
       entry->last_used_at - entry->created_at >= KB_POOL_AGE_SECS || idle >= KB_POOL_IDLE_MAX)
      pool_close_entry_locked(entry);
   else
      entry->busy = 0;
   pthread_cond_signal(&g_pool_cv);
   pthread_mutex_unlock(&g_lock);
}

int kb_client_mtls_configured(void)
{
   char connection[4096];
   int have_connection = runtime_secret_get("AIMEE_KB_CONN", connection, sizeof(connection));
   OPENSSL_cleanse(connection, sizeof(connection));
   if (have_connection)
      return 1;
   char ca[sizeof(g_ca)], cert[sizeof(g_cert)], key[sizeof(g_key)];
   identity_metadata_t metadata;
   int configured =
       identity_load(NULL, ca, sizeof(ca), cert, sizeof(cert), key, sizeof(key), &metadata) == 0 &&
       metadata.version == 2;
   OPENSSL_cleanse(key, sizeof(key));
   return configured;
}

int kb_client_mtls_managed_metadata(char *server_id_out, size_t server_id_cap,
                                    long long *team_id_out)
{
   if (server_id_out && server_id_cap)
      server_id_out[0] = '\0';
   if (team_id_out)
      *team_id_out = 0;
   char ca[sizeof(g_ca)], cert[sizeof(g_cert)], key[sizeof(g_key)];
   identity_metadata_t metadata;
   int ok =
       identity_load(NULL, ca, sizeof(ca), cert, sizeof(cert), key, sizeof(key), &metadata) == 0 &&
       metadata.version == 2 && server_id_out && server_id_cap > strlen(metadata.server_id) &&
       team_id_out;
   OPENSSL_cleanse(key, sizeof(key));
   if (!ok)
      return 0;
   snprintf(server_id_out, server_id_cap, "%s", metadata.server_id);
   *team_id_out = metadata.team_id;
   return 1;
}

/* Load the durable identity or enroll once and persist it before use. The
 * connection string remains configured after its token is consumed, so a
 * restart can recover the certificate while still taking host/port + CA pin
 * from the operator-supplied value. */
static int ensure_enrolled(void)
{
   int rc = -1;
   pthread_mutex_lock(&g_lock);
   if (g_enrolled)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   char connection[4096];
   int have_connection = runtime_secret_get("AIMEE_KB_CONN", connection, sizeof(connection));
   const char *conn = have_connection ? connection : NULL;
   kb_enroll_conn_t pc;
   if (conn && conn[0] && kb_enroll_conn_string_parse(conn, &pc) == 0)
   {
      identity_metadata_t metadata;
      int loaded = identity_load(&pc, g_ca, sizeof(g_ca), g_cert, sizeof(g_cert), g_key,
                                 sizeof(g_key), &metadata) == 0;
      if (loaded || (kb_tls_enroll(conn, g_ca, sizeof(g_ca), g_cert, sizeof(g_cert), g_key,
                                   sizeof(g_key)) == 0 &&
                     identity_save(g_ca, g_cert, g_key, NULL) == 0))
      {
         snprintf(g_host, sizeof(g_host), "%s", pc.host);
         g_port = pc.port;
         if (loaded)
            g_identity_metadata = metadata;
         else
            g_identity_metadata.version = 1;
         g_enrolled = 1;
         rc = 0;
      }
   }
   else if (!conn || !conn[0])
   {
      identity_metadata_t metadata;
      if (identity_load(NULL, g_ca, sizeof(g_ca), g_cert, sizeof(g_cert), g_key, sizeof(g_key),
                        &metadata) == 0 &&
          metadata.version == 2)
      {
         snprintf(g_host, sizeof(g_host), "%s", metadata.host);
         g_port = metadata.port;
         g_identity_metadata = metadata;
         g_enrolled = 1;
         rc = 0;
      }
   }
   if (rc != 0)
   {
      OPENSSL_cleanse(g_key, sizeof(g_key));
      g_ca[0] = '\0';
      g_cert[0] = '\0';
      memset(&g_identity_metadata, 0, sizeof(g_identity_metadata));
   }
   OPENSSL_cleanse(connection, sizeof(connection));
   pthread_mutex_unlock(&g_lock);
   return rc;
}

/* Renew the client cert before it expires (zero operator action). Holds g_lock
 * while it rotates the identity in place. */
#define KB_CLIENT_MTLS_RENEW_WINDOW (60L * 60 * 24 * 14) /* < 14 days left */

static void maybe_renew(const char *authorization)
{
   pthread_mutex_lock(&g_lock);
   long renew_window =
       g_renew_window_for_test >= 0 ? g_renew_window_for_test : KB_CLIENT_MTLS_RENEW_WINDOW;
   if (g_enrolled && kb_tls_cert_expires_within(g_cert, renew_window) == 1)
   {
      char nc[sizeof(g_cert)], nk[sizeof(g_key)];
      kb_client_mtls_renew_fn renew = g_renew_for_test ? g_renew_for_test : kb_tls_renew;
      if (renew(g_host, g_port, g_ca, g_cert, g_key, authorization, nc, sizeof(nc), nk,
                sizeof(nk)) == 0)
      {
         /* Never switch the live process to an identity a restart would lose.
          * The old cert remains usable through its existing validity window if
          * storage is temporarily unavailable. */
         if (identity_save(g_ca, nc, nk, &g_identity_metadata) == 0)
         {
            snprintf(g_cert, sizeof(g_cert), "%s", nc);
            snprintf(g_key, sizeof(g_key), "%s", nk);
            g_identity_generation++;
            SSL_CTX_free(g_pool_ctx);
            g_pool_ctx = NULL;
            SSL_SESSION_free(g_pool_session);
            g_pool_session = NULL;
            pthread_cond_broadcast(&g_pool_cv);
         }
      }
      OPENSSL_cleanse(nk, sizeof(nk));
   }
   pthread_mutex_unlock(&g_lock);
}

#define KB_CLIENT_MTLS_DEFAULT_TIMEOUT_MS 30000

char *kb_client_mtls_request_timeout_with_type(const char *method, const char *path,
                                               const char *body, const char *content_type,
                                               int timeout_ms, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!method || !path || ensure_enrolled() != 0)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = KB_CLIENT_MTLS_DEFAULT_TIMEOUT_MS;
   char authorization[KB_CLIENT_HEADERS_MAX];
   if (service_request_headers(authorization, sizeof(authorization)) != 0)
   {
      if (status_out)
         *status_out = KB_CLIENT_ERR_AUTH_REQUIRED;
      return NULL;
   }
   maybe_renew(authorization);

   size_t cap = 1u << 20; /* 1 MiB — covers kb /v1 responses (status/search/etc.) */
   char *resp = malloc(cap);
   int pool_enabled = pool_feature_enabled();
   pool_sync_enabled(pool_enabled);
   if (!pool_enabled)
   {
      char host[sizeof(g_host)], ca[sizeof(g_ca)], cert[sizeof(g_cert)], key[sizeof(g_key)];
      int port;
      pthread_mutex_lock(&g_lock);
      snprintf(host, sizeof(host), "%s", g_host);
      snprintf(ca, sizeof(ca), "%s", g_ca);
      snprintf(cert, sizeof(cert), "%s", g_cert);
      snprintf(key, sizeof(key), "%s", g_key);
      port = g_port;
      pthread_mutex_unlock(&g_lock);
      int status = -1;
      int reusable = 0;
      kb_tls_client_conn_t *conn = resp ? kb_tls_client_conn_open(host, port, ca, cert, key) : NULL;
      int rc = conn && kb_tls_client_conn_set_timeout(conn, timeout_ms) == 0
                   ? kb_tls_client_conn_request_with_type(
                         conn, method, path, (body && body[0]) ? body : NULL, authorization,
                         content_type, 1, resp, cap, &status, &reusable)
                   : -1;
      kb_tls_client_conn_close(conn);
      if (status_out)
         *status_out = status;
      /* THE BODY IS RETURNED ON A NON-2xx TOO, and *status_out is how the caller
       * tells them apart -- every caller already checks it first and frees on
       * failure. Dropping it here made kb_client_v1_post_json_keep_error's whole
       * purpose unreachable on this transport: the kb writes careful refusals, and
       * on a managed appliance (server -> kb over mTLS on 8745) the operator got a
       * generic fallback instead of any of them.
       *
       * `aimee kb reembed` is the case that exposed it. The kb answers 403 with
       * "kb.reembed_on_dim_change is disabled; set 'kb: reembed_on_dim_change: true'
       * in aimee-kb's own $AIMEE_HOME/aimee.yaml and restart it (this gate is read by
       * aimee-kb, not by aimee-server, so `aimee config set` does not reach it)" --
       * which names the file, the key and the trap. The operator saw "knowledge
       * service reembed failed". */
      char *out = (rc == 0) ? strdup(resp) : NULL;
      runtime_secret_wipe(authorization, sizeof(authorization));
      free(resp);
      return out;
   }
   int pool_error = -1;
   kb_pool_entry_t *entry = resp ? pool_borrow(&pool_error) : NULL;
   if (!entry)
   {
      runtime_secret_wipe(authorization, sizeof(authorization));
      free(resp);
      if (status_out)
         *status_out = pool_error;
      return NULL;
   }
   int status = -1;
   int reusable = 0;
   int rc = kb_tls_client_conn_set_timeout(entry->conn, timeout_ms) == 0
                ? kb_tls_client_conn_request_with_type(
                      entry->conn, method, path, (body && body[0]) ? body : NULL, authorization,
                      content_type, 0, resp, cap, &status, &reusable)
                : -1;
   pool_return(entry, rc == 0 && reusable);
   if (status_out)
      *status_out = status;
   /* Body preserved on non-2xx as above; *status_out distinguishes. */
   char *out = (rc == 0) ? strdup(resp) : NULL;
   runtime_secret_wipe(authorization, sizeof(authorization));
   free(resp);
   return out;
}

char *kb_client_mtls_request_timeout(const char *method, const char *path, const char *body,
                                     int timeout_ms, int *status_out)
{
   return kb_client_mtls_request_timeout_with_type(method, path, body, NULL, timeout_ms,
                                                   status_out);
}

char *kb_client_mtls_request(const char *method, const char *path, const char *body,
                             int *status_out)
{
   return kb_client_mtls_request_timeout(method, path, body, KB_CLIENT_MTLS_DEFAULT_TIMEOUT_MS,
                                         status_out);
}

int kb_client_mtls_management_jwks(char *envelope_out, size_t envelope_cap, size_t *envelope_len)
{
   if (envelope_out && envelope_cap)
      memset(envelope_out, 0, envelope_cap);
   if (envelope_len)
      *envelope_len = 0;
   if (!envelope_out || envelope_cap < 2 || !envelope_len)
      return -1;
   int status = -1;
   char *response = kb_client_mtls_request("GET", "/v1/management/jwks", NULL, &status);
   if (!response || status != 200)
   {
      free(response);
      return -1;
   }
   size_t n = strnlen(response, 3072);
   if (!n || n >= 3072 || n + 1 > envelope_cap)
   {
      free(response);
      return -1;
   }
   memcpy(envelope_out, response, n + 1);
   *envelope_len = n;
   free(response);
   return 0;
}

int kb_client_mtls_management_jwks_fetch(void *ctx, char *envelope_out, size_t envelope_cap,
                                         size_t *envelope_len)
{
   (void)ctx;
   return kb_client_mtls_management_jwks(envelope_out, envelope_cap, envelope_len);
}

void kb_client_mtls_pool_stats(int *total_out, int *idle_out, int *busy_out, int *waiters_out,
                               unsigned long *borrow_exhausted_total_out)
{
   int total = 0, idle = 0, busy = 0;
   pthread_mutex_lock(&g_lock);
   pool_expire_idle_locked(monotonic_seconds());
   for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      if (g_pool[i].conn)
      {
         total++;
         if (g_pool[i].busy)
            busy++;
         else
            idle++;
      }
   if (total_out)
      *total_out = total;
   if (idle_out)
      *idle_out = idle;
   if (busy_out)
      *busy_out = busy;
   if (waiters_out)
      *waiters_out = g_pool_waiters;
   if (borrow_exhausted_total_out)
      *borrow_exhausted_total_out = g_pool_borrow_exhausted_total;
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_pool_reset(void)
{
   pthread_mutex_lock(&g_lock);
   g_identity_generation++;
   SSL_CTX_free(g_pool_ctx);
   g_pool_ctx = NULL;
   SSL_SESSION_free(g_pool_session);
   g_pool_session = NULL;
   for (int i = 0; i < KB_POOL_TOTAL_MAX; i++)
      if (g_pool[i].conn && !g_pool[i].busy)
         pool_close_entry_locked(&g_pool[i]);
   pthread_cond_broadcast(&g_pool_cv);
   pthread_mutex_unlock(&g_lock);
}

void kb_client_mtls_tls_stats(unsigned long *handshakes_total_out, unsigned long *resumed_total_out)
{
   pthread_mutex_lock(&g_lock);
   if (handshakes_total_out)
      *handshakes_total_out = g_pool_handshakes_total;
   if (resumed_total_out)
      *resumed_total_out = g_pool_resumed_total;
   pthread_mutex_unlock(&g_lock);
}

int kb_client_mtls_heartbeat(const char *server_id, const char *health, const char *version)
{
   if (!server_id || !server_id[0] || strlen(server_id) > 127 || !health || strlen(health) > 127 ||
       !version || strlen(version) > 63)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "server_id", server_id);
   cJSON_AddStringToObject(root, "health", health);
   cJSON_AddStringToObject(root, "version", version);
   char *body = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!body)
      return -1;
   int status = -1;
   char *response = kb_client_mtls_request("POST", "/v1/server/heartbeat", body, &status);
   int ok = status == 200 && response != NULL;
   free(body);
   free(response);
   return ok ? 0 : -1;
}
