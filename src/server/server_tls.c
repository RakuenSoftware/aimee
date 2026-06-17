/* server_tls.c: see server_tls.h. Server-TLS context + handshake, incl. mTLS
 * client-cert verification + revocation. */
#include "server_tls.h"
#include "server_conn_io.h" /* register/clear the per-conn SSL on the I/O shim */
#include "config.h"         /* config_default_dir, config_load */
#include "aimee.h"          /* MAX_PATH_LEN */
#include "pki.h"            /* pki_ca_ensure, pki_is_revoked */
#include "log.h"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

static SSL_CTX *g_ctx = NULL;
static pthread_mutex_t g_ctx_mu = PTHREAD_MUTEX_INITIALIZER;

/* mTLS verify callback: OpenSSL has already checked the chain/validity
 * (preverify_ok). Additionally reject a revoked leaf (depth 0) by consulting the
 * in-memory revocation snapshot — no DB query in the handshake path. */
static int mtls_verify_cb(int preverify_ok, X509_STORE_CTX *ctx)
{
   if (!preverify_ok)
      return 0; /* chain/time/CA failure -> reject */
   if (X509_STORE_CTX_get_error_depth(ctx) != 0)
      return 1; /* only the leaf carries the client serial */
   X509 *cert = X509_STORE_CTX_get_current_cert(ctx);
   if (!cert)
      return 1;
   ASN1_INTEGER *sn = X509_get_serialNumber(cert);
   BIGNUM *bn = sn ? ASN1_INTEGER_to_BN(sn, NULL) : NULL;
   char *hex = bn ? BN_bn2hex(bn) : NULL;
   int revoked = hex ? pki_is_revoked(hex) : 0;
   if (hex)
      OPENSSL_free(hex);
   if (bn)
      BN_free(bn);
   if (revoked)
   {
      X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_REVOKED);
      return 0;
   }
   return 1;
}

int server_tls_init(const char *cert_path, const char *key_path, int mtls_mode,
                    const char *client_ca_path)
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
   /* The private key authenticates the server; warn loudly if it is readable by
    * group/other (defense-in-depth — the operator should 0600 it). */
   struct stat kst;
   if (stat(key_path, &kst) == 0 && (kst.st_mode & 077))
      aimee_log(LOG_WARN, "server.tls",
                "TLS private key %s is group/world-readable (mode %o) — "
                "restrict it to 0600",
                key_path, kst.st_mode & 0777);

   if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1 ||
       SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(ctx) != 1)
   {
      aimee_log(LOG_WARN, "server.tls", "failed to load TLS cert/key (%s, %s): %s", cert_path,
                key_path, ERR_error_string(ERR_get_error(), NULL));
      SSL_CTX_free(ctx);
      pthread_mutex_unlock(&g_ctx_mu);
      return -1;
   }

   /* mTLS: verify client certs against aimee's client CA. The chain is verified
    * by OpenSSL (standard callback); the CN -> principal mapping + revocation
    * happen later in the identity layer. `required` refuses a handshake with no
    * client cert; `optional` requests one but allows bearer-only. A CA that won't
    * load disables mTLS (logged) rather than silently accepting unverified certs. */
   if (mtls_mode > 0)
   {
      char ca[MAX_PATH_LEN];
      if (client_ca_path && client_ca_path[0])
         snprintf(ca, sizeof(ca), "%s", client_ca_path);
      else
         snprintf(ca, sizeof(ca), "%s/tls/client-ca.crt", config_default_dir());
      if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1)
      {
         aimee_log(LOG_WARN, "server.tls",
                   "mtls enabled but client CA %s not loadable: %s — "
                   "mTLS DISABLED (client certs not verified)",
                   ca, ERR_error_string(ERR_get_error(), NULL));
      }
      else
      {
         int vflags = SSL_VERIFY_PEER;
         if (mtls_mode >= 2)
            vflags |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
         SSL_CTX_set_verify(ctx, vflags, mtls_verify_cb);
         aimee_log(LOG_INFO, "server.tls", "mTLS %s (client CA %s)",
                   mtls_mode >= 2 ? "required" : "optional", ca);
      }
   }

   g_ctx = ctx;
   pthread_mutex_unlock(&g_ctx_mu);
   aimee_log(LOG_INFO, "server.tls", "native TLS enabled (cert %s)", cert_path);
   return 0;
}

int server_tls_peer_identity(SSL *ssl, char *cn_out, size_t cn_len, char *serial_out,
                             size_t serial_len)
{
   if (cn_out && cn_len)
      cn_out[0] = '\0';
   if (serial_out && serial_len)
      serial_out[0] = '\0';
   if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK)
      return 0;
   X509 *cert = SSL_get_peer_certificate(ssl);
   if (!cert)
      return 0; /* bearer-only TLS conn: no client cert */

   int ok = 0;
   X509_NAME *subj = X509_get_subject_name(cert);
   if (subj && cn_out && cn_len &&
       X509_NAME_get_text_by_NID(subj, NID_commonName, cn_out, (int)cn_len) > 0)
      ok = 1;
   if (ok && serial_out && serial_len)
   {
      ASN1_INTEGER *sn = X509_get_serialNumber(cert);
      BIGNUM *bn = sn ? ASN1_INTEGER_to_BN(sn, NULL) : NULL;
      char *hex = bn ? BN_bn2hex(bn) : NULL;
      if (hex)
      {
         snprintf(serial_out, serial_len, "%s", hex);
         OPENSSL_free(hex);
      }
      if (bn)
         BN_free(bn);
   }
   X509_free(cert);
   return ok;
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
   /* Bound the handshake (and subsequent blocking reads) so a stalled peer cannot
    * pin a per-conn worker thread indefinitely (the conn cap is small). */
   struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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
   config_t cfg;
   config_load(&cfg);
   /* When mTLS is on, ensure aimee's client CA exists (create-or-load) and the
    * revocation snapshot is loaded BEFORE server_tls_init loads the client CA
    * file and the verify callback starts consulting the snapshot. */
   if (cfg.server_api_mtls > 0)
      pki_ca_ensure();
   return server_tls_init(cert, key, cfg.server_api_mtls, cfg.server_api_mtls_client_ca);
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
