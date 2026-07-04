/* kb_tls_serve.c: aimee-kb's mTLS request serving + listener (distributed mode).
 * (distributed-mode-auth proposal, mTLS phase.)
 *
 * Split out of kb_tls.c so the KB-only serving path (which routes through
 * kb_http_route_ex) does not pull the kb request router into the aimee-server
 * binary, which links only the client-side TLS primitives in kb_tls.c. */
/* _GNU_SOURCE: strcasestr is a GNU extension; declare it before any include
 * pulls in <string.h>. The Makefile build does not define it globally (only
 * the CMake build does), so older glibc/gcc targets (e.g. Debian 12) need it
 * here or strcasestr is an implicit-declaration error under -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_tls.h"

/* --- serve one mTLS connection (handshake + request + scoped routing) --- */

#include "cJSON.h"
#include "kb_enroll.h"       /* KB_ENROLL_SCOPE_MAX */
#include "kb_http.h"         /* kb_http_route_ex */
#include "db2/enrollments.h" /* revocation source of truth + last-seen */
#include "kb_paths.h"        /* kb_default_config_dir */
#include "kb_pki.h"          /* CA load + CSR signing for renew */

#include <stdlib.h>
#include <string.h>

#define KB_TLS_REQ_MAX  65536
#define KB_TLS_RESP_MAX 262144

static const char *http_reason(int status)
{
   switch (status)
   {
   case 200:
      return "OK";
   case 400:
      return "Bad Request";
   case 401:
      return "Unauthorized";
   case 403:
      return "Forbidden";
   case 404:
      return "Not Found";
   case 405:
      return "Method Not Allowed";
   case 500:
      return "Internal Server Error";
   default:
      return "OK";
   }
}

/* GET /v1/enroll/ca: return the CA certificate (public trust anchor) so a
 * bootstrapping client can pin it by fingerprint. The private key is never
 * exposed. Writes the JSON response; returns the HTTP status. */
static int mtls_get_ca(char *resp, int cap)
{
   char ca_dir[1024];
   snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   kb_pki_ca_t ca;
   if (kb_pki_ca_load(ca_dir, &ca) != 0)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"no CA\"}");
      return 500;
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "ca_cert", ca.cert_pem); /* cert only, never the key */
   OPENSSL_cleanse(&ca, sizeof(ca));
   char *s = cJSON_PrintUnformatted(out);
   snprintf(resp, (size_t)cap, "%s", s ? s : "{}");
   free(s);
   cJSON_Delete(out);
   return 200;
}

/* Handle POST /v1/enroll/renew for an authenticated mTLS client: sign the body's
 * CSR with the CA, binding it to `scope_cn` — the caller's CURRENT verified cert
 * scope (NOT anything in the request) — for a fresh validity period. This lets a
 * client rotate its cert before expiry with no token and no operator action. The
 * client keeps its (new) private key. Writes the JSON response into resp[cap];
 * returns the HTTP status. */
static int mtls_renew(const char *scope_cn, const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   const cJSON *jcsr = req ? cJSON_GetObjectItemCaseSensitive(req, "csr") : NULL;
   if (!cJSON_IsString(jcsr))
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap, "{\"error\":\"bad request: csr (PEM string) required\"}");
      return 400;
   }

   char ca_dir[1024];
   snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   kb_pki_ca_t ca;
   if (kb_pki_ca_load(ca_dir, &ca) != 0)
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew unavailable: no CA\"}");
      return 500;
   }

   char *cert = malloc(KB_PKI_CERT_PEM_MAX);
   int rc = cert ? kb_pki_sign_csr(&ca, jcsr->valuestring, scope_cn, 60L * 60 * 24 * 90, cert,
                                   KB_PKI_CERT_PEM_MAX)
                 : -1;
   OPENSSL_cleanse(&ca, sizeof(ca));
   cJSON_Delete(req);
   if (rc != 0)
   {
      free(cert);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew failed: bad CSR\"}");
      return 400;
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "client_cert", cert);
   cJSON_AddStringToObject(out, "scope", scope_cn);
   char *s = cJSON_PrintUnformatted(out);
   snprintf(resp, (size_t)cap, "%s", s ? s : "{}");
   free(s);
   cJSON_Delete(out);
   free(cert);
   return 200;
}

void kb_tls_serve_conn(int fd, SSL_CTX *ctx)
{
   if (!ctx)
      return;
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
      return;
   SSL_set_fd(ssl, fd);
   /* Handshake — REQUIRES + verifies the client cert (server ctx config). */
   if (SSL_accept(ssl) != 1)
   {
      SSL_free(ssl);
      return;
   }

   char *buf = malloc(KB_TLS_REQ_MAX);
   char *resp = malloc(KB_TLS_RESP_MAX);
   if (!buf || !resp)
      goto done;

   /* Read until the header terminator (or the body, for small requests). */
   int total = 0;
   while (total < KB_TLS_REQ_MAX - 1)
   {
      int n = SSL_read(ssl, buf + total, KB_TLS_REQ_MAX - 1 - total);
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      const char *hdr_end = strstr(buf, "\r\n\r\n");
      if (hdr_end)
      {
         /* If there is a Content-Length, keep reading until the body arrives. */
         const char *cl = strcasestr(buf, "\r\nContent-Length:");
         long want = 0;
         if (cl)
            want = strtol(cl + 17, NULL, 10);
         int have = total - (int)((hdr_end + 4) - buf);
         if (have >= want)
            break;
      }
   }
   buf[total] = '\0';

   char method[16] = {0}, path[1024] = {0};
   if (sscanf(buf, "%15s %1023s", method, path) < 2)
   {
      const char *b = "{\"error\":\"bad request\"}";
      char head[160];
      int hn = snprintf(head, sizeof(head),
                        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-"
                        "Length: %zu\r\nConnection: close\r\n\r\n",
                        strlen(b));
      SSL_write(ssl, head, hn);
      SSL_write(ssl, b, (int)strlen(b));
      goto done;
   }

   /* Body follows the blank line. */
   const char *body = "";
   int body_len = 0;
   const char *be = strstr(buf, "\r\n\r\n");
   if (be)
   {
      body = be + 4;
      body_len = total - (int)(body - buf);
      if (body_len < 0)
         body_len = 0;
   }

   /* Split query string off the path. */
   char qs[1024] = "", cpath[1024] = "";
   const char *qmark = strchr(path, '?');
   if (qmark)
   {
      size_t plen = (size_t)(qmark - path);
      if (plen >= sizeof(cpath))
         plen = sizeof(cpath) - 1;
      memcpy(cpath, path, plen);
      cpath[plen] = '\0';
      snprintf(qs, sizeof(qs), "%s", qmark + 1);
   }
   else
   {
      snprintf(cpath, sizeof(cpath), "%s", path);
   }

   /* Derive the caller's scope from the verified client certificate. A scoped
    * CN "<kind>:<id>" becomes a synthetic scoped credential the router enforces
    * via verify-then-trust; "global"/owner (no ':') gets full access. */
   char cn[128] = "";
   char synth[160] = "", authhdr[180] = "";
   int have_cert = (kb_tls_peer_cn(ssl, cn, sizeof(cn)) == 0);
   if (have_cert && strchr(cn, ':'))
   {
      snprintf(synth, sizeof(synth), "scope:%s:m", cn);
      snprintf(authhdr, sizeof(authhdr), "Bearer %s", synth);
   }

   /* Revocation (mTLS seam): reject a client cert whose enrollment has been
    * revoked. The DB revoked_at is the source of truth (short-TTL cached); a
    * live cert also gets a debounced last-seen bump. */
   int cert_revoked = 0;
   if (have_cert)
   {
      char fp[65] = "";
      if (kb_tls_peer_fingerprint(ssl, fp, sizeof(fp)) == 0)
      {
         cert_revoked = db2_enrollment_is_revoked(fp);
         if (!cert_revoked)
            db2_enrollment_touch_last_seen(fp, cn); /* cn = the cert's scope identity */
      }
   }

   /* Routes reachable WITHOUT a client cert (the enrollment bootstrap): fetch
    * the CA for TOFU pinning, and redeem a token for a cert. */
   int is_bootstrap =
       (strcmp(cpath, "/v1/enroll/ca") == 0 || strcmp(cpath, "/v1/enroll/redeem") == 0);

   int status;
   /* A revoked client cert is rejected before any route runs. */
   if (cert_revoked)
   {
      snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"client certificate has been revoked\"}");
      status = 401;
   }
   /* A client without a cert yet (still enrolling) may ONLY use bootstrap
    * routes. Everything else requires an identity, so a cert-less peer is 401. */
   else if (!have_cert && !is_bootstrap)
   {
      snprintf(resp, KB_TLS_RESP_MAX,
               "{\"error\":\"client certificate required (enroll first via "
               "/v1/enroll/redeem)\"}");
      status = 401;
   }
   /* GET /v1/enroll/ca: return the CA cert so a bootstrapping client can pin it
    * by fingerprint (the value in its connection string). */
   else if (strcmp(cpath, "/v1/enroll/ca") == 0)
   {
      status = (strcmp(method, "GET") == 0) ? mtls_get_ca(resp, KB_TLS_RESP_MAX) : 405;
      if (status == 405)
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
   }
   /* Cert rotation: an authenticated client renews its cert for its CURRENT
    * verified scope (the cert is the credential — no token needed). */
   else if (have_cert && strcmp(cpath, "/v1/enroll/renew") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
         status = 405;
      }
      else
      {
         status = mtls_renew(cn, body, resp, KB_TLS_RESP_MAX);
      }
   }
   else
   {
      status = kb_http_route_ex(method, cpath, qs, authhdr[0] ? authhdr : NULL,
                                synth[0] ? synth : NULL, body, body_len, resp, KB_TLS_RESP_MAX);
   }

   char head[256];
   int hn = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                     "%zu\r\nConnection: close\r\n\r\n",
                     status, http_reason(status), strlen(resp));
   SSL_write(ssl, head, hn);
   SSL_write(ssl, resp, (int)strlen(resp));

done:
   free(buf);
   free(resp);
   SSL_shutdown(ssl);
   SSL_free(ssl);
}

/* --- the kb mTLS listener (distributed mode) --- */

#include "kb_pki.h"
#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_mtls_listen_fd = -1;
static int g_mtls_port = 0;
static volatile int g_mtls_running = 0;
static pthread_t g_mtls_thread;
static SSL_CTX *g_mtls_ctx = NULL;

static void *mtls_listener_thread(void *arg)
{
   (void)arg;
   while (g_mtls_running)
   {
      int fd = accept(g_mtls_listen_fd, NULL, NULL);
      if (fd < 0)
      {
         if (g_mtls_running)
            continue;
         break;
      }
      kb_tls_serve_conn(fd, g_mtls_ctx);
      close(fd);
   }
   return NULL;
}

int kb_mtls_start(int port, const char *data_dir, const char *host)
{
   if (port < 0 || !data_dir || !data_dir[0] || !host || !host[0])
      return -1;

   /* CA (persistent) + a fresh server cert signed by it. */
   char ca_dir[1024];
   if (snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir) >= (int)sizeof(ca_dir))
      return -1;
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_or_create(ca_dir, &ca, NULL) != 0)
      return -1;
   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   int issued = kb_pki_issue_server_cert(&ca, host, 60L * 60 * 24 * 365, scert, sizeof(scert), skey,
                                         sizeof(skey));
   if (issued != 0)
   {
      OPENSSL_cleanse(&ca, sizeof(ca));
      return -1;
   }
   g_mtls_ctx = kb_tls_server_ctx(ca.cert_pem, scert, skey);
   OPENSSL_cleanse(&ca, sizeof(ca));
   OPENSSL_cleanse(skey, sizeof(skey));
   if (!g_mtls_ctx)
      return -1;

   g_mtls_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   if (g_mtls_listen_fd < 0)
      goto fail;
   int opt = 1;
   setsockopt(g_mtls_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(INADDR_ANY); /* distributed mode: remote peers */
   sa.sin_port = htons((uint16_t)port);
   if (bind(g_mtls_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
       listen(g_mtls_listen_fd, 16) < 0)
      goto fail;

   /* Resolve the actually-bound port (when started with 0). */
   struct sockaddr_in bound;
   socklen_t blen = sizeof(bound);
   if (getsockname(g_mtls_listen_fd, (struct sockaddr *)&bound, &blen) == 0)
      g_mtls_port = ntohs(bound.sin_port);
   else
      g_mtls_port = port;

   g_mtls_running = 1;
   if (pthread_create(&g_mtls_thread, NULL, mtls_listener_thread, NULL) != 0)
   {
      g_mtls_running = 0;
      goto fail;
   }
   LOG_INFO("kb_mtls", "mTLS listening on 0.0.0.0:%d (host %s)", g_mtls_port, host);
   return 0;

fail:
   if (g_mtls_listen_fd >= 0)
   {
      close(g_mtls_listen_fd);
      g_mtls_listen_fd = -1;
   }
   if (g_mtls_ctx)
   {
      SSL_CTX_free(g_mtls_ctx);
      g_mtls_ctx = NULL;
   }
   g_mtls_port = 0;
   return -1;
}

int kb_mtls_bound_port(void)
{
   return g_mtls_running ? g_mtls_port : 0;
}

void kb_mtls_stop(void)
{
   if (!g_mtls_running)
      return;
   g_mtls_running = 0;
   if (g_mtls_listen_fd >= 0)
   {
      shutdown(g_mtls_listen_fd, SHUT_RDWR);
      close(g_mtls_listen_fd);
      g_mtls_listen_fd = -1;
   }
   pthread_join(g_mtls_thread, NULL);
   if (g_mtls_ctx)
   {
      SSL_CTX_free(g_mtls_ctx);
      g_mtls_ctx = NULL;
   }
   g_mtls_port = 0;
}
