/* kb_client_mtls.c: aimee-server's distributed-mode mTLS transport to a remote
 * aimee-kb. (distributed-mode-auth proposal, client integration.)
 *
 * When AIMEE_KB_CONN holds an `aimee://` connection string, the server enrolls
 * once (kb_tls_enroll: TOFU-pin the CA by fingerprint, generate a keypair + CSR,
 * redeem the token for a client cert) and then routes its /v1 kb calls over
 * mutual TLS with that identity. Selected at the top of the kb_client v1
 * transport (kb_client.c) ahead of the HTTP-URL and Unix-socket transports. */
#include "kb_client_mtls.h"
#include "kb_enroll.h" /* connection-string parse (for host/port) */
#include "kb_tls.h"    /* kb_tls_enroll / kb_tls_client_request */
#include "cJSON.h"

#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The enrolled identity, established once on first use. Immutable after init
 * (guarded by g_lock during enrollment), so request-time reads need no lock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_enrolled = 0;
static char g_host[256];
static int g_port = 0;
static char g_ca[8192];
static char g_cert[8192];
static char g_key[8192];

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
   const char *c = getenv("AIMEE_KB_CONN");
   return (c && c[0]) ? 1 : 0;
}

/* Enroll once: parse AIMEE_KB_CONN, then kb_tls_enroll into the static identity.
 * Returns 0 if enrolled (already or now), -1 on failure. */
static int ensure_enrolled(void)
{
   int rc = -1;
   pthread_mutex_lock(&g_lock);
   if (g_enrolled)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   const char *conn = getenv("AIMEE_KB_CONN");
   kb_enroll_conn_t pc;
   if (conn && conn[0] && kb_enroll_conn_string_parse(conn, &pc) == 0 &&
       kb_tls_enroll(conn, g_ca, sizeof(g_ca), g_cert, sizeof(g_cert), g_key, sizeof(g_key)) == 0)
   {
      snprintf(g_host, sizeof(g_host), "%s", pc.host);
      g_port = pc.port;
      g_enrolled = 1;
      rc = 0;
   }
   pthread_mutex_unlock(&g_lock);
   return rc;
}

/* Renew the client cert before it expires (zero operator action). Holds g_lock
 * while it rotates the identity in place. */
#define KB_CLIENT_MTLS_RENEW_WINDOW (60L * 60 * 24 * 14) /* < 14 days left */

static void maybe_renew(void)
{
   pthread_mutex_lock(&g_lock);
   if (g_enrolled && kb_tls_cert_expires_within(g_cert, KB_CLIENT_MTLS_RENEW_WINDOW) == 1)
   {
      char nc[sizeof(g_cert)], nk[sizeof(g_key)];
      if (kb_tls_renew(g_host, g_port, g_ca, g_cert, g_key, nc, sizeof(nc), nk, sizeof(nk)) == 0)
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
   pthread_mutex_unlock(&g_lock);
}

char *kb_client_mtls_request(const char *method, const char *path, const char *body,
                             int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!method || !path || ensure_enrolled() != 0)
      return NULL;
   maybe_renew();

   size_t cap = 1u << 20; /* 1 MiB — covers kb /v1 responses (status/search/etc.) */
   char *resp = malloc(cap);
   int pool_error = -1;
   kb_pool_entry_t *entry = resp ? pool_borrow(&pool_error) : NULL;
   if (!entry)
   {
      free(resp);
      if (status_out)
         *status_out = pool_error;
      return NULL;
   }
   int status = -1;
   int reusable = 0;
   int rc = kb_tls_client_conn_request(entry->conn, method, path, (body && body[0]) ? body : NULL,
                                       NULL, 0, resp, cap, &status, &reusable);
   pool_return(entry, rc == 0 && reusable);
   if (status_out)
      *status_out = status;
   char *out = (rc == 0 && status >= 200 && status < 300) ? strdup(resp) : NULL;
   free(resp);
   return out;
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
