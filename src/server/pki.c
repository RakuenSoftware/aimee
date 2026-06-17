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
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
