/* test_aimee_client.c: unit tests for the thin client's transport selection and
 * remote TCP path. A loopback mock HTTP server validates real request framing,
 * bearer-token injection, and response parsing end to end. */
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aimee_client.h"
#include "http_content_encoding.h"

#define PASS(name) printf("  PASS: %s\n", name)

/* Captured first request line + headers from the mock, for assertions. */
static char g_seen_request[4096];

struct mock_cfg
{
   int listen_fd;
   int response_status; /* e.g. 200 */
   const char *response_body;
   int response_gzip;
   int requests;
};

static void *mock_server(void *arg)
{
   struct mock_cfg *cfg = (struct mock_cfg *)arg;
   int requests = cfg->requests ? cfg->requests : 1;
   for (int request = 0; request < requests; request++)
   {
      int c = accept(cfg->listen_fd, NULL, NULL);
      if (c < 0)
         return NULL;
      g_seen_request[0] = '\0';

      /* Read the request: headers, then the Content-Length body (the client does
       * not close its write side, so we must read exactly the advertised body). */
      size_t len = 0;
      size_t want = 0; /* total bytes expected once headers are parsed (0 = unknown) */
      for (;;)
      {
         if (len + 1 >= sizeof(g_seen_request))
            break;
         ssize_t n = read(c, g_seen_request + len, sizeof(g_seen_request) - 1 - len);
         if (n <= 0)
            break;
         len += (size_t)n;
         g_seen_request[len] = '\0';
         char *hdr_end = strstr(g_seen_request, "\r\n\r\n");
         if (hdr_end && want == 0)
         {
            size_t header_bytes = (size_t)(hdr_end - g_seen_request) + 4;
            const char *cl = strstr(g_seen_request, "Content-Length:");
            long body_len = 0;
            if (cl)
               body_len = strtol(cl + 15, NULL, 10);
            want = header_bytes + (size_t)body_len;
         }
         if (want && len >= want)
            break;
      }

      size_t blen = strlen(cfg->response_body), wire_len = blen;
      unsigned char *compressed = NULL;
      const void *wire_body = cfg->response_body;
      if (cfg->response_gzip)
      {
         assert(http_gzip_compress(cfg->response_body, blen, &compressed, &wire_len) == 0);
         wire_body = compressed;
      }
      char resp[512];
      int rlen = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\n%sAccept-Request-Encoding: gzip\r\n"
                          "Connection: close\r\n\r\n",
                          cfg->response_status, wire_len,
                          cfg->response_gzip ? "Content-Encoding: gzip\r\n" : "");
      ssize_t w = write(c, resp, (size_t)rlen);
      if (w == rlen)
         w = write(c, wire_body, wire_len);
      (void)w;
      free(compressed);
      close(c);
   }
   return NULL;
}

/* Start a loopback mock on an ephemeral port; returns the port via *port_out. */
static pthread_t start_mock(struct mock_cfg *cfg, int *port_out)
{
   g_seen_request[0] = '\0';
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0; /* ephemeral */
   assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(fd, 1) == 0);
   socklen_t alen = sizeof(addr);
   assert(getsockname(fd, (struct sockaddr *)&addr, &alen) == 0);
   *port_out = ntohs(addr.sin_port);
   cfg->listen_fd = fd;
   pthread_t th;
   assert(pthread_create(&th, NULL, mock_server, cfg) == 0);
   return th;
}

static void test_tcp_via_env_no_token(void)
{
   struct mock_cfg cfg = {.response_status = 200, .response_body = "{\"ok\":true}"};
   int port = 0;
   pthread_t th = start_mock(&cfg, &port);

   char url[128];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
   setenv("AIMEE_SERVER_URL", url, 1);
   unsetenv("AIMEE_SERVER_TOKEN");
   aimee_client_set_remote(NULL, NULL); /* ensure env path is used */

   int st = -1;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   pthread_join(th, NULL);
   close(cfg.listen_fd);

   assert(body != NULL);
   assert(st == 200);
   assert(strcmp(body, "{\"ok\":true}") == 0);
   /* Request line + no Authorization header. */
   assert(strncmp(g_seen_request, "GET /v1/health HTTP/1.1", 23) == 0);
   assert(strstr(g_seen_request, "Authorization:") == NULL);
   free(body);
   unsetenv("AIMEE_SERVER_URL");
   PASS("tcp via AIMEE_SERVER_URL, no token");
}

static void test_tcp_set_remote_with_token(void)
{
   struct mock_cfg cfg = {.response_status = 201, .response_body = "{}"};
   int port = 0;
   pthread_t th = start_mock(&cfg, &port);

   char url[128];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
   aimee_client_set_remote(url, "s3cr3t");

   int st = -1;
   char *body = aimee_client_request("POST", "/v1/personas", "{\"x\":1}", &st);
   pthread_join(th, NULL);
   close(cfg.listen_fd);

   assert(body != NULL);
   assert(st == 201);
   assert(strstr(g_seen_request, "Authorization: Bearer s3cr3t\r\n") != NULL);
   assert(strstr(g_seen_request, "{\"x\":1}") != NULL); /* body delivered */
   free(body);
   aimee_client_set_remote(NULL, NULL);
   PASS("tcp via set_remote, bearer token + body");
}

static void test_remote_active_reporting(void)
{
   aimee_client_set_remote("http://example.test:8390", NULL);
   char desc[64] = {0};
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "example.test:8390") == 0);

   /* Default port when omitted. */
   aimee_client_set_remote("http://example.test", NULL);
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "example.test:80") == 0);

   aimee_client_set_remote(NULL, NULL);
   unsetenv("AIMEE_SERVER_URL");
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 0);
   PASS("remote_active reporting + default port");
}

static void test_gzip_roundtrip(void)
{
   char *large = malloc(8193);
   assert(large);
   for (int i = 0; i < 8192; i++)
      large[i] = (char)('a' + ((i * 31 + (i / 97) * 7) % 26));
   large[8192] = '\0';
   struct mock_cfg cfg = {
       .response_status = 200, .response_body = large, .response_gzip = 1, .requests = 2};
   int port = 0;
   pthread_t th = start_mock(&cfg, &port);
   char url[128];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
   setenv("AIMEE_TRANSPORT_THINCLIENT_GZIP_ENABLED", "1", 1);
   aimee_client_set_remote(url, NULL);

   int status = -1;
   char *body = aimee_client_request("POST", "/v1/responses", large, &status);
   assert(status == 200 && body && strcmp(body, large) == 0);
   free(body);
   /* The first authenticated response advertises request-gzip support. */
   body = aimee_client_request("POST", "/v1/responses", large, &status);
   pthread_join(th, NULL);
   close(cfg.listen_fd);
   assert(status == 200 && body && strcmp(body, large) == 0);
   assert(strstr(g_seen_request, "Accept-Encoding: gzip\r\n"));
   assert(strstr(g_seen_request, "Content-Encoding: gzip\r\n"));

   free(body);
   free(large);
   unsetenv("AIMEE_TRANSPORT_THINCLIENT_GZIP_ENABLED");
   aimee_client_set_remote(NULL, NULL);
   PASS("negotiated gzip request and response round-trip");
}

static void test_https_unreachable_null(void)
{
   /* https:// to an unresolvable host returns NULL in every build: a no-TLS
    * build refuses it; a WITH_TLS build fails to connect. */
   aimee_client_set_remote("https://example.test", NULL);
   int st = -1;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   assert(body == NULL); /* no in-client TLS yet */
   aimee_client_set_remote(NULL, NULL);
   PASS("https:// to unreachable host returns NULL");
}

#ifdef WITH_TLS
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* In-process self-signed cert (OpenSSL 3): lets the mock speak TLS so the client
 * exercises a real https:// handshake with AIMEE_TLS_INSECURE=1. */
static int load_self_signed(SSL_CTX *ctx)
{
   EVP_PKEY *pkey = EVP_RSA_gen(2048);
   if (!pkey)
      return -1;
   X509 *x = X509_new();
   if (!x)
   {
      EVP_PKEY_free(pkey);
      return -1;
   }
   ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
   X509_gmtime_adj(X509_getm_notBefore(x), 0);
   X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 3650);
   X509_set_pubkey(x, pkey);
   X509_NAME *nm = X509_get_subject_name(x);
   X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char *)"127.0.0.1", -1, -1,
                              0);
   X509_set_issuer_name(x, nm);
   int ok = (X509_sign(x, pkey, EVP_sha256()) > 0) && SSL_CTX_use_certificate(ctx, x) == 1 &&
            SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
   X509_free(x);
   EVP_PKEY_free(pkey);
   return ok ? 0 : -1;
}

static int g_tls_listen_fd;
static int g_tls_requests;
static int g_tls_seen_requests;

static void *tls_mock_server(void *arg)
{
   (void)arg;
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   if (!ctx || load_self_signed(ctx) != 0)
   {
      if (ctx)
         SSL_CTX_free(ctx);
      return NULL;
   }
   int c = accept(g_tls_listen_fd, NULL, NULL);
   if (c < 0)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL *ssl = SSL_new(ctx);
   SSL_set_fd(ssl, c);
   if (SSL_accept(ssl) == 1)
   {
      for (int request = 0; request < g_tls_requests; request++)
      {
         char buf[4096];
         int n = SSL_read(ssl, buf, sizeof(buf) - 1);
         if (n <= 0)
            break;
         buf[n] = '\0';
         assert(strstr(buf, "Connection: keep-alive\r\n"));
         g_tls_seen_requests++;
         const char *body = "{\"ok\":true}";
         char resp[256];
         int rlen = snprintf(resp, sizeof(resp),
                             "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n%s",
                             (int)strlen(body),
                             request + 1 == g_tls_requests ? "close" : "keep-alive", body);
         SSL_write(ssl, resp, rlen);
      }
   }
   SSL_shutdown(ssl);
   SSL_free(ssl);
   close(c);
   SSL_CTX_free(ctx);
   return NULL;
}

static void test_tls_roundtrip(void)
{
   aimee_client_set_remote(NULL, NULL);
   unsetenv("AIMEE_SERVER_URL");

   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(fd, 1) == 0);
   socklen_t alen = sizeof(addr);
   assert(getsockname(fd, (struct sockaddr *)&addr, &alen) == 0);
   int port = ntohs(addr.sin_port);
   g_tls_listen_fd = fd;
   g_tls_requests = 2;
   g_tls_seen_requests = 0;

   pthread_t th;
   assert(pthread_create(&th, NULL, tls_mock_server, NULL) == 0);

   char url[64];
   snprintf(url, sizeof(url), "https://127.0.0.1:%d", port);
   setenv("AIMEE_TLS_INSECURE", "1", 1); /* accept the self-signed cert */
   /* Keep-alive is the measured default; no environment opt-in is required. */
   unsetenv("AIMEE_TRANSPORT_SERVER_KEEPALIVE_ENABLED");
   aimee_client_set_remote(url, NULL);

   int st = -1;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   assert(body != NULL && st == 200 && strcmp(body, "{\"ok\":true}") == 0);
   free(body);
   body = aimee_client_request("GET", "/v1/health", NULL, &st);
   pthread_join(th, NULL);
   close(fd);

   assert(body != NULL);
   assert(st == 200);
   assert(strcmp(body, "{\"ok\":true}") == 0);
   assert(g_tls_seen_requests == 2);
   free(body);
   unsetenv("AIMEE_TLS_INSECURE");
   unsetenv("AIMEE_TRANSPORT_SERVER_KEEPALIVE_ENABLED");
   aimee_client_set_remote(NULL, NULL);
   PASS("tls https keep-alive reuses one connection for two requests");
}
#endif /* WITH_TLS */

/* The credential-in-cleartext guard: a non-empty bearer over plaintext http://
 * to a non-loopback host must be refused; loopback and https are allowed. */
static void test_cleartext_credential_guard(void)
{
   /* Blocked: a real bearer over plaintext http:// to a remote address. */
   assert(aimee_client_would_leak_cleartext(0, "192.168.1.254", "secret") == 1);
   assert(aimee_client_would_leak_cleartext(0, "example.com", "secret") == 1);
   assert(aimee_client_would_leak_cleartext(0, "10.0.0.5", "tok") == 1);

   /* Allowed: TLS carries the bearer confidentially regardless of host. */
   assert(aimee_client_would_leak_cleartext(1, "192.168.1.254", "secret") == 0);

   /* Allowed: loopback never leaves the machine, plaintext is fine there. */
   assert(aimee_client_would_leak_cleartext(0, "127.0.0.1", "secret") == 0);
   assert(aimee_client_would_leak_cleartext(0, "127.5.6.7", "secret") == 0);
   assert(aimee_client_would_leak_cleartext(0, "localhost", "secret") == 0);
   assert(aimee_client_would_leak_cleartext(0, "::1", "secret") == 0);
   assert(aimee_client_would_leak_cleartext(0, "[::1]", "secret") == 0);

   /* Allowed: no credential to leak (empty / NULL token). */
   assert(aimee_client_would_leak_cleartext(0, "example.com", "") == 0);
   assert(aimee_client_would_leak_cleartext(0, "example.com", NULL) == 0);
   PASS("cleartext-credential guard: refuses bearer over plaintext to non-loopback");
}

int main(void)
{
   printf("test_aimee_client:\n");
   test_tcp_via_env_no_token();
   test_tcp_set_remote_with_token();
   test_gzip_roundtrip();
   test_remote_active_reporting();
   test_https_unreachable_null();
   test_cleartext_credential_guard();
#ifdef WITH_TLS
   test_tls_roundtrip();
#endif
   printf("ALL PASS\n");
   return 0;
}
