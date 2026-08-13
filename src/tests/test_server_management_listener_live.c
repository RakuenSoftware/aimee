#include "server_http.h"
#include "server.h"
#include "runtime_secret.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* server_http's dispatch paths consult the process singleton, but this listener
 * gate never dispatches one of those routes. */
server_ctx_t *server_active_ctx(void)
{
   return NULL;
}

static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   assert(fwrite(text, 1, strlen(text), f) == strlen(text));
   assert(fclose(f) == 0);
}

static void command(const char *cmd)
{
   assert(system(cmd) == 0);
}

static void seal_test_key(const char *name, const char *path)
{
   char pem[8192];
   FILE *f = fopen(path, "rb");
   assert(f);
   size_t n = fread(pem, 1, sizeof(pem) - 1, f);
   assert(!ferror(f) && feof(f) && fclose(f) == 0);
   pem[n] = '\0';
   assert(runtime_secret_store(name, pem) == 0);
   OPENSSL_cleanse(pem, sizeof(pem));
}

static int reserve_port(int keep, int *fd_out)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address = {
       .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   socklen_t length = sizeof(address);
   assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
   int port = ntohs(address.sin_port);
   if (keep)
   {
      assert(listen(fd, 4) == 0);
      *fd_out = fd;
   }
   else
      close(fd);
   return port;
}

static void set_management_env(int port, const char *cert, const char *key, const char *ca)
{
   char text[16];
   snprintf(text, sizeof(text), "%d", port);
   assert(setenv("AIMEE_SERVER_MGMT_PORT", text, 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_BIND", "127.0.0.1", 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_TLS_CERT", cert, 1) == 0);
   seal_test_key("AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY", key);
   assert(setenv("AIMEE_SERVER_MGMT_CLIENT_CA", ca, 1) == 0);
   assert(setenv("AIMEE_SERVER_ID", "p5-live-test", 1) == 0);
   assert(setenv("AIMEE_MGMT_STATUS_KEY_ID", "p5-live-key", 1) == 0);
   assert(setenv("AIMEE_MGMT_STATUS_PUBLIC_KEY",
                 "0000000000000000000000000000000000000000000000000000000000000000", 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_ISSUER", "https://kb.example.test/management", 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE", ca, 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_STATUS_ENDPOINT", "https://127.0.0.1:1", 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_STATUS_CA_FILE", ca, 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_STATUS_LEAF_PIN",
                 "0000000000000000000000000000000000000000000000000000000000000000", 1) == 0);
   assert(setenv("AIMEE_SERVER_MGMT_STATUS_CLIENT_CERT", cert, 1) == 0);
   seal_test_key("AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY", key);
}

static int response_status(const char *response)
{
   int status = 0;
   return response && sscanf(response, "HTTP/1.1 %d", &status) == 1 ? status : 0;
}

static int tls_request(int port, const char *ca, const char *cert, const char *key,
                       const char *request)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address = {.sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   assert(ctx);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   assert(SSL_CTX_load_verify_locations(ctx, ca, NULL) == 1);
   assert(SSL_CTX_use_certificate_chain_file(ctx, cert) == 1);
   assert(SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) == 1);
   SSL *ssl = SSL_new(ctx);
   assert(ssl && SSL_set_fd(ssl, fd) == 1 && SSL_connect(ssl) == 1);
   size_t sent = 0, request_len = strlen(request);
   while (sent < request_len)
   {
      int n = SSL_write(ssl, request + sent, (int)(request_len - sent));
      assert(n > 0);
      sent += (size_t)n;
   }
   char response[8192];
   int total = SSL_read(ssl, response, (int)sizeof(response) - 1);
   assert(total > 0);
   response[total] = '\0';
   SSL_free(ssl);
   SSL_CTX_free(ctx);
   close(fd);
   return response_status(response);
}

static int uds_request(const char *path, const char *request)
{
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   assert(strlen(path) < sizeof(address.sun_path));
   snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
   assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   assert(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
   char response[4096];
   ssize_t n = read(fd, response, sizeof(response) - 1);
   assert(n > 0);
   response[n] = '\0';
   close(fd);
   return response_status(response);
}

static int tcp_request(int port, const char *request)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address = {.sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   assert(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
   char response[4096];
   ssize_t n = read(fd, response, sizeof(response) - 1);
   assert(n > 0);
   response[n] = '\0';
   close(fd);
   return response_status(response);
}

int main(void)
{
   /* Production requires root-owned checkpoint TLS material. Exercise the live
    * listener when the test can create that material without weakening the
    * ownership check; ordinary unprivileged unit runs cover the pure gates. */
   if (geteuid() != 0)
   {
      puts("test_server_management_listener_live: SKIP (requires root-owned TLS fixtures)");
      return 0;
   }
   signal(SIGPIPE, SIG_IGN);
   char dir[256];
   snprintf(dir, sizeof dir, "%s/aimee-mgmt-live-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir));
   char ca[256], cakey[256], server[256], serverkey[256], client[256], clientkey[256], ext[256],
       serverext[256], uds[256], cmd[4096];
#define PATH(name, suffix) snprintf(name, sizeof(name), "%s/%s", dir, suffix)
   PATH(ca, "ca.pem");
   PATH(cakey, "ca.key");
   PATH(server, "server.pem");
   PATH(serverkey, "server.key");
   PATH(client, "client.pem");
   PATH(clientkey, "client.key");
   PATH(ext, "client.ext");
   PATH(serverext, "server.ext");
   PATH(uds, "http.sock");
#undef PATH
   write_text(ext, "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                   "extendedKeyUsage=clientAuth\n"
                   "1.3.6.1.4.1.55555.5.1=DER:"
                   "61696d65652d70352d6b622d6d616e6167656d656e742d7631\n");
   write_text(serverext, "basicConstraints=critical,CA:FALSE\n"
                         "keyUsage=critical,digitalSignature,keyEncipherment\n"
                         "extendedKeyUsage=serverAuth\n");
   snprintf(cmd, sizeof(cmd),
            "openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=management-ca -days 1 "
            "-keyout %s -out %s >/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=management-server -keyout %s "
            "-out %s/server.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/server.csr -CA %s -CAkey %s -CAcreateserial -days 1 "
            "-extfile %s -out %s >/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=p5-kb-management -keyout %s "
            "-out %s/client.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/client.csr -CA %s -CAkey %s -days 1 -extfile %s -out %s "
            ">/dev/null 2>&1",
            cakey, ca, serverkey, dir, dir, ca, cakey, serverext, server, clientkey, dir, dir, ca,
            cakey, ext, client);
   command(cmd);

   /* TLS-profile failure is fatal before the UDS listener is published. */
   int invalid_port = reserve_port(0, NULL);
   set_management_env(invalid_port, client, clientkey, ca);
   assert(server_http_start(uds, 0, 0, NULL, 0, 0) == SERVER_HTTP_START_MGMT_FATAL);
   assert(access(uds, F_OK) != 0);

   int occupied_fd = -1;
   int occupied_port = reserve_port(1, &occupied_fd);
   set_management_env(occupied_port, server, serverkey, ca);
   assert(server_http_start(uds, 0, 0, NULL, 0, 0) == SERVER_HTTP_START_MGMT_FATAL);
   assert(access(uds, F_OK) != 0);
   close(occupied_fd);

   int port = reserve_port(0, NULL);
   int data_port = reserve_port(0, NULL);
   set_management_env(port, server, serverkey, ca);
   assert(server_http_start(uds, data_port, 0, "p5-data-bearer", 0, 0) == 0);
   static const char challenge[] =
       "POST /v1/management/challenge HTTP/1.1\r\nHost: p5-live-test\r\n"
       "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
   int status = tls_request(port, ca, client, clientkey, challenge);
   assert(status == 200 || status == 503);
   static const char generic[] =
       "GET /v1/health HTTP/1.1\r\nHost: p5-live-test\r\nContent-Length: 0\r\n"
       "Connection: close\r\n\r\n";
   assert(tls_request(port, ca, client, clientkey, generic) == 403);
   assert(uds_request(uds, challenge) == 401);
   static const char data_cross_lane[] =
       "POST /v1/management/challenge HTTP/1.1\r\nHost: localhost\r\n"
       "Authorization: Bearer p5-data-bearer\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
   assert(tcp_request(data_port, data_cross_lane) == 401);
   server_http_stop();
   assert(access(uds, F_OK) != 0);

   /* The process-lifetime context and listener lifecycle both support an exact
    * same-packet restart after all listener fds have drained. */
   assert(server_http_start(uds, data_port, 0, "p5-data-bearer", 0, 0) == 0);
   assert(tls_request(port, ca, client, clientkey, generic) == 403);
   server_http_stop();
   assert(access(uds, F_OK) != 0);

   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   command(cmd);
   puts("test_server_management_listener_live: ok");
   return 0;
}
