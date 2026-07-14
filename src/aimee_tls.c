/* aimee_tls.c: OpenSSL TLS client wrapper (WITH_TLS builds only). */
#include "aimee_tls.h"
#include "aimee_home.h"
#include "platform_net.h"
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

/* Resolve <aimee_home>/remote-ca.pem — the pinned server certificate written by
 * `aimee remote set/trust`. Returns 1 (and fills |out|) when the file exists,
 * else 0. Trusting this file lets a self-signed/private server verify fully
 * without disabling verification (AIMEE_TLS_INSECURE). */
static int pinned_ca_path(char *out, size_t n)
{
   const char *home = aimee_home();
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
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
      return NULL;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

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
         if (!pinned || SSL_CTX_load_verify_locations(ctx, pin, NULL) != 1)
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

   aimee_tls_present_client_cert(ctx);

   SSL *ssl = SSL_new(ctx);
   if (!ssl)
   {
      if (pinned)
         X509_free(pinned);
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL_set_fd(ssl, fd);
   if (pinned)
      SSL_set_app_data(ssl, pinned); /* consumed by pin_leaf_verify_cb */
   if (host && *host)
   {
      SSL_set_tlsext_host_name(ssl, host); /* SNI */
      if (!insecure && !pinned)
      {
         X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
         X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
         /* An IP-literal host (e.g. a private server reached by address) must be
          * matched against the cert's IP-address SANs: set1_host only matches DNS
          * names/CN, so an IP would otherwise fail the name check even with a
          * valid cert. set1_ip_asc returns 1 only for a real IP literal;
          * fall back to DNS-name matching for actual hostnames. If neither arms
          * the name check, fail CLOSED — verifying the chain but not the identity
          * would accept any otherwise-valid cert. (Pinned mode skips this: the
          * exact-leaf pin IS the identity.) */
         if (X509_VERIFY_PARAM_set1_ip_asc(param, host) != 1 &&
             X509_VERIFY_PARAM_set1_host(param, host, 0) != 1)
         {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return NULL;
         }
      }
   }

   int connected = (SSL_connect(ssl) == 1);
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

   int fd = platform_net_connect(host, port, 10000);
   if (fd < 0)
      return -1;

   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
   {
      platform_net_close(fd);
      return -1;
   }
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   /* Deliberately NO verification: we are FETCHING the cert to show its
    * fingerprint and pin it (trust-on-first-use), not trusting it yet. The
    * caller is expected to surface the fingerprint for out-of-band check. */
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
   {
      SSL_CTX_free(ctx);
      platform_net_close(fd);
      return -1;
   }
   SSL_set_fd(ssl, fd);
   SSL_set_tlsext_host_name(ssl, host); /* SNI — many servers select a cert by it */

   int rc = -1;
   if (SSL_connect(ssl) == 1)
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
   SSL_free(ssl);
   SSL_CTX_free(ctx);
   platform_net_close(fd); /* SSL_set_fd does not take ownership of the fd */
   return rc;
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
