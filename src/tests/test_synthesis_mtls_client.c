/* test_synthesis_mtls_client.c: the kb's OWN http client against a real mTLS peer.
 *
 * WHY THIS EXISTS. scripts/test-synthesis-mtls-hop.sh passed 9/9 against a
 * deployment where synthesis could not work at all. It drives `curl --cert --key`
 * from inside the kb container, so it proves the SIDECAR accepts a correct client --
 * and says nothing about whether the kb's own HTTP client presents one. It did not.
 *
 * agent_http kept a single SSL_CTX with the system trust store and no client
 * certificate, and SYNTHESIS_CA_FILE / SYNTHESIS_CERT_FILE / SYNTHESIS_KEY_FILE were
 * read by no line of code in the tree. Every curator call died in the handshake with
 * "tlsv1 alert unknown ca", reported to the operator as `provider HTTP -1` on a
 * permanently failed job, and logged as "TCP connect failed" -- on a connection whose
 * TCP connect had succeeded.
 *
 * So this test drives agent_http_post itself, against an OpenSSL server that demands
 * a client certificate, using material minted by the same issuer the kb uses in
 * production (kb_synthesis_identity_ensure). Three properties, in order of what they
 * would have caught:
 *
 *   1. with the three variables set, the request SUCCEEDS   (the bug: it did not)
 *   2. without them, it FAILS                               (proves 1 is not vacuous)
 *   3. with them pointing at a different port, it FAILS     (the identity is bound to
 *      the sidecar, and is not offered to every host the kb talks to)
 */
#include "agent_exec.h"
#include "kb_synthesis_identity.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[512];
/* kb_synthesis_identity_ensure() writes into <data_dir>/synthesis-tls, not into the
 * data dir itself; g_dir is that subdirectory. */
static char g_dir[600];
static int g_listen_fd = -1;
static int g_port;

/* One-shot TLS server: accept exactly one connection, require a client certificate
 * against our CA, answer 200. Mirrors what stunnel does in front of llama-server
 * (verifyChain = yes), which is the peer behaviour the client has to satisfy. */
static void *server_thread(void *arg)
{
   (void)arg;
   char ca[600], cert[600], key[600];
   snprintf(ca, sizeof(ca), "%s/ca.pem", g_dir);
   snprintf(cert, sizeof(cert), "%s/server.pem", g_dir);
   snprintf(key, sizeof(key), "%s/server.key", g_dir);

   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   assert(ctx);
   assert(SSL_CTX_use_certificate_chain_file(ctx, cert) == 1);
   assert(SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) == 1);
   assert(SSL_CTX_load_verify_file(ctx, ca) == 1);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

   /* accept() must not block forever: two of the three cases below are SUPPOSED to
    * fail, and a test that hangs on its own success condition is worse than one that
    * fails. SO_RCVTIMEO on a listening socket bounds accept(). */
   struct timeval tv = {20, 0};
   setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   int fd = accept(g_listen_fd, NULL, NULL);
   if (fd < 0)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL *ssl = SSL_new(ctx);
   SSL_set_fd(ssl, fd);
   if (SSL_accept(ssl) == 1)
   {
      char buf[4096];
      SSL_read(ssl, buf, sizeof(buf)); /* request; content is irrelevant here */
      const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: 2\r\n\r\n{}";
      SSL_write(ssl, resp, (int)strlen(resp));
   }
   SSL_shutdown(ssl);
   SSL_free(ssl);
   close(fd);
   SSL_CTX_free(ctx);
   return NULL;
}

/* Bind and pick the port WITHOUT serving yet: the ordering case below has to know
 * the endpoint (and so the port) before any certificate exists. */
static void bind_listener(void)
{
   /* DUAL-STACK, and that is load-bearing. The client connects to the NAME
    * "localhost" -- it has to, because the server certificate's SAN is DNS:localhost
    * and an IP literal would fail hostname verification. On a host where localhost
    * resolves to ::1 first, an AF_INET listener is simply not there, and the test
    * fails with "TCP connect failed" while claiming something about TLS. */
   g_listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
   assert(g_listen_fd >= 0);
   int one = 1;
   int off = 0;
   setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   setsockopt(g_listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

   struct sockaddr_in6 sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin6_family = AF_INET6;
   sa.sin6_addr = in6addr_any;
   sa.sin6_port = 0; /* ephemeral: a fixed port makes this flaky under parallel CI */
   assert(bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
   assert(listen(g_listen_fd, 1) == 0);

   socklen_t len = sizeof(sa);
   assert(getsockname(g_listen_fd, (struct sockaddr *)&sa, &len) == 0);
   g_port = ntohs(sa.sin6_port);
}

static pthread_t spawn_server(void)
{
   pthread_t t;
   assert(pthread_create(&t, NULL, server_thread, NULL) == 0);
   return t;
}

static pthread_t start_server(void)
{
   bind_listener();
   return spawn_server();
}

static void set_identity_env(int port)
{
   char v[700];
   snprintf(v, sizeof(v), "https://localhost:%d/v1", port);
   setenv("SYNTHESIS_ENDPOINT", v, 1);
   snprintf(v, sizeof(v), "%s/ca.pem", g_dir);
   setenv("SYNTHESIS_CA_FILE", v, 1);
   snprintf(v, sizeof(v), "%s/client.pem", g_dir);
   setenv("SYNTHESIS_CERT_FILE", v, 1);
   snprintf(v, sizeof(v), "%s/client.key", g_dir);
   setenv("SYNTHESIS_KEY_FILE", v, 1);
}

static int post_once(void)
{
   char url[700];
   snprintf(url, sizeof(url), "https://localhost:%d/v1/chat/completions", g_port);
   char *resp = NULL;
   int status = agent_http_post(url, NULL, "{}", &resp, 15000, NULL);
   free(resp);
   return status;
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-synth-mtls-%d", (int)getpid());
   assert(mkdir(g_home, 0700) == 0);

   snprintf(g_dir, sizeof(g_dir), "%s/synthesis-tls", g_home);

   /* 1. THE MATERIAL APPEARS AFTER agent_http_init(), which is the deployment's
    *    actual order and not a contrived one. In the kb, agent_http_init() runs
    *    during startup and kb_synthesis_identity_ensure() mints these files later in
    *    the same startup -- 81 seconds later on a first boot, with Postgres coming up
    *    in between. An eager load read three absent files, gave up, and disabled
    *    synthesis for the life of the process; it looked fine on every restart
    *    afterwards, because by then the material was on disk.
    *
    *    So: configure the endpoint, initialise, and only THEN issue the identity. */
   bind_listener();
   set_identity_env(g_port);
   agent_http_init();

   /* The SAME issuer the kb uses in production. A hand-rolled CA here could drift
    * from what kb_synthesis_identity_ensure actually emits -- and the bugs on this
    * hop have all been about material and ordering, not about protocol. */
   assert(kb_synthesis_identity_ensure(g_home, "localhost") == 0);

   pthread_t t = spawn_server();
   int status = post_once();
   pthread_join(t, NULL);
   close(g_listen_fd);
   if (status != 200)
      fprintf(stderr, "identity issued after init: expected 200, got %d\n", status);
   assert(status == 200);
   agent_http_cleanup();

   /* 2. Without it, the same request must fail. Otherwise (1) proves nothing: a
    *    server that did not really demand a certificate would pass it too. */
   t = start_server();
   unsetenv("SYNTHESIS_CA_FILE");
   unsetenv("SYNTHESIS_CERT_FILE");
   unsetenv("SYNTHESIS_KEY_FILE");
   agent_http_init();
   status = post_once();
   pthread_join(t, NULL);
   close(g_listen_fd);
   assert(status < 200 || status >= 300);
   agent_http_cleanup();

   /* 3. The identity belongs to the endpoint SYNTHESIS_ENDPOINT names, and to
    *    nothing else. Point the endpoint at a different port and the certificate
    *    must not be offered here -- otherwise the kb would hand its identity to
    *    every host it talks to, which is a worse bug than the one being fixed. */
   t = start_server();
   set_identity_env(g_port + 1); /* deliberately NOT the port we are about to call */
   agent_http_init();
   status = post_once();
   pthread_join(t, NULL);
   close(g_listen_fd);
   assert(status < 200 || status >= 300);
   agent_http_cleanup();

   printf("test_synthesis_mtls_client: ok\n");
   return 0;
}
