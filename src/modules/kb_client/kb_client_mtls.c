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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The enrolled identity, established once on first use. Immutable after init
 * (guarded by g_lock during enrollment), so request-time reads need no lock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_enrolled = 0;
static char g_host[256];
static int g_port = 0;
static char g_ca[8192];
static char g_cert[8192];
static char g_key[8192];

int kb_client_mtls_configured(void)
{
   const char *c = getenv("AIMEE_KB_CONN");
   return (c && c[0]) ? 1 : 0;
}

/* Enroll once: parse AIMEE_KB_CONN, then kb_tls_enroll into the static identity.
 * Returns 0 if enrolled (already or now), -1 on failure. */
static int ensure_enrolled(void)
{
   if (g_enrolled)
      return 0;
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
   if (kb_tls_cert_expires_within(g_cert, KB_CLIENT_MTLS_RENEW_WINDOW) != 1)
      return;
   pthread_mutex_lock(&g_lock);
   if (g_enrolled && kb_tls_cert_expires_within(g_cert, KB_CLIENT_MTLS_RENEW_WINDOW) == 1)
   {
      char nc[sizeof(g_cert)], nk[sizeof(g_key)];
      if (kb_tls_renew(g_host, g_port, g_ca, g_cert, g_key, nc, sizeof(nc), nk, sizeof(nk)) == 0)
      {
         snprintf(g_cert, sizeof(g_cert), "%s", nc);
         snprintf(g_key, sizeof(g_key), "%s", nk);
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

   /* Snapshot the identity under the lock so a concurrent renew cannot mutate it
    * mid-request. */
   char host[sizeof(g_host)];
   char *ca, *cert, *key;
   int port;
   pthread_mutex_lock(&g_lock);
   snprintf(host, sizeof(host), "%s", g_host);
   port = g_port;
   ca = strdup(g_ca);
   cert = strdup(g_cert);
   key = strdup(g_key);
   pthread_mutex_unlock(&g_lock);
   if (!ca || !cert || !key)
   {
      free(ca);
      free(cert);
      free(key);
      return NULL;
   }

   size_t cap = 1u << 20; /* 1 MiB — covers kb /v1 responses (status/search/etc.) */
   char *resp = malloc(cap);
   int status = -1;
   int rc = resp ? kb_tls_client_request(host, port, ca, cert, key, method, path,
                                         (body && body[0]) ? body : NULL, resp, cap, &status)
                 : -1;
   free(ca);
   free(cert);
   free(key);
   if (status_out)
      *status_out = status;
   char *out = (rc == 0 && status >= 200 && status < 300) ? strdup(resp) : NULL;
   free(resp);
   return out;
}
