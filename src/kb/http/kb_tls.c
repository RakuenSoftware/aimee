/* kb_tls.c: mutual-TLS context builders for aimee-kb. See kb_tls.h.
 * (distributed-mode-auth proposal, mTLS phase.)
 *
 * Identity (cert + key) and the trusted CA are loaded from PEM strings — the
 * same in-memory PEM the CA / cert-issuance layer (kb_pki.h) produces — so no
 * temp files are involved. TLS 1.2+ only. */
#include "kb_tls.h"

#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "cJSON.h"
#include "kb_enroll.h" /* connection-string parse */
#include "kb_pki.h"    /* CA fingerprint for TOFU pinning */

#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Trust `ca` as the verify anchor, and — when both cert_pem and key_pem are
 * non-NULL — load (cert, key) as this peer's own identity. A NULL cert/key pair
 * yields a CA-verify-only context (e.g. a client doing the cert-less enrollment
 * bootstrap). Returns 0 on success, -1 on any failure. */
static int ctx_load_identity(SSL_CTX *ctx, const char *cert_pem, const char *key_pem,
                             const char *ca_pem)
{
   if (!ca_pem)
      return -1;
   int want_identity = (cert_pem && key_pem);

   int ok = -1;
   BIO *cb = want_identity ? BIO_new_mem_buf(cert_pem, -1) : NULL;
   BIO *kb = want_identity ? BIO_new_mem_buf(key_pem, -1) : NULL;
   BIO *ab = BIO_new_mem_buf(ca_pem, -1);
   X509 *cert = cb ? PEM_read_bio_X509(cb, NULL, NULL, NULL) : NULL;
   EVP_PKEY *key = kb ? PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL) : NULL;
   X509 *ca = ab ? PEM_read_bio_X509(ab, NULL, NULL, NULL) : NULL;
   if (!ca)
      goto done;

   if (want_identity)
   {
      if (!cert || !key)
         goto done;
      if (SSL_CTX_use_certificate(ctx, cert) != 1)
         goto done;
      if (SSL_CTX_use_PrivateKey(ctx, key) != 1)
         goto done;
      if (SSL_CTX_check_private_key(ctx) != 1)
         goto done;
   }
   /* Trust the CA for verifying the peer. */
   if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) != 1)
      goto done;
   ok = 0;

done:
   X509_free(ca);
   EVP_PKEY_free(key);
   X509_free(cert);
   BIO_free(ab);
   BIO_free(kb);
   BIO_free(cb);
   return ok;
}

SSL_CTX *kb_tls_server_ctx(const char *ca_cert_pem, const char *server_cert_pem,
                           const char *server_key_pem)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   if (!ctx)
      return NULL;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   if (ctx_load_identity(ctx, server_cert_pem, server_key_pem, ca_cert_pem) != 0)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   /* Request a client cert and REJECT a presented-but-untrusted one, but allow a
    * cert-less handshake so a not-yet-enrolled client can reach /v1/enroll/redeem
    * to bootstrap. kb_tls_serve_conn gates everything else on cert presence. */
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   static const unsigned char session_id_context[] = "aimee-kb-mtls";
   if (SSL_CTX_set_session_id_context(ctx, session_id_context,
                                      (unsigned int)(sizeof(session_id_context) - 1)) != 1)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
   return ctx;
}

SSL_CTX *kb_tls_client_ctx(const char *ca_cert_pem, const char *client_cert_pem,
                           const char *client_key_pem)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
      return NULL;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   if (ctx_load_identity(ctx, client_cert_pem, client_key_pem, ca_cert_pem) != 0)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
   return ctx;
}

int kb_tls_peer_cn(SSL *ssl, char *out, size_t cap)
{
   if (!ssl || !out || cap == 0)
      return -1;
   out[0] = '\0';
   X509 *peer = SSL_get1_peer_certificate(ssl);
   if (!peer)
      return -1;
   X509_NAME *name = X509_get_subject_name(peer);
   int required = X509_NAME_get_text_by_NID(name, NID_commonName, NULL, 0);
   int n = required > 0 && (size_t)required < cap
               ? X509_NAME_get_text_by_NID(name, NID_commonName, out, (int)cap)
               : -1;
   X509_free(peer);
   if (n != required)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int kb_tls_peer_fingerprint(SSL *ssl, char *hex_out, size_t cap)
{
   if (!ssl || !hex_out || cap < 65)
      return -1;
   X509 *peer = SSL_get1_peer_certificate(ssl);
   if (!peer)
      return -1;
   /* sha256 of the DER encoding — must match kb_pki_ca_fingerprint (which the
    * enrollment record was keyed by), so a revoked cert is recognized here. */
   unsigned char md[EVP_MAX_MD_SIZE];
   unsigned int mdlen = 0;
   int rc = -1;
   if (X509_digest(peer, EVP_sha256(), md, &mdlen) == 1 && mdlen == 32)
   {
      for (unsigned int i = 0; i < mdlen; i++)
         snprintf(hex_out + i * 2, 3, "%02x", md[i]);
      hex_out[64] = '\0';
      rc = 0;
   }
   X509_free(peer);
   return rc;
}

/* Peer certificate issuer DN (RFC2253-ish oneline) — the immutable revocation key
 * pairs (issuer, serial), CN being only a policy label (P1 I5). */
int kb_tls_peer_issuer(SSL *ssl, char *out, size_t cap)
{
   if (!ssl || !out || cap == 0)
      return -1;
   out[0] = '\0';
   X509 *peer = SSL_get1_peer_certificate(ssl);
   if (!peer)
      return -1;
   char *dn = X509_NAME_oneline(X509_get_issuer_name(peer), NULL, 0);
   int rc = -1;
   if (dn)
   {
      size_t n = strlen(dn);
      if (n < cap)
      {
         memcpy(out, dn, n + 1);
         rc = 0;
      }
      OPENSSL_free(dn);
   }
   X509_free(peer);
   return rc;
}

/* Peer certificate serial as uppercase hex (no separators). Normalized downstream
 * by kb_cert_serial_normalize into the stable revocation key. */
int kb_tls_peer_serial(SSL *ssl, char *out, size_t cap)
{
   if (!ssl || !out || cap == 0)
      return -1;
   out[0] = '\0';
   X509 *peer = SSL_get1_peer_certificate(ssl);
   if (!peer)
      return -1;
   int rc = -1;
   ASN1_INTEGER *ser = X509_get_serialNumber(peer);
   BIGNUM *bn = ser ? ASN1_INTEGER_to_BN(ser, NULL) : NULL;
   if (bn)
   {
      char *hex = BN_bn2hex(bn);
      if (hex)
      {
         size_t n = strlen(hex);
         if (n < cap)
         {
            memcpy(out, hex, n + 1);
            rc = 0;
         }
         OPENSSL_free(hex);
      }
      BN_free(bn);
   }
   X509_free(peer);
   return rc;
}

/* --- the mTLS client dialer --- */

#include <netdb.h>

#define KB_TLS_CLIENT_HEAD_MAX (64 * 1024)

struct kb_tls_client_conn
{
   SSL_CTX *ctx;
   SSL *ssl;
   int fd;
   char host[256];
};

static int ssl_write_all(SSL *ssl, const char *buf, size_t len)
{
   size_t sent = 0;
   while (sent < len)
   {
      int n = SSL_write(ssl, buf + sent, (int)(len - sent));
      if (n <= 0)
         return -1;
      sent += (size_t)n;
   }
   return 0;
}

static int response_token(const char *start, const char *end, const char *token)
{
   size_t token_len = strlen(token);
   while (start < end)
   {
      while (start < end && (*start == ' ' || *start == '\t' || *start == ','))
         start++;
      const char *item_end = start;
      while (item_end < end && *item_end != ',')
         item_end++;
      const char *trimmed_end = item_end;
      while (trimmed_end > start && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t'))
         trimmed_end--;
      if ((size_t)(trimmed_end - start) == token_len && !strncasecmp(start, token, token_len))
         return 1;
      start = item_end < end ? item_end + 1 : end;
   }
   return 0;
}

static int response_header_name_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int read_content_length_response(SSL *ssl, char *resp_out, size_t resp_cap, int *status_out,
                                        int *reusable_out)
{
   if (resp_cap == 0 || resp_cap > SIZE_MAX - KB_TLS_CLIENT_HEAD_MAX - 1)
      return -1;
   size_t raw_cap = KB_TLS_CLIENT_HEAD_MAX + resp_cap + 1;
   char *raw = malloc(raw_cap);
   if (!raw)
      return -1;
   int rc = -1, status = 0, have_length = 0, connection_close = 0;
   size_t total = 0, head_len = 0, content_len = 0;
   while (total < KB_TLS_CLIENT_HEAD_MAX)
   {
      int n = SSL_read(ssl, raw + total, (int)(KB_TLS_CLIENT_HEAD_MAX - total));
      if (n <= 0)
         goto done;
      total += (size_t)n;
      raw[total] = '\0';
      char *end = strstr(raw, "\r\n\r\n");
      if (end)
      {
         head_len = (size_t)(end + 4 - raw);
         break;
      }
   }
   if (!head_len || head_len > KB_TLS_CLIENT_HEAD_MAX)
      goto done;
   for (size_t i = 0; i < head_len; i++)
      if (raw[i] == '\0' || (raw[i] == '\r' && (i + 1 >= head_len || raw[i + 1] != '\n')) ||
          (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r')))
         goto done;

   char *line_end = strstr(raw, "\r\n");
   char extra;
   if (!line_end)
      goto done;
   *line_end = '\0';
   int fields = sscanf(raw, "HTTP/1.1 %d %c", &status, &extra);
   *line_end = '\r';
   if (fields < 1 || status < 100 || status > 599)
      goto done;

   char *p = line_end + 2;
   char *headers_end = raw + head_len - 2;
   int header_count = 0;
   while (p < headers_end && !(p[0] == '\r' && p[1] == '\n'))
   {
      if (++header_count > 64)
         goto done;
      char *e = strstr(p, "\r\n");
      char *colon = e ? memchr(p, ':', (size_t)(e - p)) : NULL;
      if (!e || e > headers_end || !colon || colon == p || p[0] == ' ' || p[0] == '\t')
         goto done;
      for (char *q = p; q < colon; q++)
         if (!response_header_name_char((unsigned char)*q))
            goto done;
      for (char *q = colon + 1; q < e; q++)
         if (((unsigned char)*q < 0x20 && *q != '\t') || (unsigned char)*q == 0x7f)
            goto done;
      size_t name_len = (size_t)(colon - p);
      if (name_len == 17 && !strncasecmp(p, "Transfer-Encoding", 17))
         goto done;
      if (name_len == 14 && !strncasecmp(p, "Content-Length", 14))
      {
         if (have_length)
            goto done;
         have_length = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         if (v == ve || (ve - v > 1 && *v == '0'))
            goto done;
         size_t value = 0;
         for (char *q = v; q < ve; q++)
         {
            if (*q < '0' || *q > '9' || value > (resp_cap - 1) / 10)
               goto done;
            value = value * 10 + (size_t)(*q - '0');
         }
         content_len = value;
      }
      if (name_len == 10 && !strncasecmp(p, "Connection", 10) &&
          response_token(colon + 1, e, "close"))
         connection_close = 1;
      p = e + 2;
   }
   if (!have_length || content_len >= resp_cap || total > head_len + content_len)
      goto done;
   while (total < head_len + content_len)
   {
      int n = SSL_read(ssl, raw + total, (int)(head_len + content_len - total));
      if (n <= 0)
         goto done;
      total += (size_t)n;
   }
   memcpy(resp_out, raw + head_len, content_len);
   resp_out[content_len] = '\0';
   if (status_out)
      *status_out = status;
   if (reusable_out)
      *reusable_out = !connection_close;
   rc = 0;

done:
   free(raw);
   return rc;
}

static kb_tls_client_conn_t *client_conn_open_owned_ctx(const char *host, int port, SSL_CTX *ctx,
                                                        SSL_SESSION *session)
{
   if (!host || !host[0] || strlen(host) >= 256 || port <= 0 || port > 65535 || !ctx)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   kb_tls_client_conn_t *conn = calloc(1, sizeof(*conn));
   if (!conn)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   conn->fd = -1;
   conn->ctx = ctx;

   char portstr[16];
   snprintf(portstr, sizeof(portstr), "%d", port);
   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, portstr, &hints, &res) != 0)
      goto fail;
   for (struct addrinfo *a = res; a; a = a->ai_next)
   {
      conn->fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (conn->fd < 0)
         continue;
      if (connect(conn->fd, a->ai_addr, a->ai_addrlen) == 0)
         break;
      close(conn->fd);
      conn->fd = -1;
   }
   freeaddrinfo(res);
   if (conn->fd < 0)
      goto fail;
   int one = 1;
   (void)setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
   setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   conn->ssl = SSL_new(conn->ctx);
   if (!conn->ssl)
      goto fail;
   if (session && SSL_set_session(conn->ssl, session) != 1)
      goto fail;
   SSL_set_fd(conn->ssl, conn->fd);
   SSL_set_tlsext_host_name(conn->ssl, host);
   SSL_set1_host(conn->ssl, host);
   if (SSL_connect(conn->ssl) != 1)
      goto fail;
   snprintf(conn->host, sizeof(conn->host), "%s", host);
   return conn;

fail:
   kb_tls_client_conn_close(conn);
   return NULL;
}

kb_tls_client_conn_t *kb_tls_client_conn_open(const char *host, int port, const char *ca_cert_pem,
                                              const char *client_cert_pem,
                                              const char *client_key_pem)
{
   SSL_CTX *ctx = kb_tls_client_ctx(ca_cert_pem, client_cert_pem, client_key_pem);
   return client_conn_open_owned_ctx(host, port, ctx, NULL);
}

kb_tls_client_conn_t *kb_tls_client_conn_open_ctx(const char *host, int port, SSL_CTX *ctx)
{
   if (!ctx || SSL_CTX_up_ref(ctx) != 1)
      return NULL;
   return client_conn_open_owned_ctx(host, port, ctx, NULL);
}

kb_tls_client_conn_t *kb_tls_client_conn_open_session(const char *host, int port, SSL_CTX *ctx,
                                                      SSL_SESSION *session)
{
   if (!ctx || SSL_CTX_up_ref(ctx) != 1)
      return NULL;
   return client_conn_open_owned_ctx(host, port, ctx, session);
}

int kb_tls_client_conn_session_reused(const kb_tls_client_conn_t *conn)
{
   return conn && conn->ssl ? SSL_session_reused(conn->ssl) : 0;
}

SSL_SESSION *kb_tls_client_conn_get1_session(const kb_tls_client_conn_t *conn)
{
   return conn && conn->ssl ? SSL_get1_session(conn->ssl) : NULL;
}

int kb_tls_client_conn_request(kb_tls_client_conn_t *conn, const char *method, const char *path,
                               const char *body, const char *authorization, int close_after,
                               char *resp_out, size_t resp_cap, int *status_out, int *reusable_out)
{
   if (reusable_out)
      *reusable_out = 0;
   if (!conn || !conn->ssl || !method || !path || !resp_out || resp_cap == 0)
      return -1;
   const char *b = body ? body : "";
   size_t blen = strlen(b);
   int bodyless = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
   if (bodyless && blen != 0)
      return -1;
   size_t cap = strlen(method) + strlen(path) + strlen(conn->host) + blen +
                (authorization ? strlen(authorization) : 0) + 192;
   char *req = malloc(cap);
   if (!req)
      return -1;
   int rn = bodyless ? snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n%sConnection: %s\r\n\r\n",
                                method, path, conn->host, authorization ? authorization : "",
                                close_after ? "close" : "keep-alive")
                     : snprintf(req, cap,
                                "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: application/json\r\n"
                                "Content-Length: %zu\r\nConnection: %s\r\n\r\n%s",
                                method, path, conn->host, authorization ? authorization : "", blen,
                                close_after ? "close" : "keep-alive", b);
   int rc =
       (rn > 0 && (size_t)rn < cap && ssl_write_all(conn->ssl, req, (size_t)rn) == 0)
           ? read_content_length_response(conn->ssl, resp_out, resp_cap, status_out, reusable_out)
           : -1;
   free(req);
   if (close_after && reusable_out)
      *reusable_out = 0;
   return rc;
}

void kb_tls_client_conn_close(kb_tls_client_conn_t *conn)
{
   if (!conn)
      return;
   if (conn->ssl)
   {
      SSL_shutdown(conn->ssl);
      SSL_free(conn->ssl);
   }
   if (conn->fd >= 0)
      close(conn->fd);
   SSL_CTX_free(conn->ctx);
   free(conn);
}

int kb_tls_client_request_auth(const char *host, int port, const char *ca_cert_pem,
                               const char *client_cert_pem, const char *client_key_pem,
                               const char *method, const char *path, const char *body,
                               const char *authorization, char *resp_out, size_t resp_cap,
                               int *status_out)
{
   /* client_cert_pem/client_key_pem may be NULL for a cert-less server-auth
    * request (the enrollment bootstrap reaching /v1/enroll/redeem). */
   if (!host || !host[0] || port <= 0 || port > 65535 || !ca_cert_pem || !method || !path ||
       !resp_out || resp_cap == 0)
      return -1;

   kb_tls_client_conn_t *conn =
       kb_tls_client_conn_open(host, port, ca_cert_pem, client_cert_pem, client_key_pem);
   if (!conn)
      return -1;
   int reusable = 0;
   int rc = kb_tls_client_conn_request(conn, method, path, body, authorization, 1, resp_out,
                                       resp_cap, status_out, &reusable);
   kb_tls_client_conn_close(conn);
   return rc;
}

int kb_tls_client_request(const char *host, int port, const char *ca_cert_pem,
                          const char *client_cert_pem, const char *client_key_pem,
                          const char *method, const char *path, const char *body, char *resp_out,
                          size_t resp_cap, int *status_out)
{
   return kb_tls_client_request_auth(host, port, ca_cert_pem, client_cert_pem, client_key_pem,
                                     method, path, body, NULL, resp_out, resp_cap, status_out);
}

/* --- TOFU CA fetch (client bootstrap) --- */

int kb_tls_fetch_ca(const char *host, int port, const char *expected_fp_hex, char *ca_out,
                    size_t ca_cap)
{
   if (!host || !host[0] || port <= 0 || port > 65535 || !expected_fp_hex || !ca_out || ca_cap == 0)
      return -1;

   /* First contact: no CA yet, so do NOT verify the server here — trust comes
    * from matching the fetched CA's fingerprint to the pinned value below. */
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
      return -1;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

   char portstr[16];
   snprintf(portstr, sizeof(portstr), "%d", port);
   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, portstr, &hints, &res) != 0)
   {
      SSL_CTX_free(ctx);
      return -1;
   }
   int fd = -1;
   for (struct addrinfo *a = res; a; a = a->ai_next)
   {
      fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd < 0)
         continue;
      if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
         break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   if (fd < 0)
   {
      SSL_CTX_free(ctx);
      return -1;
   }

   int rc = -1;
   char *raw = NULL;
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
      goto done;
   SSL_set_fd(ssl, fd);
   SSL_set_tlsext_host_name(ssl, host);
   if (SSL_connect(ssl) != 1)
      goto done;

   {
      char req[256];
      int rn =
          snprintf(req, sizeof(req),
                   "GET /v1/enroll/ca HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
      if (SSL_write(ssl, req, rn) <= 0)
         goto done;
   }

   {
      size_t rawcap = 16384;
      raw = malloc(rawcap);
      if (!raw)
         goto done;
      size_t total = 0;
      while (total < rawcap - 1)
      {
         int n = SSL_read(ssl, raw + total, (int)(rawcap - 1 - total));
         if (n <= 0)
            break;
         total += (size_t)n;
      }
      raw[total] = '\0';

      const char *bp = strstr(raw, "\r\n\r\n");
      if (!bp)
         goto done;
      bp += 4;
      cJSON *j = cJSON_Parse(bp);
      const cJSON *jca = j ? cJSON_GetObjectItemCaseSensitive(j, "ca_cert") : NULL;
      if (cJSON_IsString(jca))
      {
         /* Pin: the fetched CA must hash to the out-of-band fingerprint. */
         char fp[KB_PKI_FP_HEX];
         if (kb_pki_ca_fingerprint(jca->valuestring, fp, sizeof(fp)) == 0 &&
             strcmp(fp, expected_fp_hex) == 0 && strlen(jca->valuestring) < ca_cap)
         {
            snprintf(ca_out, ca_cap, "%s", jca->valuestring);
            rc = 0;
         }
      }
      cJSON_Delete(j);
   }

done:
   free(raw);
   if (ssl)
   {
      SSL_shutdown(ssl);
      SSL_free(ssl);
   }
   close(fd);
   SSL_CTX_free(ctx);
   return rc;
}

/* --- full client enrollment (connection string -> mTLS identity) --- */

#include <openssl/evp.h>
#include <openssl/x509.h>

/* Generate a fresh RSA-2048 keypair: write its PKCS#8 private key PEM into
 * key_pem[key_cap] and a self-signed PEM CSR (CN "client"; the server overrides
 * the subject with the token scope) into csr_pem[csr_cap]. Returns 0 / -1. */
static int gen_keypair_csr(char *key_pem, size_t key_cap, char *csr_pem, size_t csr_cap)
{
   int rc = -1;
   EVP_PKEY *key = EVP_RSA_gen(2048);
   X509_REQ *req = X509_REQ_new();
   BIO *kbio = NULL, *rbio = NULL;
   if (!key || !req)
      goto done;
   X509_REQ_set_version(req, 0);
   X509_NAME *n = X509_REQ_get_subject_name(req);
   if (X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"client", -1, -1,
                                  0) != 1)
      goto done;
   if (X509_REQ_set_pubkey(req, key) != 1 || X509_REQ_sign(req, key, EVP_sha256()) <= 0)
      goto done;

   kbio = BIO_new(BIO_s_mem());
   if (!kbio || PEM_write_bio_PrivateKey(kbio, key, NULL, NULL, 0, NULL, NULL) != 1)
      goto done;
   {
      BUF_MEM *bm = NULL;
      BIO_get_mem_ptr(kbio, &bm);
      if (!bm || bm->length == 0 || bm->length >= key_cap)
         goto done;
      memcpy(key_pem, bm->data, bm->length);
      key_pem[bm->length] = '\0';
   }
   rbio = BIO_new(BIO_s_mem());
   if (!rbio || PEM_write_bio_X509_REQ(rbio, req) != 1)
      goto done;
   {
      BUF_MEM *bm = NULL;
      BIO_get_mem_ptr(rbio, &bm);
      if (!bm || bm->length == 0 || bm->length >= csr_cap)
         goto done;
      memcpy(csr_pem, bm->data, bm->length);
      csr_pem[bm->length] = '\0';
   }
   rc = 0;

done:
   BIO_free(kbio);
   BIO_free(rbio);
   X509_REQ_free(req);
   EVP_PKEY_free(key);
   return rc;
}

int kb_tls_enroll(const char *conn_string, char *ca_out, size_t ca_cap, char *cert_out,
                  size_t cert_cap, char *key_out, size_t key_cap)
{
   if (!conn_string || !ca_out || !cert_out || !key_out)
      return -1;

   kb_enroll_conn_t c;
   if (kb_enroll_conn_string_parse(conn_string, &c) != 0)
      return -1;

   /* 1. TOFU-fetch + pin the CA by the connection-string fingerprint. */
   if (kb_tls_fetch_ca(c.host, c.port, c.ca_sha256, ca_out, ca_cap) != 0)
      return -1;

   /* 2. Fresh keypair + CSR (the private key stays here). */
   char csr[4096];
   if (gen_keypair_csr(key_out, key_cap, csr, sizeof(csr)) != 0)
      return -1;

   /* 3. Redeem the token for a cert over the now-pinned (cert-less) connection. */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "token", c.enroll_token);
   cJSON_AddStringToObject(req, "csr", csr);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   char *resp = malloc(16384);
   int status = -1;
   int reqrc = resp ? kb_tls_client_request(c.host, c.port, ca_out, NULL, NULL, "POST",
                                            "/v1/enroll/redeem", body, resp, 16384, &status)
                    : -1;
   free(body);
   if (reqrc != 0 || status != 200)
   {
      free(resp);
      return -1;
   }

   cJSON *j = cJSON_Parse(resp);
   free(resp);
   const cJSON *jcert = j ? cJSON_GetObjectItemCaseSensitive(j, "client_cert") : NULL;
   int rc = -1;
   if (cJSON_IsString(jcert) && strlen(jcert->valuestring) < cert_cap)
   {
      snprintf(cert_out, cert_cap, "%s", jcert->valuestring);
      rc = 0;
   }
   cJSON_Delete(j);
   return rc;
}

/* --- cert rotation (client) --- */

#include <time.h>

int kb_tls_cert_expires_within(const char *cert_pem, long seconds)
{
   if (!cert_pem || !cert_pem[0])
      return -1;
   BIO *b = BIO_new_mem_buf(cert_pem, -1);
   X509 *cert = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
   BIO_free(b);
   if (!cert)
      return -1;
   time_t future = time(NULL) + seconds;
   /* X509_cmp_time(notAfter, &future) < 0 iff notAfter is before `future`, i.e.
    * the cert expires within `seconds`. */
   int cmp = X509_cmp_time(X509_getm_notAfter(cert), &future);
   X509_free(cert);
   if (cmp == 0)
      return -1; /* malformed time */
   return cmp < 0 ? 1 : 0;
}

int kb_tls_renew(const char *host, int port, const char *ca_cert_pem, const char *cur_cert_pem,
                 const char *cur_key_pem, char *cert_out, size_t cert_cap, char *key_out,
                 size_t key_cap)
{
   if (!host || !ca_cert_pem || !cur_cert_pem || !cur_key_pem || !cert_out || !key_out)
      return -1;

   /* Fresh keypair + CSR (the new private key stays here). */
   char csr[4096];
   if (gen_keypair_csr(key_out, key_cap, csr, sizeof(csr)) != 0)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "csr", csr);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   /* POST /v1/enroll/renew authenticated by the CURRENT cert (mTLS). */
   char *resp = malloc(16384);
   int status = -1;
   int reqrc = resp ? kb_tls_client_request(host, port, ca_cert_pem, cur_cert_pem, cur_key_pem,
                                            "POST", "/v1/enroll/renew", body, resp, 16384, &status)
                    : -1;
   free(body);
   if (reqrc != 0 || status != 200)
   {
      free(resp);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   const cJSON *jcert = j ? cJSON_GetObjectItemCaseSensitive(j, "client_cert") : NULL;
   int rc = -1;
   if (cJSON_IsString(jcert) && strlen(jcert->valuestring) < cert_cap)
   {
      snprintf(cert_out, cert_cap, "%s", jcert->valuestring);
      rc = 0;
   }
   cJSON_Delete(j);
   return rc;
}
