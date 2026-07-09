/* pki.c: see pki.h. aimee's self-generated client-cert CA + issuance/revocation. */
#include "pki.h"
#include "vault_service.h"   /* seal/inject the CA key */
#include "vault_principal.h" /* vault_principal_cert_sanitize */
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

#define PKI_CA_AGENT     "__pki_ca__" /* vault agent name under which the CA key is sealed */
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

/* --- DB1 (caller holds no lock; sqlite is serialized) --- */
static void db_ensure_tables(void)
{
   sqlite3 *db = db1_conn();
   if (db)
      sqlite3_exec(db,
                   "CREATE TABLE IF NOT EXISTS pki_certs(serial TEXT PRIMARY KEY, cn TEXT NOT NULL,"
                   " issued_at INTEGER, expires_at INTEGER, revoked INTEGER NOT NULL DEFAULT 0)",
                   NULL, NULL, NULL);
}

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
   sqlite3 *db = db1_conn();
   if (!db)
      return;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT serial FROM pki_certs WHERE revoked=1", -1, &st, NULL) !=
       SQLITE_OK)
      return;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *s = (const char *)sqlite3_column_text(st, 0);
      if (s)
         snapshot_add(s);
   }
   sqlite3_finalize(st);
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

/* Generate a self-signed EC P-256 server certificate at (cert_path, key_path)
 * when neither already exists. Makes native TLS zero-config: with
 * aimee.api.tls_port set but no operator cert present, the server provisions its
 * own rather than fall back to a plaintext-only listener. The channel is secured
 * regardless of trust; clients pin the cert or pass AIMEE_TLS_INSECURE=1. The key
 * is written 0600; the cert (public) is world-readable. Returns 0 if a usable
 * cert exists (already present or freshly written), -1 on failure (logged). */
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

int pki_ensure_self_signed_server_cert(const char *cert_path, const char *key_path)
{
   if (!cert_path || !cert_path[0] || !key_path || !key_path[0])
      return -1;

   /* Desired SAN for this boot: a STABLE CN + AIMEE_TLS_EXTRA_SAN. Computed up
    * front so it can be compared against what a previously-provisioned cert used.
    * The CN comes from pki_resolve_server_cn (operator identity preferred over the
    * OS hostname) — so a container recreate, which changes gethostname() to a fresh
    * per-container ID, no longer shifts the SAN and rotates the cert out from under
    * pinned clients. */
   char cn[256];
   pki_resolve_server_cn(cn, sizeof(cn));
   char san[1024];
   pki_build_server_san(cn, getenv("AIMEE_TLS_EXTRA_SAN"), san, sizeof(san));

   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s/tls", config_default_dir());
   char san_path[MAX_PATH_LEN];
   snprintf(san_path, sizeof(san_path), "%s/server.san", dir);

   struct stat st;
   if (stat(cert_path, &st) == 0 && stat(key_path, &st) == 0)
   {
      /* A cert already exists. Never touch an operator-mounted cert — identified
       * by the ABSENCE of our server.san sidecar. For a cert WE provisioned
       * (sidecar present), regenerate only when the desired SAN changed (e.g.
       * AIMEE_TLS_EXTRA_SAN was set to add the LAN IP/hostname a thin client
       * reaches us at) so pin+verify works without an operator manually deleting
       * the cert. Unchanged SAN → keep it. */
      FILE *sf = fopen(san_path, "r");
      if (!sf)
         return 0; /* operator / pre-sidecar cert: never overwrite */
      char prev[1024] = "";
      if (fgets(prev, sizeof(prev), sf))
         prev[strcspn(prev, "\r\n")] = '\0';
      fclose(sf);
      if (strcmp(prev, san) == 0)
         return 0; /* provisioned cert, SAN unchanged: keep it */
      aimee_log(LOG_INFO, "pki", "server TLS SAN changed; regenerating self-signed cert (SAN=%s)",
                san);
      /* fall through: the O_TRUNC / "w" writes below overwrite the old cert+key */
   }

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
      int kfd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (kfd >= 0)
      {
         FILE *kf = fdopen(kfd, "w");
         if (kf)
         {
            fputs(key_pem, kf);
            if (fclose(kf) == 0)
            {
               FILE *cf = fopen(cert_path, "w");
               if (cf)
               {
                  fputs(cert_pem, cf);
                  if (fclose(cf) == 0)
                     ok = 1;
               }
            }
         }
         else
            close(kfd);
      }
      OPENSSL_cleanse(key_pem, strlen(key_pem));
      free(key_pem);
      free(cert_pem);
      if (ok)
      {
         rc = 0;
         /* Record the SAN we baked in, marking this as a server-provisioned cert
          * and enabling regeneration when AIMEE_TLS_EXTRA_SAN/hostname later change. */
         FILE *snf = fopen(san_path, "w");
         if (snf)
         {
            fputs(san, snf);
            fputc('\n', snf);
            fclose(snf);
         }
         aimee_log(LOG_INFO, "pki", "generated self-signed server TLS cert (CN=%s) at %s", cn,
                   cert_path);
      }
      else
         aimee_log(LOG_ERROR, "pki", "failed to write self-signed server TLS cert to %s/%s",
                   cert_path, key_path);
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
   db_ensure_tables(); /* inside the lock: table exists before snapshot_load reads it */
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
   if (!cn || !vault_principal_cert_sanitize(cn, san, sizeof(san)) || !cert_pem || !key_pem ||
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
         snprintf(cert_pem, cert_len, "%s", cp);
         snprintf(key_pem, key_len, "%s", kp);
         snprintf(serial_out, serial_len, "%s", serial);
         sqlite3 *db = db1_conn();
         if (db)
         {
            sqlite3_stmt *stm = NULL;
            /* Plain INSERT (not REPLACE): a (2^-128, impossible) serial collision
             * must never overwrite/reset an existing cert's revocation row. */
            if (sqlite3_prepare_v2(db,
                                   "INSERT INTO pki_certs(serial,cn,issued_at,"
                                   "expires_at,revoked) VALUES(?,?,?,?,0)",
                                   -1, &stm, NULL) == SQLITE_OK)
            {
               long now = (long)time(NULL);
               sqlite3_bind_text(stm, 1, serial, -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(stm, 2, san, -1, SQLITE_TRANSIENT);
               sqlite3_bind_int64(stm, 3, now);
               sqlite3_bind_int64(stm, 4, now + (long)validity_days * 24 * 3600);
               sqlite3_step(stm);
               sqlite3_finalize(stm);
            }
         }
         rc = 0;
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

int pki_revoke(const char *serial)
{
   if (!serial || !serial[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (db)
   {
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(db, "UPDATE pki_certs SET revoked=1 WHERE serial=?", -1, &st, NULL) ==
          SQLITE_OK)
      {
         sqlite3_bind_text(st, 1, serial, -1, SQLITE_TRANSIENT);
         sqlite3_step(st);
         sqlite3_finalize(st);
      }
   }
   pthread_mutex_lock(&g_mu);
   snapshot_add(serial);
   pthread_mutex_unlock(&g_mu);
   aimee_log(LOG_INFO, "pki.audit", "revoked client cert serial=%s", serial);
   return 0;
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

int pki_list(void (*cb)(void *ctx, const char *serial, const char *cn, long issued_at,
                        long expires_at, int revoked),
             void *ctx)
{
   if (!cb)
      return -1;
   db_ensure_tables();
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT serial,cn,issued_at,expires_at,revoked FROM pki_certs ORDER BY issued_at DESC",
           -1, &st, NULL) != SQLITE_OK)
      return -1;
   int n = 0;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      cb(ctx, (const char *)sqlite3_column_text(st, 0), (const char *)sqlite3_column_text(st, 1),
         (long)sqlite3_column_int64(st, 2), (long)sqlite3_column_int64(st, 3),
         sqlite3_column_int(st, 4));
      n++;
   }
   sqlite3_finalize(st);
   return n;
}
