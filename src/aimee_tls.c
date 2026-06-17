/* aimee_tls.c: OpenSSL TLS client wrapper (WITH_TLS builds only). */
#include "aimee_tls.h"
#include "aimee_home.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct aimee_tls
{
   SSL_CTX *ctx;
   SSL *ssl;
};

static int tls_insecure(void)
{
   const char *v = getenv("AIMEE_TLS_INSECURE");
   return v && *v && strcmp(v, "0") != 0;
}

/* Decide whether client mTLS material under |home| should be presented, and
 * fill the resolved crt/key paths. Returns:
 *    1  -> present (both files exist and the key is owner-only)
 *    0  -> none (no key/cert configured)
 *   -1  -> refused: the private key is group/world-readable (fail closed — the
 *          key is this client's identity material, never present a loose one)
 * Pure (no OpenSSL/network); exposed for unit testing the fail-closed gate. */
int aimee_tls_client_cert_eligible(const char *home, char *crt, size_t crt_n, char *key,
                                   size_t key_n)
{
   if (!home || !*home || !crt || !key)
      return 0;
   snprintf(crt, crt_n, "%s/tls/client.crt", home);
   snprintf(key, key_n, "%s/tls/client.key", home);
   struct stat cs;
   if (stat(crt, &cs) != 0)
      return 0; /* no client-cert material configured */
#ifndef _WIN32
   /* The key is this client's identity material: lstat it (do NOT follow a
    * symlink) and require a plain, owner-only regular file. This refuses a
    * group/world-accessible key and a symlink swapped in for one. (A writer to
    * your own 0700 tls/ dir is already privileged and can read the key outright,
    * so this gate targets accidental loose perms + symlink tricks, not an
    * attacker who already controls the directory — full TOCTOU-atomic loading
    * is therefore out of scope.) */
   struct stat ks;
   if (lstat(key, &ks) != 0)
      return 0;
   if (!S_ISREG(ks.st_mode))
      return -1; /* symlink / special file — refuse */
   if (ks.st_mode & 077)
      return -1; /* group/world-accessible — refuse */
#else
   struct stat ks;
   if (stat(key, &ks) != 0)
      return 0;
#endif
   return 1;
}

/* Present <aimee_home>/tls/client.{crt,key} as this client's certificate for
 * mutual TLS, when both files exist. Absent => plain client TLS (the server may
 * not require a client cert). A load failure leaves the ctx cert-less and the
 * handshake then fails at the mTLS server, which is the correct signal for a
 * broken/loose client cert. */
static void aimee_tls_present_client_cert(SSL_CTX *ctx)
{
   char crt[600], key[600];
   if (aimee_tls_client_cert_eligible(aimee_home(), crt, sizeof(crt), key, sizeof(key)) != 1)
      return;
   if (SSL_CTX_use_certificate_chain_file(ctx, crt) != 1 ||
       SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(ctx) != 1)
      ERR_clear_error();
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
      return NULL;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

   int insecure = tls_insecure();
   if (!insecure)
   {
      SSL_CTX_set_default_verify_paths(ctx);
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   }

   aimee_tls_present_client_cert(ctx);

   SSL *ssl = SSL_new(ctx);
   if (!ssl)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL_set_fd(ssl, fd);
   if (host && *host)
   {
      SSL_set_tlsext_host_name(ssl, host); /* SNI */
      if (!insecure)
      {
         X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
         X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
         X509_VERIFY_PARAM_set1_host(param, host, 0);
      }
   }

   if (SSL_connect(ssl) != 1)
   {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      return NULL;
   }

   aimee_tls_t *t = calloc(1, sizeof(*t));
   if (!t)
   {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      return NULL;
   }
   t->ctx = ctx;
   t->ssl = ssl;
   return t;
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   const char *p = (const char *)buf;
   size_t off = 0;
   while (off < len)
   {
      int n = SSL_write(t->ssl, p + off, (int)(len - off));
      if (n <= 0)
         return -1;
      off += (size_t)n;
   }
   return 0;
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   int n = SSL_read(t->ssl, buf, (int)len);
   return n < 0 ? -1 : (long)n;
}

void aimee_tls_free(aimee_tls_t *t)
{
   if (!t)
      return;
   if (t->ssl)
   {
      SSL_shutdown(t->ssl);
      SSL_free(t->ssl);
   }
   if (t->ctx)
      SSL_CTX_free(t->ctx);
   free(t);
}
