/* OpenSSL implementation of the core native TLS client contract. */
#include <aimee/core/connection/native_tls.h>
#include <aimee/core/connection/socket.h>
#include <aimee/core/connection/tls_openssl.h>
#include "native_tls_internal.h"
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
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

/* Present <aimee_home>/tls/client.{crt,key} as this client's certificate for
 * mutual TLS, when both files exist. Absent => plain client TLS (the server may
 * not require a client cert). Once either identity file is configured, unsafe
 * permissions, malformed PEM, or a key mismatch fail the connection closed. */
/* Process-local; see aimee_tls_suppress_client_cert in aimee_tls.h. */
static int g_suppress_client_cert = 0;

void aimee_tls_suppress_client_cert(int on)
{
   g_suppress_client_cert = on ? 1 : 0;
}

static int aimee_tls_present_client_cert(SSL_CTX *ctx)
{
   /* Checked ahead of the eligibility gate on purpose: the caller is probing
    * whether the STORED identity is what the server refuses, so a refused-but-
    * present cert must not fail the probe closed before it can be tested. */
   if (g_suppress_client_cert)
      return 0;
   char crt[600], key[600];
   int eligible = aimee_tls_client_cert_eligible(aimee_core_native_tls_home(), crt, sizeof(crt),
                                                 key, sizeof(key));
   if (eligible == 0)
      return 0;
   if (eligible < 0)
      return -1;
   if (aimee_core_tls_use_identity_files(ctx, crt, key) != 0)
      return -1;
   return 0;
}

/* Resolve <aimee_home>/remote-ca.pem — the pinned server certificate written by
 * `aimee remote set/trust`. Returns 1 (and fills |out|) when the file exists,
 * else 0. Trusting this file lets a self-signed/private server verify fully
 * without disabling verification (AIMEE_TLS_INSECURE). */
static int pinned_ca_path(char *out, size_t n)
{
   const char *home = aimee_core_native_tls_home();
   if (!home || !*home || !out || n == 0)
      return 0;
   snprintf(out, n, "%s/remote-ca.pem", home);
   struct stat st;
   return (stat(out, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

/* Load the pinned certificate itself (first PEM cert in remote-ca.pem), or NULL. */
static X509 *pinned_cert_load(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
   fclose(f);
   return cert;
}

/* Pinned-mode verify: the identity is an EXACT leaf match against the TOFU-
 * pinned certificate (SSH known_hosts semantics). Hostname/SAN is deliberately
 * NOT consulted: a containerized/NAT'd server cannot know its reachable
 * host address at cert-mint time, so its self-signed cert routinely lacks the
 * SAN the client dials — while the byte-exact pin is already a STRONGER
 * identity than any name match (a MITM cannot present the pinned cert without
 * its private key; any other cert, even one validly chaining to a public CA,
 * fails the compare). Chain/time preverify results are likewise subordinate to
 * the pin at the leaf, exactly like an SSH host key. */
static int pin_leaf_verify_cb(int preverify_ok, X509_STORE_CTX *xctx)
{
   (void)preverify_ok;
   if (X509_STORE_CTX_get_error_depth(xctx) > 0)
      return 1; /* only the leaf decides in pinned mode */
   SSL *ssl = X509_STORE_CTX_get_ex_data(xctx, SSL_get_ex_data_X509_STORE_CTX_idx());
   X509 *pin = ssl ? (X509 *)SSL_get_app_data(ssl) : NULL;
   X509 *leaf = X509_STORE_CTX_get_current_cert(xctx);
   return (pin && leaf && X509_cmp(pin, leaf) == 0) ? 1 : 0;
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   SSL_CTX *ctx = aimee_core_tls_client_context();
   if (!ctx)
      return NULL;

   int insecure = tls_insecure();
   X509 *pinned = NULL;
   if (!insecure)
   {
      char pin[600];
      if (pinned_ca_path(pin, sizeof(pin)))
      {
         /* Strict pin: a recorded cert means a self-signed/private server, so
          * trust ONLY that exact cert and NOT the system store — a mis-issued or
          * compromised public-CA cert for the same host is then still rejected.
          * A pin that won't load fails the connection CLOSED rather than silently
          * widening trust back to the system store. Identity is decided by
          * pin_leaf_verify_cb (exact leaf match); hostname/SAN is not consulted
          * in pinned mode — see the callback's rationale. */
         pinned = pinned_cert_load(pin);
         if (!pinned || aimee_core_tls_trust_file(ctx, pin) != 0)
         {
            if (pinned)
               X509_free(pinned);
            SSL_CTX_free(ctx);
            return NULL;
         }
         SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, pin_leaf_verify_cb);
      }
      else
      {
         SSL_CTX_set_default_verify_paths(ctx); /* publicly-trusted servers */
         SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
      }
   }

   /* A configured identity is security policy, not a hint. Refuse to connect if
    * it is unsafe, malformed, or mismatched; silently continuing would turn a
    * broken mTLS deployment into bearer-only TLS during the optional ramp. */
   if (aimee_tls_present_client_cert(ctx) != 0)
   {
      if (pinned)
         X509_free(pinned);
      SSL_CTX_free(ctx);
      return NULL;
   }

   SSL *ssl = aimee_core_tls_client_session_new(ctx, fd, host, !insecure && !pinned);
   if (!ssl)
   {
      if (pinned)
         X509_free(pinned);
      SSL_CTX_free(ctx);
      return NULL;
   }
   if (pinned)
      SSL_set_app_data(ssl, pinned); /* consumed by pin_leaf_verify_cb */
   int connected = aimee_core_tls_handshake_client(ssl) == 0;
   if (pinned)
   {
      SSL_set_app_data(ssl, NULL); /* the handshake (and its verify) is done */
      X509_free(pinned);
   }
   if (!connected)
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

int aimee_tls_fetch_peer_cert(const char *host, const char *port, char **pem_out, char *fp_out,
                              size_t fp_n)
{
   if (pem_out)
      *pem_out = NULL;
   if (fp_out && fp_n)
      fp_out[0] = '\0';
   if (!host || !*host || !port || !*port || !pem_out)
      return -1;

   int fd = aimee_core_socket_connect(host, port, 10000);
   if (fd < 0)
      return -1;

   SSL_CTX *ctx = aimee_core_tls_client_context();
   if (!ctx)
   {
      aimee_core_socket_close(fd);
      return -1;
   }
   /* Deliberately NO verification: we are FETCHING the cert to show its
    * fingerprint and pin it (trust-on-first-use), not trusting it yet. The
    * caller is expected to surface the fingerprint for out-of-band check. */
   SSL *ssl = aimee_core_tls_client_session_new(ctx, fd, host, 0);
   if (!ssl)
   {
      SSL_CTX_free(ctx);
      aimee_core_socket_close(fd);
      return -1;
   }

   int rc = -1;
   if (aimee_core_tls_handshake_client(ssl) == 0)
   {
      X509 *cert = SSL_get1_peer_certificate(ssl);
      if (cert)
      {
         BIO *bio = BIO_new(BIO_s_mem());
         if (bio && PEM_write_bio_X509(bio, cert) == 1)
         {
            char *data = NULL;
            long len = BIO_get_mem_data(bio, &data);
            if (len > 0 && data)
            {
               char *pem = malloc((size_t)len + 1);
               if (pem)
               {
                  memcpy(pem, data, (size_t)len);
                  pem[len] = '\0';
                  *pem_out = pem;
                  rc = 0;
               }
            }
         }
         if (bio)
            BIO_free(bio);
         if (rc == 0 && fp_out && fp_n)
         {
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int mdlen = 0;
            if (X509_digest(cert, EVP_sha256(), md, &mdlen) == 1)
            {
               size_t o = 0;
               for (unsigned int i = 0; i < mdlen && o + 4 < fp_n; i++)
                  o += (size_t)snprintf(fp_out + o, fp_n - o, i ? ":%02X" : "%02X", md[i]);
            }
         }
         X509_free(cert);
      }
   }
   aimee_core_tls_session_free(ssl);
   SSL_CTX_free(ctx);
   aimee_core_socket_close(fd); /* SSL_set_fd does not take ownership of the fd */
   return rc;
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   return t ? aimee_core_tls_write_all(t->ssl, buf, len) : -1;
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   return t ? aimee_core_tls_read(t->ssl, buf, len) : -1;
}

void aimee_tls_free(aimee_tls_t *t)
{
   if (!t)
      return;
   if (t->ssl)
   {
      aimee_core_tls_session_free(t->ssl);
   }
   if (t->ctx)
      SSL_CTX_free(t->ctx);
   free(t);
}
