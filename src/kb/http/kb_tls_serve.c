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
#include "kb_enroll.h" /* KB_ENROLL_SCOPE_MAX */
#include "kb_http.h"   /* kb_http_route_ex */
#include "kb_http_egress.h"
#include "../../db2/server_registry.h"
#include "../../db2/db2_tenant.h"
#include "kb_ingress.h" /* B5 identity-header ingress guard */
#include "kb_reqctx.h"
#include "log.h"             /* LOG_WARN */
#include "db2/enrollments.h" /* revocation source of truth + last-seen */
#include "kb_paths.h"        /* kb_default_config_dir */
#include "kb_pki.h"          /* CA load + CSR signing for renew */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#define KB_TLS_REQ_MAX    (1024 * 1024)
#define KB_TLS_HEADER_MAX 16384
#define KB_TLS_RESP_MAX   262144

static int header_name_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Read exactly one strict HTTP/1.1 request. Returns an HTTP error status or 0. */
static int strict_request_read(SSL *ssl, char *buf, size_t cap, int *total_out, int *header_out,
                               size_t *body_out)
{
   size_t total = 0, header_len = 0, content_len = 0;
   int have_cl = 0;
   while (total + 1 < cap && total < KB_TLS_HEADER_MAX)
   {
      int n = SSL_read(ssl, buf + total, (int)(KB_TLS_HEADER_MAX - total));
      if (n <= 0)
         return 400;
      total += (size_t)n;
      buf[total] = '\0';
      char *end = strstr(buf, "\r\n\r\n");
      if (end)
      {
         header_len = (size_t)(end + 4 - buf);
         break;
      }
   }
   if (!header_len || header_len > KB_TLS_HEADER_MAX)
      return 413;
   for (size_t i = 0; i < header_len; i++)
      if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r'))
         return 400;
   char *line_end = strstr(buf, "\r\n");
   if (!line_end)
      return 400;
   char method[16], target[1024], version[16], extra;
   *line_end = '\0';
   int fields = sscanf(buf, "%15s %1023s %15s %c", method, target, version, &extra);
   *line_end = '\r';
   if (fields != 3 || strcmp(version, "HTTP/1.1") || target[0] != '/' || strstr(target, "://") ||
       strchr(target, '#'))
      return 400;
   char *p = line_end + 2;
   char *headers_end = buf + header_len - 2;
   while (p < headers_end && !(p[0] == '\r' && p[1] == '\n'))
   {
      char *e = strstr(p, "\r\n");
      if (!e || e > headers_end || p[0] == ' ' || p[0] == '\t')
         return 400;
      char *colon = memchr(p, ':', (size_t)(e - p));
      if (!colon || colon == p || colon[-1] == ' ' || colon[-1] == '\t')
         return 400;
      for (char *q = p; q < colon; q++)
         if (!header_name_char((unsigned char)*q))
            return 400;
      for (char *q = colon + 1; q < e; q++)
         if (((unsigned char)*q < 0x20 && *q != '\t') || (unsigned char)*q == 0x7f)
            return 400;
      size_t name_len = (size_t)(colon - p);
      if (name_len == 17 && !strncasecmp(p, "Transfer-Encoding", 17))
         return 400;
      if (name_len == 14 && !strncasecmp(p, "Content-Length", 14))
      {
         if (have_cl)
            return 400;
         have_cl = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         if (v == ve || (ve - v > 1 && *v == '0'))
            return 400;
         size_t value = 0;
         for (char *q = v; q < ve; q++)
         {
            if (*q < '0' || *q > '9' || value > (cap - 1) / 10)
               return 400;
            value = value * 10 + (size_t)(*q - '0');
         }
         content_len = value;
      }
      p = e + 2;
   }
   int bodyless = !strcmp(method, "GET") || !strcmp(method, "HEAD");
   if ((bodyless && have_cl) || (!bodyless && !have_cl))
      return 400;
   if (content_len > cap - header_len - 1)
      return 413;
   if (total > header_len + content_len)
      return 400;
   while (total < header_len + content_len)
   {
      int n = SSL_read(ssl, buf + total, (int)(header_len + content_len - total));
      if (n <= 0)
         return 400;
      total += (size_t)n;
   }
   buf[total] = '\0';
   *total_out = (int)total;
   *header_out = (int)header_len;
   *body_out = content_len;
   return 0;
}

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
   case 402:
      return "Payment Required";
   case 403:
      return "Forbidden";
   case 404:
      return "Not Found";
   case 405:
      return "Method Not Allowed";
   case 409:
      return "Conflict";
   case 429:
      return "Too Many Requests";
   case 500:
      return "Internal Server Error";
   case 502:
      return "Bad Gateway";
   case 503:
      return "Service Unavailable";
   case 504:
      return "Gateway Timeout";
   default:
      return "OK";
   }
}

/* Certificate-bound server heartbeat. The server_id is untrusted input but the
 * DB update is keyed by both server_id and the verified peer cert CN, so a
 * certificate cannot refresh another registry row. */
static int mtls_server_heartbeat(const char *issuer, const char *serial, const char *fingerprint,
                                 const char *body, char *resp, int cap)
{
   cJSON *j = body ? cJSON_Parse(body) : NULL;
   cJSON *sid = j ? cJSON_GetObjectItemCaseSensitive(j, "server_id") : NULL;
   cJSON *health = j ? cJSON_GetObjectItemCaseSensitive(j, "health") : NULL;
   cJSON *version = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
   int ok = cJSON_IsString(sid) && cJSON_IsString(health) && cJSON_IsString(version) &&
            db2_server_registry_heartbeat(cJSON_GetStringValue(sid), issuer, serial, fingerprint,
                                          cJSON_GetStringValue(health),
                                          cJSON_GetStringValue(version)) == 0;
   cJSON_Delete(j);
   snprintf(resp, (size_t)cap, ok ? "{\"ok\":true}" : "{\"error\":\"heartbeat rejected\"}");
   return ok ? 200 : 403;
}

/* GET /v1/enroll/ca: return the CA certificate (public trust anchor) so a
 * bootstrapping client can pin it by fingerprint. The private key is never
 * exposed. Writes the JSON response; returns the HTTP status. */
static int mtls_get_ca(char *resp, int cap)
{
   char ca_dir[1024];
   snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
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
static int mtls_renew(const char *scope_cn, const char *old_fp, const char *old_issuer,
                      const char *old_serial_norm, const char *body, char *resp, int cap)
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
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
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
   char new_fp[KB_PKI_FP_HEX] = "", new_issuer[256] = "", raw_serial[128] = "";
   char new_serial[128] = "";
   int metadata_ok = kb_pki_ca_fingerprint(cert, new_fp, sizeof(new_fp)) == 0 &&
                     kb_pki_cert_metadata(cert, new_issuer, sizeof(new_issuer), raw_serial,
                                          sizeof(raw_serial)) == 0 &&
                     kb_cert_serial_normalize(raw_serial, new_serial, sizeof(new_serial)) == 0;
   kb_principal_t renew_actor = {.kind = KB_PRIN_CERT, .authenticated = 1};
   snprintf(renew_actor.issuer, sizeof(renew_actor.issuer), "%s", old_issuer);
   snprintf(renew_actor.subject, sizeof(renew_actor.subject), "%s", old_serial_norm);
   int persisted = -1;
   if (metadata_ok && db2_tenant_scope_begin(&renew_actor, 0) == 0)
   {
      persisted = db2_enrollment_renew(old_fp, old_issuer, old_serial_norm, scope_cn, new_fp,
                                       new_issuer, new_serial, NULL);
      if (persisted == 0)
         persisted = db2_tenant_scope_commit();
      else
         db2_tenant_scope_rollback();
   }
   if (!metadata_ok || persisted != 0)
   {
      OPENSSL_cleanse(cert, KB_PKI_CERT_PEM_MAX);
      free(cert);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew persistence unavailable\"}");
      return 503;
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
   struct timeval io_timeout = {.tv_sec = 35, .tv_usec = 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
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

   int total = 0, header_len = 0;
   size_t declared_body = 0;
   int read_status =
       strict_request_read(ssl, buf, KB_TLS_REQ_MAX, &total, &header_len, &declared_body);
   if (read_status)
   {
      const char *b =
          read_status == 413 ? "{\"error\":\"request too large\"}" : "{\"error\":\"bad request\"}";
      char head[160];
      int hn = snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                        "%zu\r\nConnection: close\r\n\r\n",
                        read_status, read_status == 413 ? "Payload Too Large" : "Bad Request",
                        strlen(b));
      SSL_write(ssl, head, hn);
      SSL_write(ssl, b, (int)strlen(b));
      goto done;
   }

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
   if (header_len > 0)
   {
      body = buf + header_len;
      body_len = (int)declared_body;
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
   char fp[65] = "", issuer[256] = "", serial[128] = "";
   kb_principal_t transport;
   memset(&transport, 0, sizeof(transport));
   if (have_cert)
   {
      if (kb_tls_peer_fingerprint(ssl, fp, sizeof(fp)) == 0 &&
          kb_tls_peer_issuer(ssl, issuer, sizeof(issuer)) == 0 &&
          kb_tls_peer_serial(ssl, serial, sizeof(serial)) == 0 &&
          kb_principal_from_cert(issuer, serial, cn, &transport) == 0)
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
   /* B5: kb never honors a client-supplied identity header; reject fail-closed
    * before any route runs. */
   if (kb_ingress_identity_header_present(buf))
   {
      LOG_WARN("kb.tls",
               "kb ingress (mtls): rejected request bearing a spoofable X-Aimee-* identity header");
      snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"identity header not permitted\"}");
      status = 400;
   }
   /* A revoked client cert is rejected before any route runs. */
   else if (cert_revoked)
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
   /* Certificate-only P2b egress authority: never route through the synthetic
    * CN bearer. The exact verified issuer/serial/fingerprint are carried in. */
   else if (have_cert && strcmp(cpath, "/v1/llm/egress") == 0)
   {
      status = kb_http_egress_route(method, cpath, body, body_len, &transport, fp, resp,
                                    KB_TLS_RESP_MAX);
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
         status =
             mtls_renew(cn, fp, transport.issuer, transport.subject, body, resp, KB_TLS_RESP_MAX);
      }
   }
   else if (have_cert && strcmp(cpath, "/v1/server/heartbeat") == 0)
   {
      status = (strcmp(method, "POST") == 0)
                   ? mtls_server_heartbeat(transport.issuer, transport.subject, fp, body, resp,
                                           KB_TLS_RESP_MAX)
                   : 405;
      if (status == 405)
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
   }
   else
   {
      status = kb_http_route_ex(method, cpath, qs, authhdr[0] ? authhdr : NULL,
                                synth[0] ? synth : NULL, body, body_len, resp, KB_TLS_RESP_MAX);
   }
   kb_reqctx_clear(); /* drop the request's actor before the next request on this conn */

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

#define KB_MTLS_WORKERS   8
#define KB_MTLS_QUEUE_CAP 32
static pthread_t g_mtls_workers[KB_MTLS_WORKERS];
static int g_mtls_workers_started = 0;
static int g_mtls_queue[KB_MTLS_QUEUE_CAP];
static size_t g_mtls_queue_head = 0;
static size_t g_mtls_queue_len = 0;
static pthread_mutex_t g_mtls_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mtls_queue_cv = PTHREAD_COND_INITIALIZER;

static void *mtls_worker_thread(void *arg)
{
   (void)arg;
   for (;;)
   {
      pthread_mutex_lock(&g_mtls_queue_mu);
      while (g_mtls_queue_len == 0 && g_mtls_running)
         pthread_cond_wait(&g_mtls_queue_cv, &g_mtls_queue_mu);
      if (g_mtls_queue_len == 0 && !g_mtls_running)
      {
         pthread_mutex_unlock(&g_mtls_queue_mu);
         break;
      }
      int fd = g_mtls_queue[g_mtls_queue_head];
      g_mtls_queue_head = (g_mtls_queue_head + 1) % KB_MTLS_QUEUE_CAP;
      g_mtls_queue_len--;
      pthread_mutex_unlock(&g_mtls_queue_mu);

      kb_tls_serve_conn(fd, g_mtls_ctx);
      close(fd);
   }
   return NULL;
}

static int mtls_queue_conn(int fd)
{
   int queued = 0;
   pthread_mutex_lock(&g_mtls_queue_mu);
   if (g_mtls_running && g_mtls_queue_len < KB_MTLS_QUEUE_CAP)
   {
      size_t tail = (g_mtls_queue_head + g_mtls_queue_len) % KB_MTLS_QUEUE_CAP;
      g_mtls_queue[tail] = fd;
      g_mtls_queue_len++;
      queued = 1;
      pthread_cond_signal(&g_mtls_queue_cv);
   }
   pthread_mutex_unlock(&g_mtls_queue_mu);
   return queued ? 0 : -1;
}

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
      /* Bound both concurrent handshakes and queued sockets. Saturation is
       * fail-closed: the accepted socket is dropped before reading a request. */
      if (mtls_queue_conn(fd) != 0)
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
   if (kb_pki_ca_load_or_create_custodied(ca_dir, &ca, NULL) != 0)
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

   pthread_mutex_lock(&g_mtls_queue_mu);
   g_mtls_queue_head = 0;
   g_mtls_queue_len = 0;
   g_mtls_running = 1;
   pthread_mutex_unlock(&g_mtls_queue_mu);
   for (int i = 0; i < KB_MTLS_WORKERS; i++)
   {
      if (pthread_create(&g_mtls_workers[i], NULL, mtls_worker_thread, NULL) != 0)
      {
         pthread_mutex_lock(&g_mtls_queue_mu);
         g_mtls_running = 0;
         pthread_cond_broadcast(&g_mtls_queue_cv);
         pthread_mutex_unlock(&g_mtls_queue_mu);
         for (int j = 0; j < i; j++)
            pthread_join(g_mtls_workers[j], NULL);
         g_mtls_workers_started = 0;
         goto fail;
      }
      g_mtls_workers_started++;
   }
   if (pthread_create(&g_mtls_thread, NULL, mtls_listener_thread, NULL) != 0)
   {
      pthread_mutex_lock(&g_mtls_queue_mu);
      g_mtls_running = 0;
      pthread_cond_broadcast(&g_mtls_queue_cv);
      pthread_mutex_unlock(&g_mtls_queue_mu);
      for (int i = 0; i < g_mtls_workers_started; i++)
         pthread_join(g_mtls_workers[i], NULL);
      g_mtls_workers_started = 0;
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
   pthread_mutex_lock(&g_mtls_queue_mu);
   g_mtls_running = 0;
   pthread_mutex_unlock(&g_mtls_queue_mu);
   if (g_mtls_listen_fd >= 0)
   {
      shutdown(g_mtls_listen_fd, SHUT_RDWR);
      close(g_mtls_listen_fd);
      g_mtls_listen_fd = -1;
   }
   pthread_join(g_mtls_thread, NULL);
   pthread_mutex_lock(&g_mtls_queue_mu);
   pthread_cond_broadcast(&g_mtls_queue_cv);
   pthread_mutex_unlock(&g_mtls_queue_mu);
   for (int i = 0; i < g_mtls_workers_started; i++)
      pthread_join(g_mtls_workers[i], NULL);
   g_mtls_workers_started = 0;
   if (g_mtls_ctx)
   {
      SSL_CTX_free(g_mtls_ctx);
      g_mtls_ctx = NULL;
   }
   g_mtls_port = 0;
}
