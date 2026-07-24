#include "kb/kb_mgmt_status_client.h"

#include "kb_pki.h"
#include "kb_tls.h"

#include <arpa/inet.h>
#include <assert.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
   SSL_CTX *ctx;
   int listener;
   const char *reply;
   int handshake_ok;
   int saw_client_cert;
   int saw_status_request;
} authority_t;

static uint64_t monotonic_ms(void)
{
   struct timespec ts;
   assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
   return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void *authority_main(void *opaque)
{
   authority_t *a = opaque;
   int fd = accept(a->listener, NULL, NULL);
   if (fd < 0)
      return NULL;
   SSL *ssl = SSL_new(a->ctx);
   assert(ssl);
   SSL_set_fd(ssl, fd);
   if (SSL_accept(ssl) == 1)
   {
      a->handshake_ok = 1;
      X509 *peer = SSL_get1_peer_certificate(ssl);
      a->saw_client_cert = peer != NULL && SSL_get_verify_result(ssl) == X509_V_OK;
      X509_free(peer);
      char request[4096] = {0};
      size_t used = 0;
      while (used < sizeof(request) - 1)
      {
         int n = SSL_read(ssl, request + used, (int)(sizeof(request) - 1 - used));
         if (n <= 0)
            break;
         used += (size_t)n;
         request[used] = 0;
         char *headers = strstr(request, "\r\n\r\n");
         if (headers && used >= (size_t)(headers + 4 - request) + 2)
            break;
      }
      a->saw_status_request =
          strstr(request, "POST /v1/management/status HTTP/1.1\r\n") == request &&
          strstr(request, "\r\n\r\n{}") != NULL;
      (void)SSL_write(ssl, a->reply, (int)strlen(a->reply));
      (void)SSL_shutdown(ssl);
   }
   SSL_free(ssl);
   close(fd);
   return NULL;
}

static pthread_t authority_start(authority_t *a, SSL_CTX *ctx, const char *reply, char *endpoint,
                                 size_t endpoint_cap)
{
   memset(a, 0, sizeof(*a));
   a->ctx = ctx;
   a->reply = reply;
   a->listener = socket(AF_INET, SOCK_STREAM, 0);
   assert(a->listener >= 0);
   struct sockaddr_in addr = {
       .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
   assert(bind(a->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(a->listener, 1) == 0);
   socklen_t addr_len = sizeof(addr);
   assert(getsockname(a->listener, (struct sockaddr *)&addr, &addr_len) == 0);
   assert(snprintf(endpoint, endpoint_cap, "https://localhost:%u", ntohs(addr.sin_port)) > 0);
   pthread_t thread;
   assert(pthread_create(&thread, NULL, authority_main, a) == 0);
   return thread;
}

static void authority_join(authority_t *a, pthread_t thread)
{
   assert(pthread_join(thread, NULL) == 0);
   close(a->listener);
}

static kb_management_health_result_t
issue_once(SSL_CTX *server_ctx, const kb_management_cert_bundle_t *bundle, const char *ca_pem,
           const char *primary, const char *secondary, const char *reply, authority_t *authority,
           char *response, size_t response_cap, int *status)
{
   char endpoint[128];
   pthread_t thread = authority_start(authority, server_ctx, reply, endpoint, sizeof(endpoint));
   kb_mgmt_status_client_config_t config = {.endpoint = endpoint,
                                            .ca_pem = ca_pem,
                                            .leaf_pin = primary,
                                            .secondary_leaf_pin = secondary};
   kb_management_health_result_t result = kb_mgmt_status_client_issue(
       &config, bundle, "{}", 2, monotonic_ms() + 5000, response, response_cap, status);
   authority_join(authority, thread);
   return result;
}

int main(void)
{
   signal(SIGPIPE, SIG_IGN);
   char a[65], b[65], c[65];
   memset(a, 'a', 64);
   memset(b, 'b', 64);
   memset(c, 'c', 64);
   a[64] = b[64] = c[64] = 0;
   assert(kb_mgmt_status_client_pin_matches(a, a, NULL));
   assert(kb_mgmt_status_client_pin_matches(b, a, b));
   assert(!kb_mgmt_status_client_pin_matches(c, a, b));
   a[0] = 'A';
   assert(!kb_mgmt_status_client_pin_matches(a, a, NULL));

   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0);
   char server_cert[KB_PKI_CERT_PEM_MAX], server_key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(&ca, "localhost", 3600, server_cert, sizeof(server_cert),
                                   server_key, sizeof(server_key)) == 0);
   char client_cert[KB_PKI_CERT_PEM_MAX], client_key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&ca, "aimee-kb-management", 3600, client_cert,
                                   sizeof(client_cert), client_key, sizeof(client_key)) == 0);
   SSL_CTX *server_ctx = kb_tls_server_ctx(ca.cert_pem, server_cert, server_key);
   assert(server_ctx);
   kb_management_cert_bundle_t bundle = {0};
   memcpy(bundle.leaf_pem, client_cert, strlen(client_cert) + 1);
   bundle.leaf_pem_len = strlen(client_cert);
   memcpy(bundle.key_pem, client_key, strlen(client_key) + 1);
   bundle.key_pem_len = strlen(client_key);

   char server_pin[65];
   assert(kb_pki_ca_fingerprint(server_cert, server_pin, sizeof(server_pin)) == 0);
   char other_pin[65];
   memset(other_pin, '0', 64);
   other_pin[64] = 0;
   if (strcmp(other_pin, server_pin) == 0)
      other_pin[0] = '1';
   const char *ok_reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
   char response[256];
   int status = 0;
   authority_t authority;

   /* The reverse-management connector is explicitly trusted to dial private
    * authority addresses, but still requires CA-authenticated mutual TLS and a
    * leaf pin. A normal untrusted connector would filter 127.0.0.1. */
   assert(issue_once(server_ctx, &bundle, ca.cert_pem, server_pin, NULL, ok_reply, &authority,
                     response, sizeof(response), &status) == KB_MANAGEMENT_HEALTH_OK);
   assert(status == 200 && strcmp(response, "{}") == 0);
   assert(authority.handshake_ok && authority.saw_client_cert && authority.saw_status_request);

   /* Rotation accepts the secondary pin, while neither a missing primary nor
    * a mismatch is allowed to authorize the TLS peer. */
   assert(issue_once(server_ctx, &bundle, ca.cert_pem, other_pin, server_pin, ok_reply, &authority,
                     response, sizeof(response), &status) == KB_MANAGEMENT_HEALTH_OK);
   assert(authority.saw_client_cert && authority.saw_status_request);
   assert(issue_once(server_ctx, &bundle, ca.cert_pem, other_pin, NULL, ok_reply, &authority,
                     response, sizeof(response), &status) == KB_MANAGEMENT_HEALTH_INTEGRITY);
   assert(response[0] == 0);

   kb_mgmt_status_client_config_t expired = {
       .endpoint = "https://localhost:443", .ca_pem = ca.cert_pem, .leaf_pin = server_pin};
   memset(response, 'x', sizeof(response));
   status = 99;
   assert(kb_mgmt_status_client_issue(&expired, &bundle, "{}", 2, monotonic_ms(), response,
                                      sizeof(response), &status) == KB_MANAGEMENT_HEALTH_INVALID);
   assert(response[0] == 0); /* Refused before DNS/connect/TLS work. */
   expired.leaf_pin = NULL;
   assert(kb_mgmt_status_client_issue(&expired, &bundle, "{}", 2, UINT64_MAX, response,
                                      sizeof(response), &status) == KB_MANAGEMENT_HEALTH_INVALID);

   static const char *forbidden[] = {
       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nUpgrade: websocket\r\n\r\n{}",
       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTrailer: Digest\r\n\r\n{}",
       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\nConnection: "
       "close\r\n\r\n{}"};
   for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++)
   {
      memset(response, 'x', sizeof(response));
      status = 99;
      assert(issue_once(server_ctx, &bundle, ca.cert_pem, server_pin, NULL, forbidden[i],
                        &authority, response, sizeof(response),
                        &status) == KB_MANAGEMENT_HEALTH_UNAVAILABLE);
      assert(authority.saw_client_cert && authority.saw_status_request);
      assert(response[0] == 0 && status == 0);
   }

   SSL_CTX_free(server_ctx);
   puts("kb_mgmt_status_client: all tests passed");
   return 0;
}
