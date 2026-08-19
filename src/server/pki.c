/* pki.c: see pki.h. aimee's self-generated client-cert CA + issuance/revocation. */
#include "pki.h"
#include "pki_store.h"

/* How many revoked serials the in-memory snapshot holds. */
#define PKI_SNAPSHOT_MAX 4096
#include "vault_service.h"   /* seal/inject the CA key */
#include "vault_principal.h" /* vault_principal_name_sanitize */
#include "config.h"          /* config_default_dir */
#include "db1_internal.h"    /* db1_conn */
#include "aimee.h"           /* MAX_PATH_LEN */
#include "log.h"

#include <sqlite3.h>

#include <openssl/bn.h>
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <fcntl.h> /* open (0600 key file) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>    /* gethostname, close */
#include <arpa/inet.h> /* inet_pton — classify AIMEE_TLS_EXTRA_SAN entries */

#define PKI_CA_AGENT     "__pki_ca__"     /* vault agent name under which the CA key is sealed */
#define PKI_SERVER_AGENT "__pki_server__" /* Vault-only native TLS identity key */
#define PKI_CA_CN        "aimee-client-CA"
#define PKI_CA_DAYS      3650 /* CA validity (10y) */
#define PKI_KEY_PEM_MAX  4000 /* an EC P-256 key PEM is ~240B; cap well under MAX_API_KEY_LEN */
#define PKI_CERT_PEM_MAX 8192

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static EVP_PKEY *g_ca_key = NULL; /* cached CA key (loaded once) */
static X509 *g_ca_cert = NULL;
/* In-memory revocation snapshot (hex serials). Revocations are rare; a flat
 * growable array consulted under the mutex is plenty and keeps the TLS verify
 * callback off the database. */
static char **g_revoked = NULL;
static int g_revoked_n = 0, g_revoked_cap = 0;

void pki_reset_for_test(void)
{
   pthread_mutex_lock(&g_mu);
   if (g_ca_key)
      EVP_PKEY_free(g_ca_key);
   if (g_ca_cert)
      X509_free(g_ca_cert);
   g_ca_key = NULL;
   g_ca_cert = NULL;
   for (int i = 0; i < g_revoked_n; i++)
      free(g_revoked[i]);
   free(g_revoked);
   g_revoked = NULL;
   g_revoked_n = g_revoked_cap = 0;
   pthread_mutex_unlock(&g_mu);
}

static void ca_cert_path(char *out, size_t n)
{
   snprintf(out, n, "%s/tls/client-ca.crt", config_default_dir());
}

int pki_server_tls_key_load(char *out, size_t cap)
{
   if (!out || cap < PKI_KEY_PEM_MAX)
      return -1;
   out[0] = '\0';
   vault_status_t st = vault_service_inject_api_key("", PKI_SERVER_AGENT, out, cap, time(NULL));
   if (st != VAULT_OK || !out[0])
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   return 0;
}

/* --- DB1 (caller holds no lock; sqlite is serialized) --- */

/* Add a serial to the in-memory snapshot (caller holds g_mu). */
static void snapshot_add(const char *serial)
{
   for (int i = 0; i < g_revoked_n; i++)
      if (strcmp(g_revoked[i], serial) == 0)
         return;
   if (g_revoked_n == g_revoked_cap)
   {
      int cap = g_revoked_cap ? g_revoked_cap * 2 : 16;
      char **nb = realloc(g_revoked, (size_t)cap * sizeof(char *));
      if (!nb)
         return;
      g_revoked = nb;
      g_revoked_cap = cap;
   }
   g_revoked[g_revoked_n] = strdup(serial);
   if (g_revoked[g_revoked_n])
      g_revoked_n++;
   else
      /* Out-of-memory: the revocation IS durable in DB1 and reloads on the next
       * pki_ca_ensure/snapshot_load; log so an incomplete snapshot isn't silent. */
      aimee_log(LOG_WARN, "pki",
                "revocation snapshot insert failed (serial=%s); "
                "rejection deferred to next snapshot reload",
                serial);
}

/* Reload the snapshot from DB1 (caller holds g_mu). */
static void snapshot_load(void)
{
   for (int i = 0; i < g_revoked_n; i++)
      free(g_revoked[i]);
   g_revoked_n = 0;
   /* The revoked set comes from the store; the snapshot itself is this
      process's cache of it. */
   char serials[PKI_SNAPSHOT_MAX][DB1_PKI_SERIAL_MAX];
   int n = db1_pki_revoked_serials(serials, PKI_SNAPSHOT_MAX);
   for (int i = 0; i < n; i++)
      snapshot_add(serials[i]);
}

/* --- crypto helpers --- */
static EVP_PKEY *gen_ec_key(void)
{
   return EVP_EC_gen("prime256v1"); /* P-256; OpenSSL 3.0+ */
}

static char *pem_private_key(EVP_PKEY *k)
{
   BIO *bio = BIO_new(BIO_s_mem());
   if (!bio)
      return NULL;
   char *out = NULL;
   if (PEM_write_bio_PrivateKey(bio, k, NULL, NULL, 0, NULL, NULL) == 1)
   {
      char *p = NULL;
      long n = BIO_get_mem_data(bio, &p);
      if (p && n > 0)
      {
         out = malloc((size_t)n + 1);
         if (out)
         {
            memcpy(out, p, (size_t)n);
            out[n] = '\0';
         }
      }
   }
   BIO_free(bio);
   return out;
}

static char *pem_cert(X509 *c)
{
   BIO *bio = BIO_new(BIO_s_mem());
   if (!bio)
      return NULL;
   char *out = NULL;
   if (PEM_write_bio_X509(bio, c) == 1)
   {
      char *p = NULL;
      long n = BIO_get_mem_data(bio, &p);
      if (p && n > 0)
      {
         out = malloc((size_t)n + 1);
         if (out)
         {
            memcpy(out, p, (size_t)n);
            out[n] = '\0';
         }
      }
   }
   BIO_free(bio);
   return out;
}

static int set_name_cn(X509_NAME *name, const char *cn)
{
   return X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1,
                                     0) == 1
              ? 0
              : -1;
}

static int add_ext(X509 *cert, X509 *issuer, int nid, const char *value)
{
   X509V3_CTX ctx;
   X509V3_set_ctx_nodb(&ctx);
   X509V3_set_ctx(&ctx, issuer ? issuer : cert, cert, NULL, NULL, 0);
   X509_EXTENSION *ex = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
   if (!ex)
      return -1;
   int rc = X509_add_ext(cert, ex, -1);
   X509_EXTENSION_free(ex);
   return rc == 1 ? 0 : -1;
}

/* Set a random 128-bit positive serial on the cert and write its hex form. The
 * hex is derived by reading the serial BACK off the cert via the exact same
 * ASN1_INTEGER -> BIGNUM -> BN_bn2hex path the TLS verify callback uses, so the
 * stored serial and the handshake-time serial are byte-identical (no
 * leading-zero / sign-byte skew between issuance and the revocation check). */
static int set_random_serial(X509 *cert, char *hex_out, size_t hex_len)
{
   if (hex_len)
      hex_out[0] = '\0';
   BIGNUM *bn = BN_new();
   if (!bn || !BN_rand(bn, 128, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY))
   {
      BN_free(bn);
      return -1;
   }
   ASN1_INTEGER *ai = BN_to_ASN1_INTEGER(bn, NULL);
   BN_free(bn);
   if (!ai)
      return -1;
   int set_ok = (X509_set_serialNumber(cert, ai) == 1); /* copies into the cert */
   ASN1_INTEGER_free(ai);
   if (!set_ok)
      return -1;
   BIGNUM *back = ASN1_INTEGER_to_BN(X509_get_serialNumber(cert), NULL);
   char *hex = back ? BN_bn2hex(back) : NULL;
   if (hex)
   {
      snprintf(hex_out, hex_len, "%s", hex);
      OPENSSL_free(hex);
   }
   if (back)
      BN_free(back);
   return hex_out[0] ? 0 : -1;
}

/* Strip a leading "DNS:"/"IP:" SAN type prefix, if present, returning the bare
 * host/IP. Other typed prefixes (email:/URI:) are left as-is — they are not
 * hostname candidates and the caller only tests them for IP-ness. */
static const char *san_bare(const char *tok)
{
   if (strncmp(tok, "DNS:", 4) == 0)
      return tok + 4;
   if (strncmp(tok, "IP:", 3) == 0)
      return tok + 3;
   return tok;
}

/* True if |tok| parses as an IPv4 or IPv6 literal. */
static int san_is_ip(const char *tok)
{
   unsigned char ipbuf[16];
   return inet_pton(AF_INET, tok, ipbuf) == 1 || inet_pton(AF_INET6, tok, ipbuf) == 1;
}

/* Resolve the STABLE common name / primary identity for the self-signed server
 * cert. In a container gethostname() returns the per-container ID, which changes
 * on every recreate; feeding it into the cert rotated the self-signed cert on each
 * restart and silently broke every TOFU-pinned client. Prefer an operator-declared
 * identity so the cert stays put across recreates. Precedence:
 *   1. AIMEE_TLS_CN                              — explicit override (also test hook)
 *   2. first non-IP token of AIMEE_TLS_EXTRA_SAN — the declared reachable hostname
 *   3. first token of AIMEE_TLS_EXTRA_SAN        — all-IP list: use the IP
 *   4. gethostname()                             — bare-metal fallback (unchanged)
 *   5. "aimee-server"                            — last resort
 * Writes a NUL-terminated CN into |out|. Exposed for tests. */
void pki_resolve_server_cn(char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   out[0] = '\0';

   const char *cn_env = getenv("AIMEE_TLS_CN");
   if (cn_env && *cn_env)
   {
      snprintf(out, cap, "%s", cn_env);
      return;
   }

   const char *extra = getenv("AIMEE_TLS_EXTRA_SAN");
   if (extra && *extra)
   {
      char buf[512];
      snprintf(buf, sizeof(buf), "%s", extra);
      char *save = NULL;
      const char *first = NULL;
      for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
      {
         if (!*tok)
            continue;
         const char *bare = san_bare(tok);
         if (!*bare)
            continue;
         if (!first)
            first = bare; /* remember the first entry as an all-IP fallback */
         if (!san_is_ip(bare))
         {
            snprintf(out, cap, "%s", bare); /* prefer a real hostname */
            return;
         }
      }
      if (first)
      {
         snprintf(out, cap, "%s", first);
         return;
      }
   }

   char host[256];
   if (gethostname(host, sizeof(host)) == 0 && host[0])
   {
      host[sizeof(host) - 1] = '\0';
      snprintf(out, cap, "%s", host);
      return;
   }
   snprintf(out, cap, "aimee-server");
}

void pki_build_server_san(const char *cn, const char *extra, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   /* Classify the leading CN entry: an IP literal must be typed "IP:", not "DNS:",
    * or verification against the address fails (and OpenSSL rejects DNS:<ip>). */
   const char *cnv = (cn && *cn) ? cn : "localhost";
   const char *cn_prefix = san_is_ip(cnv) ? "IP:" : "DNS:";
   int n = snprintf(out, cap, "%s%s,DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1", cn_prefix, cnv);
   if (n < 0 || (size_t)n >= cap)
   {
      out[cap - 1] = '\0';
      return;
   }
   if (!extra || !*extra)
      return;
   /* Append operator-configured extra SANs (comma/space-separated). Each entry is
    * either pre-typed ("IP:.."/"DNS:.."/"email:.."/"URI:..") or a bare host/IP
    * that we auto-classify: parses as an IP -> "IP:", else "DNS:". An entry that
    * would overflow |out| is dropped whole (never half-written). */
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", extra);
   char *save = NULL;
   for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
   {
      if (!*tok)
         continue;
      const char *prefix = "";
      if (strncmp(tok, "IP:", 3) != 0 && strncmp(tok, "DNS:", 4) != 0 &&
          strncmp(tok, "email:", 6) != 0 && strncmp(tok, "URI:", 4) != 0)
      {
         unsigned char ipbuf[16];
         prefix = (inet_pton(AF_INET, tok, ipbuf) == 1 || inet_pton(AF_INET6, tok, ipbuf) == 1)
                      ? "IP:"
                      : "DNS:";
      }
      size_t cur = strlen(out);
      int w = snprintf(out + cur, cap - cur, ",%s%s", prefix, tok);
      if (w < 0 || (size_t)w >= cap - cur)
      {
         out[cur] = '\0'; /* drop the entry that didn't fit; keep prior SANs */
         break;
      }
   }
}

/* Native server identity keys are Vault-only. `key_path` below exists solely as
 * a one-way migration source for pre-policy installs; it is never a destination. */
static int read_legacy_server_key(const char *path, char *out, size_t cap)
{
   if (!path || !path[0] || !out || cap < 2)
      return -1;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size >= cap)
   {
      close(fd);
      return -1;
   }
   size_t off = 0, want = (size_t)st.st_size;
   while (off < want)
   {
      ssize_t n = read(fd, out + off, want - off);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      off += (size_t)n;
   }
   close(fd);
   if (off != want)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   out[off] = '\0';
   return 0;
}

static EVP_PKEY *private_key_from_pem(const char *pem)
{
   BIO *bio = pem ? BIO_new_mem_buf(pem, -1) : NULL;
   EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   return key;
}

static int server_cert_matches_key(const char *cert_path, const char *key_pem)
{
   FILE *f = cert_path ? fopen(cert_path, "r") : NULL;
   X509 *cert = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;
   if (f)
      fclose(f);
   EVP_PKEY *key = private_key_from_pem(key_pem);
   int ok = cert && key && X509_check_private_key(cert, key) == 1;
   EVP_PKEY_free(key);
   X509_free(cert);
   return ok ? 0 : -1;
}

static int private_keys_equal(const char *a, const char *b)
{
   EVP_PKEY *ka = private_key_from_pem(a), *kb = private_key_from_pem(b);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
   int equal = ka && kb && EVP_PKEY_eq(ka, kb) == 1;
#else
   int equal = ka && kb && EVP_PKEY_cmp(ka, kb) == 1;
#endif
   EVP_PKEY_free(ka);
   EVP_PKEY_free(kb);
   return equal;
}

static int erase_legacy_server_key(const char *path)
{
   int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0)
   {
      close(fd);
      return -1;
   }
   unsigned char zero[512] = {0};
   off_t left = st.st_size;
   while (left > 0)
   {
      size_t chunk = left > (off_t)sizeof(zero) ? sizeof(zero) : (size_t)left;
      ssize_t n = write(fd, zero, chunk);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
      {
         close(fd);
         return -1;
      }
      left -= n;
   }
   int rc = fsync(fd) == 0 && ftruncate(fd, 0) == 0 && fsync(fd) == 0 ? 0 : -1;
   close(fd);
   if (rc == 0 && unlink(path) != 0)
      rc = -1;
   return rc;
}

int pki_ensure_self_signed_server_cert(const char *cert_path, const char *key_path)
{
   if (!cert_path || !cert_path[0] || !key_path || !key_path[0])
      return -1;

   /* An existing public cert is AUTHORITATIVE. Its matching key must already be
    * in Vault or be present in the one-time legacy migration file. */
   struct stat st;
   if (stat(cert_path, &st) == 0)
   {
      char vault_key[PKI_KEY_PEM_MAX] = "", legacy_key[PKI_KEY_PEM_MAX] = "";
      int have_vault = pki_server_tls_key_load(vault_key, sizeof(vault_key)) == 0;
      int have_legacy = read_legacy_server_key(key_path, legacy_key, sizeof(legacy_key)) == 0;
      struct stat legacy_st;
      int legacy_exists = lstat(key_path, &legacy_st) == 0;
      int rc = -1;
      if (legacy_exists && !have_legacy)
         aimee_log(LOG_ERROR, "pki", "legacy server TLS key is not a safe bounded regular file");
      else if (have_vault && server_cert_matches_key(cert_path, vault_key) != 0)
         aimee_log(LOG_ERROR, "pki", "Vault-held server TLS key does not match %s", cert_path);
      else if (have_vault && have_legacy && !private_keys_equal(vault_key, legacy_key))
         aimee_log(LOG_ERROR, "pki", "legacy server TLS key conflicts with the Vault identity");
      else if (!have_vault && (!have_legacy || server_cert_matches_key(cert_path, legacy_key) != 0))
         aimee_log(LOG_ERROR, "pki", "server TLS certificate has no matching Vault key");
      else
      {
         if (!have_vault &&
             vault_service_set_server(PKI_SERVER_AGENT, VAULT_API_KEY_CRED, legacy_key) != VAULT_OK)
            aimee_log(LOG_ERROR, "pki", "failed to seal legacy server TLS key into Vault");
         else
         {
            char verify[PKI_KEY_PEM_MAX] = "";
            if (pki_server_tls_key_load(verify, sizeof(verify)) == 0 &&
                server_cert_matches_key(cert_path, verify) == 0 &&
                (!have_legacy || erase_legacy_server_key(key_path) == 0))
            {
               rc = 0;
               aimee_log(LOG_DEBUG, "pki", "server TLS cert present at %s; Vault identity kept",
                         cert_path);
            }
            OPENSSL_cleanse(verify, sizeof(verify));
         }
      }
      OPENSSL_cleanse(vault_key, sizeof(vault_key));
      OPENSSL_cleanse(legacy_key, sizeof(legacy_key));
      return rc;
   }

   /* A key without its public certificate cannot be recovered as an identity.
    * Remove that orphan before generating anything new so a cleanup failure
    * cannot leave an on-disk credential alongside a newly sealed Vault key. */
   struct stat orphan_st;
   if (lstat(key_path, &orphan_st) == 0)
   {
      if (erase_legacy_server_key(key_path) != 0)
      {
         aimee_log(LOG_ERROR, "pki", "cannot safely erase orphaned legacy server TLS key");
         return -1;
      }
   }
   else if (errno != ENOENT)
   {
      aimee_log(LOG_ERROR, "pki", "cannot inspect legacy server TLS key path");
      return -1;
   }

   /* No cert yet — provision a self-signed one. The CN prefers an operator-declared
    * identity (pki_resolve_server_cn) over the volatile OS/container hostname. */
   char cn[256];
   pki_resolve_server_cn(cn, sizeof(cn));
   char san[1024];
   pki_build_server_san(cn, getenv("AIMEE_TLS_EXTRA_SAN"), san, sizeof(san));

   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s/tls", config_default_dir());

   EVP_PKEY *key = gen_ec_key();
   if (!key)
      return -1;
   X509 *cert = X509_new();
   if (!cert)
   {
      EVP_PKEY_free(key);
      return -1;
   }
   int rc = -1;
   char serial[64] = "";

   X509_set_version(cert, 2);
   X509_NAME *name = X509_get_subject_name(cert);
   if (set_random_serial(cert, serial, sizeof(serial)) != 0 || set_name_cn(name, cn) != 0 ||
       X509_set_issuer_name(cert, name) != 1 || !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
       !X509_gmtime_adj(X509_getm_notAfter(cert), (long)3650 * 24 * 3600) ||
       X509_set_pubkey(cert, key) != 1 ||
       add_ext(cert, NULL, NID_basic_constraints, "critical,CA:FALSE") != 0 ||
       add_ext(cert, NULL, NID_key_usage, "critical,digitalSignature,keyEncipherment") != 0 ||
       add_ext(cert, NULL, NID_ext_key_usage, "serverAuth") != 0 ||
       add_ext(cert, NULL, NID_subject_alt_name, san) != 0 ||
       X509_sign(cert, key, EVP_sha256()) == 0)
      goto done;
   {
      char *key_pem = pem_private_key(key);
      char *cert_pem = pem_cert(cert);
      if (!key_pem || !cert_pem)
      {
         free(key_pem);
         free(cert_pem);
         goto done;
      }
      mkdir(dir, 0700);
      int ok = 0;
      if (vault_service_set_server(PKI_SERVER_AGENT, VAULT_API_KEY_CRED, key_pem) == VAULT_OK)
      {
         char verify[PKI_KEY_PEM_MAX] = "";
         if (pki_server_tls_key_load(verify, sizeof(verify)) == 0 &&
             private_keys_equal(key_pem, verify))
         {
            FILE *cf = fopen(cert_path, "w");
            if (cf)
            {
               fputs(cert_pem, cf);
               if (fclose(cf) == 0 && server_cert_matches_key(cert_path, verify) == 0 &&
                   (access(key_path, F_OK) != 0 || erase_legacy_server_key(key_path) == 0))
                  ok = 1;
            }
         }
         OPENSSL_cleanse(verify, sizeof(verify));
      }
      OPENSSL_cleanse(key_pem, strlen(key_pem));
      free(key_pem);
      free(cert_pem);
      if (ok)
      {
         rc = 0;
         aimee_log(LOG_INFO, "pki", "generated self-signed server TLS cert (CN=%s) at %s", cn,
                   cert_path);
      }
      else
         aimee_log(LOG_ERROR, "pki", "failed to seal server TLS identity or write cert %s",
                   cert_path);
   }
done:
   if (key)
      EVP_PKEY_free(key);
   if (cert)
      X509_free(cert);
   return rc;
}

/* --- CA --- */
static int ca_generate_and_persist(void)
{
   EVP_PKEY *key = gen_ec_key();
   if (!key)
      return -1;
   X509 *cert = X509_new();
   if (!cert)
   {
      EVP_PKEY_free(key);
      return -1;
   }
   int rc = -1;
   char serial[64] = "";
   X509_set_version(cert, 2);
   X509_NAME *name = X509_get_subject_name(cert);
   if (set_random_serial(cert, serial, sizeof(serial)) != 0 || set_name_cn(name, PKI_CA_CN) != 0 ||
       X509_set_issuer_name(cert, name) != 1 || !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
       !X509_gmtime_adj(X509_getm_notAfter(cert), (long)PKI_CA_DAYS * 24 * 3600) ||
       X509_set_pubkey(cert, key) != 1 ||
       add_ext(cert, NULL, NID_basic_constraints, "critical,CA:TRUE") != 0 ||
       add_ext(cert, NULL, NID_key_usage, "critical,keyCertSign,cRLSign") != 0 ||
       X509_sign(cert, key, EVP_sha256()) == 0)
      goto done;

   /* Seal the CA private key in the server vault; write the cert to disk. */
   {
      char *key_pem = pem_private_key(key);
      char *cert_pem = pem_cert(cert);
      if (!key_pem || !cert_pem)
      {
         free(key_pem);
         free(cert_pem);
         goto done;
      }
      vault_status_t vs = vault_service_set_server(PKI_CA_AGENT, VAULT_API_KEY_CRED, key_pem);
      OPENSSL_cleanse(key_pem, strlen(key_pem));
      free(key_pem);
      if (vs != VAULT_OK)
      {
         aimee_log(LOG_ERROR, "pki", "failed to seal CA key in vault: %s", vault_status_str(vs));
         free(cert_pem);
         goto done;
      }
      char path[MAX_PATH_LEN], dir[MAX_PATH_LEN];
      snprintf(dir, sizeof(dir), "%s/tls", config_default_dir());
      mkdir(dir, 0700);
      ca_cert_path(path, sizeof(path));
      FILE *f = fopen(path, "w");
      if (f)
      {
         fputs(cert_pem, f);
         fclose(f);
      }
      free(cert_pem);
      if (!f)
         goto done;
   }
   g_ca_key = key;
   key = NULL;
   g_ca_cert = cert;
   cert = NULL;
   rc = 0;
   aimee_log(LOG_INFO, "pki", "generated client CA (%s)", PKI_CA_CN);
done:
   if (key)
      EVP_PKEY_free(key);
   if (cert)
      X509_free(cert);
   return rc;
}

/* Load the CA from the sealed key + cert file. Returns 0 if both loaded. */
static int ca_load(void)
{
   char key_pem[MAX_API_KEY_LEN] = "";
   if (vault_service_inject_api_key("", PKI_CA_AGENT, key_pem, sizeof(key_pem), time(NULL)) !=
           VAULT_OK ||
       !key_pem[0])
      return -1;
   BIO *kb = BIO_new_mem_buf(key_pem, -1);
   EVP_PKEY *key = kb ? PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL) : NULL;
   if (kb)
      BIO_free(kb);
   OPENSSL_cleanse(key_pem, sizeof(key_pem));
   if (!key)
      return -1;
   char path[MAX_PATH_LEN];
   ca_cert_path(path, sizeof(path));
   FILE *f = fopen(path, "r");
   X509 *cert = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;
   if (f)
      fclose(f);
   if (!cert)
   {
      EVP_PKEY_free(key);
      return -1;
   }
   g_ca_key = key;
   g_ca_cert = cert;
   return 0;
}

int pki_ca_ensure(void)
{
   pthread_mutex_lock(&g_mu);
   /* The tables are the store's to create, and every entry point there
      ensures them -- including the read snapshot_load makes below. */
   int rc = 0;
   if (!g_ca_key || !g_ca_cert)
      rc = (ca_load() == 0) ? 0 : ca_generate_and_persist();
   snapshot_load();
   pthread_mutex_unlock(&g_mu);
   return rc;
}

int pki_issue(const char *cn, int validity_days, char *cert_pem, size_t cert_len, char *key_pem,
              size_t key_len, char *serial_out, size_t serial_len)
{
   char san[VAULT_CERT_CN_MAX + 1];
   if (!cn || !vault_principal_name_sanitize(cn, san, sizeof(san)) || !cert_pem || !key_pem ||
       !serial_out)
      return -1;
   if (validity_days <= 0)
      validity_days = 90;
   if (cert_len)
      cert_pem[0] = '\0';
   if (key_len)
      key_pem[0] = '\0';
   if (serial_len)
      serial_out[0] = '\0';

   if (pki_ca_ensure() != 0)
      return -1;

   pthread_mutex_lock(&g_mu);
   int rc = -1;
   EVP_PKEY *key = gen_ec_key();
   X509 *cert = key ? X509_new() : NULL;
   char serial[64] = "";
   if (!cert)
      goto done;
   X509_set_version(cert, 2);
   if (set_random_serial(cert, serial, sizeof(serial)) != 0 ||
       set_name_cn(X509_get_subject_name(cert), san) != 0 ||
       X509_set_issuer_name(cert, X509_get_subject_name(g_ca_cert)) != 1 ||
       !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
       !X509_gmtime_adj(X509_getm_notAfter(cert), (long)validity_days * 24 * 3600) ||
       X509_set_pubkey(cert, key) != 1 ||
       add_ext(cert, g_ca_cert, NID_basic_constraints, "critical,CA:FALSE") != 0 ||
       add_ext(cert, g_ca_cert, NID_key_usage, "critical,digitalSignature") != 0 ||
       add_ext(cert, g_ca_cert, NID_ext_key_usage, "clientAuth") != 0 ||
       X509_sign(cert, g_ca_key, EVP_sha256()) == 0)
      goto done;

   {
      char *cp = pem_cert(cert), *kp = pem_private_key(key);
      if (cp && kp && strlen(cp) < cert_len && strlen(kp) < key_len)
      {
         long now = (long)time(NULL);
         if (db1_pki_cert_upsert(serial, san, now, now + (long)validity_days * 24 * 3600) == 0)
         {
            snprintf(cert_pem, cert_len, "%s", cp);
            snprintf(key_pem, key_len, "%s", kp);
            snprintf(serial_out, serial_len, "%s", serial);
            rc = 0;
         }
      }
      if (kp)
         OPENSSL_cleanse(kp, strlen(kp));
      free(cp);
      free(kp);
   }
done:
   if (key)
      EVP_PKEY_free(key);
   if (cert)
      X509_free(cert);
   pthread_mutex_unlock(&g_mu);
   if (rc == 0)
      aimee_log(LOG_INFO, "pki.audit", "issued client cert cn=%s serial=%s days=%d", san, serial,
                validity_days);
   return rc;
}

int pki_sign_csr(const char *cn, int validity_days, const char *csr_pem, char *cert_pem,
                 size_t cert_len, char *serial_out, size_t serial_len)
{
   char san[VAULT_CERT_CN_MAX + 1];
   if (!cn || !vault_principal_name_sanitize(cn, san, sizeof(san)) || !csr_pem || !cert_pem ||
       !serial_out || cert_len == 0 || serial_len == 0)
      return -1;
   if (validity_days <= 0)
      validity_days = 90;
   cert_pem[0] = serial_out[0] = '\0';
   BIO *bio = BIO_new_mem_buf(csr_pem, -1);
   X509_REQ *req = bio ? PEM_read_bio_X509_REQ(bio, NULL, NULL, NULL) : NULL;
   if (bio)
      BIO_free(bio);
   EVP_PKEY *pub = req ? X509_REQ_get_pubkey(req) : NULL;
   if (!req || !pub || X509_REQ_verify(req, pub) != 1 || pki_ca_ensure() != 0)
   {
      EVP_PKEY_free(pub);
      X509_REQ_free(req);
      return -1;
   }

   pthread_mutex_lock(&g_mu);
   int rc = -1;
   X509 *cert = X509_new();
   char serial[64] = "";
   if (!cert || X509_set_version(cert, 2) != 1 ||
       set_random_serial(cert, serial, sizeof(serial)) != 0 ||
       set_name_cn(X509_get_subject_name(cert), san) != 0 ||
       X509_set_issuer_name(cert, X509_get_subject_name(g_ca_cert)) != 1 ||
       !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
       !X509_gmtime_adj(X509_getm_notAfter(cert), (long)validity_days * 24 * 3600) ||
       X509_set_pubkey(cert, pub) != 1 ||
       add_ext(cert, g_ca_cert, NID_basic_constraints, "critical,CA:FALSE") != 0 ||
       add_ext(cert, g_ca_cert, NID_key_usage, "critical,digitalSignature") != 0 ||
       add_ext(cert, g_ca_cert, NID_ext_key_usage, "clientAuth") != 0 ||
       X509_sign(cert, g_ca_key, EVP_sha256()) == 0)
      goto done_csr;

   char *cp = pem_cert(cert);
   if (!cp || strlen(cp) >= cert_len)
   {
      free(cp);
      goto done_csr;
   }
   long now = (long)time(NULL);
   if (db1_pki_cert_upsert(serial, san, now, now + (long)validity_days * 24 * 3600) == 0)
   {
      snprintf(cert_pem, cert_len, "%s", cp);
      snprintf(serial_out, serial_len, "%s", serial);
      rc = 0;
   }
   free(cp);
done_csr:
   X509_free(cert);
   pthread_mutex_unlock(&g_mu);
   EVP_PKEY_free(pub);
   X509_REQ_free(req);
   if (rc == 0)
      aimee_log(LOG_INFO, "pki.audit", "signed client CSR cn=%s serial=%s days=%d", san, serial,
                validity_days);
   return rc;
}

int pki_is_revoked(const char *serial)
{
   if (!serial || !serial[0])
      return 0;
   int revoked = 0;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < g_revoked_n; i++)
      if (strcmp(g_revoked[i], serial) == 0)
      {
         revoked = 1;
         break;
      }
   pthread_mutex_unlock(&g_mu);
   return revoked;
}

const char *pki_cert_status_str(pki_cert_status_t s)
{
   switch (s)
   {
   case PKI_CERT_VALID:
      return "VALID";
   case PKI_CERT_REVOKED:
      return "REVOKED";
   case PKI_CERT_EXPIRED:
      return "EXPIRED";
   case PKI_CERT_UNKNOWN:
      return "UNKNOWN";
   case PKI_CERT_ERROR:
      return "ERROR";
   }
   return "ERROR";
}

/* Per-request DURABLE revocation/expiry re-check — see pki.h. A FRESH prepared
 * SELECT against DB1 on every call (never the g_revoked snapshot), mirroring
 * pki_list's DB access pattern. Precedence: a prepare/bind/step failure ->
 * PKI_CERT_ERROR (an unavailable authority must never fail open, and must be
 * distinguishable from UNKNOWN); no matching row -> PKI_CERT_UNKNOWN; a row with
 * revoked!=0 -> PKI_CERT_REVOKED; a row with expires_at>0 AND expires_at<=now ->
 * PKI_CERT_EXPIRED; else PKI_CERT_VALID. SQLite is serialized, so (as with
 * pki_list) no g_mu is held around the query. */

/* --- the public names, forwarding to the store --------------------------
 *
 * These kept their signatures so their callers did not have to change. What
 * each adds beyond the store is what belongs on this side: the in-memory
 * revocation snapshot, the audit line, and the daemon's own enum. */
int pki_revoke(const char *serial)
{
   if (!serial || !serial[0])
      return -1;
   if (db1_pki_cert_revoke(serial) != 0)
      return -1;
   pthread_mutex_lock(&g_mu);
   snapshot_add(serial);
   pthread_mutex_unlock(&g_mu);
   aimee_log(LOG_INFO, "pki.audit", "revoked client cert serial=%s", serial);
   return 0;
}

pki_cert_status_t pki_cert_check(const char *serial, long now)
{
   /* One for one with the store's answers. A caller admits a connection on
      VALID and refuses it on every other, so none of them may collapse. */
   switch (db1_pki_cert_check(serial, now))
   {
   case DB1_PKI_CERT_VALID:
      return PKI_CERT_VALID;
   case DB1_PKI_CERT_REVOKED:
      return PKI_CERT_REVOKED;
   case DB1_PKI_CERT_EXPIRED:
      return PKI_CERT_EXPIRED;
   case DB1_PKI_CERT_UNKNOWN:
      return PKI_CERT_UNKNOWN;
   default:
      return PKI_CERT_ERROR;
   }
}

int pki_mtls_note_presentation(const char *serial, long now)
{
   return db1_pki_note_presentation(serial, now);
}

int pki_mtls_ramp_init(int configured_mode)
{
   int rc = db1_pki_ramp_init(configured_mode);
   if (rc < 0)
      aimee_log(LOG_WARN, "pki.ramp", "mTLS ramp startup self-test failed; refusing mTLS startup");
   return rc;
}

int pki_mtls_ramp_ready(long now)
{
   return db1_pki_ramp_ready(now);
}

int pki_mtls_ramp_advance(long now)
{
   return db1_pki_ramp_advance(now);
}

int pki_mtls_ramp_get(int *state_out, char *hash_out, size_t hash_len, long *advanced_at_out)
{
   return db1_pki_ramp_get(state_out, hash_out, hash_len, advanced_at_out);
}

int pki_list(void (*cb)(void *ctx, const char *serial, const char *cn, long issued_at,
                        long expires_at, int revoked),
             void *ctx)
{
   /* The callback stays here: a function pointer does not cross a process
      boundary, so the rows come back and the loop runs on this side. */
   if (!cb)
      return -1;
   db1_pki_cert_t rows[PKI_SNAPSHOT_MAX];
   int n = db1_pki_cert_list(rows, PKI_SNAPSHOT_MAX);
   if (n < 0)
      return -1;
   for (int i = 0; i < n; i++)
      cb(ctx, rows[i].serial, rows[i].cn, (long)rows[i].issued_at, (long)rows[i].expires_at,
         rows[i].revoked);
   return n;
}
