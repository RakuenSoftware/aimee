/* server_tls.c: see server_tls.h. Plain server-TLS context + handshake. */
#include "server_tls.h"
#include "server_conn_io.h" /* register/clear the per-conn SSL on the I/O shim */
#include "config.h"         /* config_default_dir */
#include "aimee.h"          /* MAX_PATH_LEN */
#include "log.h"

#include <openssl/err.h>
#include <pthread.h>
#include <stdio.h>

static SSL_CTX *g_ctx = NULL;
static pthread_mutex_t g_ctx_mu = PTHREAD_MUTEX_INITIALIZER;

int server_tls_init(const char *cert_path, const char *key_path)
{
   if (!cert_path || !cert_path[0] || !key_path || !key_path[0])
      return -1;
   pthread_mutex_lock(&g_ctx_mu);
   if (g_ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      return 0;
   }
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   if (!ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      aimee_log(LOG_WARN, "server.tls", "SSL_CTX_new failed");
      return -1;
   }
   /* Modern floor; no client-cert verification (plain server TLS — the bearer is
    * the caller's authentication, the TLS is the channel). */
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1 ||
       SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(ctx) != 1)
   {
      aimee_log(LOG_WARN, "server.tls", "failed to load TLS cert/key (%s, %s)", cert_path,
                key_path);
      SSL_CTX_free(ctx);
      pthread_mutex_unlock(&g_ctx_mu);
      return -1;
   }
   g_ctx = ctx;
   pthread_mutex_unlock(&g_ctx_mu);
   aimee_log(LOG_INFO, "server.tls", "native TLS enabled (cert %s)", cert_path);
   return 0;
}

int server_tls_enabled(void)
{
   return g_ctx != NULL;
}

SSL *server_tls_accept(int fd)
{
   SSL_CTX *ctx = g_ctx;
   if (!ctx || fd < 0)
      return NULL;
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
      return NULL;
   SSL_set_fd(ssl, fd);
   if (SSL_accept(ssl) != 1)
   {
      SSL_free(ssl);
      return NULL;
   }
   return ssl;
}

int server_tls_init_default(void)
{
   char cert[MAX_PATH_LEN], key[MAX_PATH_LEN];
   snprintf(cert, sizeof(cert), "%s/tls/server.crt", config_default_dir());
   snprintf(key, sizeof(key), "%s/tls/server.key", config_default_dir());
   return server_tls_init(cert, key);
}

SSL *server_tls_begin(int fd)
{
   SSL *ssl = server_tls_accept(fd);
   if (ssl)
      server_conn_io_set_ssl(fd, ssl);
   return ssl;
}

void server_tls_end(int fd, SSL *ssl)
{
   if (!ssl)
      return;
   server_conn_io_clear(fd);
   SSL_shutdown(ssl);
   SSL_free(ssl);
}
