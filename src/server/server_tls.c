/* server_tls.c: see server_tls.h. Server-TLS context + handshake, incl. mTLS
 * client-cert verification + revocation. */
#include "server_tls.h"
#include "server_conn_io.h" /* register/clear the per-conn SSL on the I/O shim */
#include "config.h"         /* config_default_dir, config_load */
#include "aimee.h"          /* MAX_PATH_LEN */
#include "pki.h"            /* pki_ca_ensure, pki_is_revoked */
#include "log.h"
#include "cJSON.h"
#include <aimee/core/connection/tls_openssl.h>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static SSL_CTX *g_ctx = NULL;
static pthread_mutex_t g_ctx_mu = PTHREAD_MUTEX_INITIALIZER;
/* Serializes a live cert reload against the prepare -> durable commit -> activate
 * promotion sequence. Lock order is transition, then g_ctx_mu. */
static pthread_mutex_t g_transition_mu = PTHREAD_MUTEX_INITIALIZER;
/* Saved at init so a SIGHUP reload can re-read the SAME cert/key files (live-config-reload). */
static char g_cert_path[MAX_PATH_LEN];
static char g_key_path[MAX_PATH_LEN];
static char g_client_ca_path[MAX_PATH_LEN];
static int g_mtls_mode = 0;

/* The management listener has an intentionally separate trust domain. Unlike
 * g_ctx it is never reloaded or freed: detached HTTP workers may retain SSL
 * objects until process exit. */
static SSL_CTX *g_management_ctx = NULL;
static pthread_mutex_t g_management_ctx_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_management_cert_hash[32];
static unsigned char g_management_key_hash[32];
static unsigned char g_management_ca_hash[32];

static int exact_cert_eku(X509 *cert, int required_nid);

#define MANAGEMENT_PEM_MAX (1024U * 1024U)

typedef struct
{
   unsigned char *bytes;
   size_t len;
   unsigned char hash[32];
} captured_pem_t;

static void captured_pem_clear(captured_pem_t *pem)
{
   if (!pem)
      return;
   if (pem->bytes)
   {
      OPENSSL_cleanse(pem->bytes, pem->len);
      free(pem->bytes);
   }
   memset(pem, 0, sizeof(*pem));
}

/* Capture exactly the bytes later passed to OpenSSL. This avoids the common
 * hash-then-reopen TOCTOU error and rejects symlinks/non-regular files. */
static int capture_pem(const char *path, captured_pem_t *out)
{
   if (!path || !path[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
       st.st_size > (off_t)MANAGEMENT_PEM_MAX)
   {
      close(fd);
      return -1;
   }
   size_t len = (size_t)st.st_size;
   unsigned char *bytes = malloc(len + 1);
   if (!bytes)
   {
      close(fd);
      return -1;
   }
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = read(fd, bytes + off, len - off);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      off += (size_t)n;
   }
   unsigned char extra;
   ssize_t trailing;
   do
      trailing = read(fd, &extra, 1);
   while (trailing < 0 && errno == EINTR);
   close(fd);
   if (off != len || trailing != 0)
   {
      OPENSSL_cleanse(bytes, len);
      free(bytes);
      return -1;
   }
   bytes[len] = '\0';
   unsigned int hash_len = 0;
   if (EVP_Digest(bytes, len, out->hash, &hash_len, EVP_sha256(), NULL) != 1 || hash_len != 32)
   {
      OPENSSL_cleanse(bytes, len);
      free(bytes);
      return -1;
   }
   out->bytes = bytes;
   out->len = len;
   return 0;
}

static int capture_pem_value(const char *value, captured_pem_t *out)
{
   if (!value || !value[0] || !out)
      return -1;
   size_t len = strlen(value);
   if (len > MANAGEMENT_PEM_MAX)
      return -1;
   memset(out, 0, sizeof(*out));
   unsigned char *bytes = malloc(len + 1);
   if (!bytes)
      return -1;
   memcpy(bytes, value, len + 1);
   unsigned int hash_len = 0;
   if (EVP_Digest(bytes, len, out->hash, &hash_len, EVP_sha256(), NULL) != 1 || hash_len != 32)
   {
      OPENSSL_cleanse(bytes, len + 1);
      free(bytes);
      return -1;
   }
   out->bytes = bytes;
   out->len = len;
   return 0;
}

/* mTLS verify callback: OpenSSL has already checked the chain/validity
 * (preverify_ok). Additionally reject a revoked leaf (depth 0) by consulting the
 * in-memory revocation snapshot — no DB query in the handshake path. */
static int mtls_verify_cb(int preverify_ok, X509_STORE_CTX *ctx)
{
   if (!preverify_ok)
      return 0; /* chain/time/CA failure -> reject */
   if (X509_STORE_CTX_get_error_depth(ctx) != 0)
      return 1; /* EKU and revocation are leaf-only; CA/intermediate EKU is irrelevant */
   X509 *cert = X509_STORE_CTX_get_current_cert(ctx);
   if (!cert)
      return 1;
   if (!exact_cert_eku(cert, NID_client_auth))
   {
      X509_STORE_CTX_set_error(ctx, X509_V_ERR_INVALID_PURPOSE);
      return 0;
   }
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

/* ALPN selection: aimee's /v1 server speaks HTTP/1.1 only (hand-rolled HTTP/1.1
 * server — see server_http.c). Advertise that explicitly so ALPN-strict clients
 * settle on HTTP/1.1 instead of attempting HTTP/2 on an un-negotiated channel.
 * The Codex CLI's model client (reqwest/hyper) does exactly this: without a
 * negotiated ALPN it tries HTTP/2 and the request fails before reaching the
 * server; offered "http/1.1" it uses HTTP/1.1 and connects. We never advertise
 * "h2" (we cannot speak it).
 *
 * On no overlap we deliberately return NOACK, not a fatal no_application_protocol
 * alert (RFC 7301 §3.2). NOACK makes OpenSSL omit the ALPN extension from the
 * ServerHello, which is wire-identical to this server's prior no-ALPN behaviour —
 * an h2-only or legacy no-ALPN client (e.g. the aimee thin client) keeps the
 * exact handshake it had before. A fatal alert would instead *regress* those
 * clients into a hard handshake failure, so backward-compatibility wins here.
 *
 * SSL_select_next_proto is the canonical ALPN-selection helper (see the example
 * in SSL_CTX_set_alpn_select_cb(3)); the (unsigned char **) cast is mandated by
 * its signature. Requires OpenSSL >= 1.0.2 — far below the 1.1.0+ floor this
 * file already assumes via SSL_CTX_set_min_proto_version. */
static int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg)
{
   (void)ssl;
   (void)arg;
   /* ALPN protocol list = length-prefixed wire form; the leading 8 is the byte
    * length of "http/1.1" and must track the literal. */
   static const unsigned char http11[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
   if (SSL_select_next_proto((unsigned char **)out, outlen, http11, sizeof(http11), in, inlen) !=
       OPENSSL_NPN_NEGOTIATED)
      return SSL_TLSEXT_ERR_NOACK; /* client didn't offer http/1.1 -> leave ALPN unset */
   return SSL_TLSEXT_ERR_OK;
}

static int ctx_use_private_key_pem(SSL_CTX *ctx, const char *pem)
{
   BIO *bio = pem ? BIO_new_mem_buf(pem, -1) : NULL;
   EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
   int ok = key && SSL_CTX_use_PrivateKey(ctx, key) == 1;
   EVP_PKEY_free(key);
   BIO_free(bio);
   return ok ? 0 : -1;
}

static int exact_cert_eku(X509 *cert, int required_nid)
{
   int pos = cert ? X509_get_ext_by_NID(cert, NID_ext_key_usage, -1) : -1;
   if (pos < 0 || X509_get_ext_by_NID(cert, NID_ext_key_usage, pos) >= 0)
      return 0;
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
   int ok = eku && sk_ASN1_OBJECT_num(eku) == 1 &&
            OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == required_nid;
   EXTENDED_KEY_USAGE_free(eku);
   return ok;
}

/* Enforce the reciprocal half of pair separation. kb_client_mtls rejects a KB
 * client identity that collides with an already-installed listener identity;
 * this rejects a listener identity installed or rotated after the KB identity. */
static int distinct_from_kb_client_identity(X509 *server_cert)
{
   char path[MAX_PATH_LEN];
   int n = snprintf(path, sizeof(path), "%s/kb-client-identity.json", config_default_dir());
   if (n <= 0 || (size_t)n >= sizeof(path))
      return 0;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return errno == ENOENT;
   struct stat st;
   int valid_file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                    st.st_nlink == 1 && !(st.st_mode & (S_IRWXG | S_IRWXO)) && st.st_size > 0 &&
                    st.st_size < 32768;
   char *raw = valid_file ? calloc(1, (size_t)st.st_size + 1) : NULL;
   size_t used = 0;
   while (raw && used < (size_t)st.st_size)
   {
      ssize_t got = read(fd, raw + used, (size_t)st.st_size - used);
      if (got < 0 && errno == EINTR)
         continue;
      if (got <= 0)
         break;
      used += (size_t)got;
   }
   close(fd);
   cJSON *root = raw && used == (size_t)st.st_size ? cJSON_ParseWithLength(raw, used) : NULL;
   cJSON *cert_item = root ? cJSON_GetObjectItemCaseSensitive(root, "cert") : NULL;
   BIO *client_bio = cJSON_IsString(cert_item) && cert_item->valuestring[0]
                         ? BIO_new_mem_buf(cert_item->valuestring, -1)
                         : NULL;
   X509 *client_cert = client_bio ? PEM_read_bio_X509(client_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *server_key = server_cert ? X509_get_pubkey(server_cert) : NULL;
   EVP_PKEY *client_key = client_cert ? X509_get_pubkey(client_cert) : NULL;
   int distinct = server_key && client_key && EVP_PKEY_eq(server_key, client_key) == 0;
   EVP_PKEY_free(server_key);
   EVP_PKEY_free(client_key);
   X509_free(client_cert);
   BIO_free(client_bio);
   cJSON_Delete(root);
   if (raw)
   {
      OPENSSL_cleanse(raw, (size_t)st.st_size + 1);
      free(raw);
   }
   return distinct;
}

/* Build a fresh SSL_CTX from the given cert/key/mtls settings, WITHOUT touching g_ctx.
 * Returns the ctx (caller owns) or NULL on any load failure — so both init and a live reload
 * validate-or-keep: a bad cert never disturbs the running listener. */
static SSL_CTX *tls_build_ctx(const char *cert_path, const char *key_path, int mtls_mode,
                              const char *client_ca_path)
{
   SSL_CTX *ctx = aimee_core_tls_server_context();
   if (!ctx)
   {
      aimee_log(LOG_WARN, "server.tls", "SSL_CTX_new failed");
      return NULL;
   }
   /* Modern floor; no client-cert verification (plain server TLS — the bearer is
    * the caller's authentication, the TLS is the channel). */
   /* Advertise HTTP/1.1 over ALPN so ALPN-strict clients (e.g. Codex) don't
    * fall back to HTTP/2 on this HTTP/1.1-only server. */
   SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);
   /* Session resumption REQUIRES this once client certs are in play (below).
    * OpenSSL refuses to resume a session carrying a peer certificate unless the
    * server's session id context matches the one the session was created under;
    * with none set it fails SSL_R_SESSION_ID_CONTEXT_UNINITIALIZED and sends an
    * internal_error alert instead of completing the handshake. Server-side
    * caching and TLS 1.3 tickets are both on by default here, so leaving this
    * unset broke exactly the resuming half of a client's connections. Set it
    * unconditionally: mTLS can be ramped on at runtime, and a stable value is
    * correct for a non-mTLS context too. */
   static const unsigned char sid_ctx[] = "aimee-server";
   SSL_CTX_set_session_id_context(ctx, sid_ctx, sizeof(sid_ctx) - 1);
   int identity_ok = 0;
   if (key_path && key_path[0])
      identity_ok = aimee_core_tls_use_identity_files(ctx, cert_path, key_path) == 0;
   else
   {
      char key_pem[4096] = "";
      int key_ok = pki_server_tls_key_load(key_pem, sizeof(key_pem)) == 0 &&
                   ctx_use_private_key_pem(ctx, key_pem) == 0;
      OPENSSL_cleanse(key_pem, sizeof(key_pem));
      identity_ok = key_ok && SSL_CTX_use_certificate_chain_file(ctx, cert_path) == 1 &&
                    SSL_CTX_check_private_key(ctx) == 1;
   }

   if (!identity_ok)
   {
      aimee_log(LOG_WARN, "server.tls", "failed to load TLS cert/Vault key (%s): %s", cert_path,
                ERR_error_string(ERR_get_error(), NULL));
      SSL_CTX_free(ctx);
      return NULL;
   }
   if (!exact_cert_eku(SSL_CTX_get0_certificate(ctx), NID_server_auth))
   {
      aimee_log(LOG_WARN, "server.tls", "TLS certificate must have only the serverAuth EKU: %s",
                cert_path);
      SSL_CTX_free(ctx);
      return NULL;
   }
   if (!distinct_from_kb_client_identity(SSL_CTX_get0_certificate(ctx)))
   {
      aimee_log(LOG_WARN, "server.tls", "TLS certificate reuses the KB client identity key: %s",
                cert_path);
      SSL_CTX_free(ctx);
      return NULL;
   }

   /* mTLS: verify client certs against aimee's client CA. The chain is verified
    * by OpenSSL; the CN -> principal mapping + revocation happen later in the
    * identity layer. Even application-required mode must allow a cert-less TLS
    * handshake to reach the narrowly scoped bearer-authenticated enrollment
    * routes. server_http_mtls_transport_allowed rejects every other cert-less
    * request before bearer/capability dispatch. The dedicated management
    * listener below remains transport-required mTLS. */
   if (mtls_mode > 0)
   {
      char ca[MAX_PATH_LEN];
      if (client_ca_path && client_ca_path[0])
         snprintf(ca, sizeof(ca), "%s", client_ca_path);
      else
         snprintf(ca, sizeof(ca), "%s/tls/client-ca.crt", config_default_dir());
      if (aimee_core_tls_trust_file(ctx, ca) != 0)
      {
         aimee_log(LOG_WARN, "server.tls",
                   "mtls enabled but client CA %s not loadable: %s — refusing TLS context", ca,
                   ERR_error_string(ERR_get_error(), NULL));
         SSL_CTX_free(ctx);
         return NULL;
      }
      else
      {
         SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, mtls_verify_cb);
         aimee_log(LOG_INFO, "server.tls", "mTLS %s (client CA %s)",
                   mtls_mode >= 2 ? "application-required" : "optional", ca);
      }
   }

   return ctx;
}

static int ctx_use_captured_chain(SSL_CTX *ctx, const captured_pem_t *pem)
{
   BIO *bio = BIO_new_mem_buf(pem->bytes, (int)pem->len);
   STACK_OF(X509_INFO) *info = bio ? PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   if (!info || sk_X509_INFO_num(info) < 1)
   {
      sk_X509_INFO_pop_free(info, X509_INFO_free);
      return -1;
   }
   int ok = 1;
   for (int i = 0; ok && i < sk_X509_INFO_num(info); ++i)
   {
      X509_INFO *item = sk_X509_INFO_value(info, i);
      if (!item || !item->x509 || item->crl || item->x_pkey)
      {
         ok = 0;
         break;
      }
      if (i == 0)
         ok = SSL_CTX_use_certificate(ctx, item->x509) == 1;
      else
      {
         /* SSL_CTX_add_extra_chain_cert takes ownership of one reference. */
         ok = X509_up_ref(item->x509) == 1;
         if (ok && SSL_CTX_add_extra_chain_cert(ctx, item->x509) != 1)
         {
            X509_free(item->x509);
            ok = 0;
         }
      }
   }
   sk_X509_INFO_pop_free(info, X509_INFO_free);
   return ok ? 0 : -1;
}

static int ctx_use_captured_key(SSL_CTX *ctx, const captured_pem_t *pem)
{
   BIO *bio = BIO_new_mem_buf(pem->bytes, (int)pem->len);
   EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
   int ok = key && SSL_CTX_use_PrivateKey(ctx, key) == 1;
   EVP_PKEY_free(key);
   BIO_free(bio);
   return ok ? 0 : -1;
}

static int ctx_use_captured_ca(SSL_CTX *ctx, const captured_pem_t *pem)
{
   BIO *bio = BIO_new_mem_buf(pem->bytes, (int)pem->len);
   STACK_OF(X509_INFO) *info = bio ? PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   if (!info || sk_X509_INFO_num(info) != 1)
   {
      sk_X509_INFO_pop_free(info, X509_INFO_free);
      return -1;
   }
   X509_STORE *store = SSL_CTX_get_cert_store(ctx);
   int certs = 0, ok = store != NULL;
   for (int i = 0; ok && i < sk_X509_INFO_num(info); ++i)
   {
      X509_INFO *item = sk_X509_INFO_value(info, i);
      EVP_PKEY *ca_key = item && item->x509 ? X509_get_pubkey(item->x509) : NULL;
      int self_signed = item && item->x509 && ca_key &&
                        X509_check_issued(item->x509, item->x509) == X509_V_OK &&
                        X509_verify(item->x509, ca_key) == 1;
      EVP_PKEY_free(ca_key);
      if (!item || !item->x509 || item->crl || item->x_pkey || X509_check_ca(item->x509) <= 0 ||
          !self_signed || X509_STORE_add_cert(store, item->x509) != 1)
         ok = 0;
      else
         ++certs;
   }
   sk_X509_INFO_pop_free(info, X509_INFO_free);
   return ok && certs > 0 ? 0 : -1;
}

static int end_entity_key_usage(X509 *cert, int exact_digital_signature)
{
   int bc_pos = cert ? X509_get_ext_by_NID(cert, NID_basic_constraints, -1) : -1;
   int ku_pos = cert ? X509_get_ext_by_NID(cert, NID_key_usage, -1) : -1;
   if (bc_pos < 0 || ku_pos < 0 || X509_get_ext_by_NID(cert, NID_basic_constraints, bc_pos) >= 0 ||
       X509_get_ext_by_NID(cert, NID_key_usage, ku_pos) >= 0)
      return 0;
   BASIC_CONSTRAINTS *bc = X509_get_ext_d2i(cert, NID_basic_constraints, NULL, NULL);
   ASN1_BIT_STRING *ku = X509_get_ext_d2i(cert, NID_key_usage, NULL, NULL);
   int ok = bc && !bc->ca && ku && ASN1_BIT_STRING_get_bit(ku, 0);
   if (ok && exact_digital_signature)
      for (int bit = 1; bit <= 8; bit++)
         if (ASN1_BIT_STRING_get_bit(ku, bit))
            ok = 0;
   BASIC_CONSTRAINTS_free(bc);
   ASN1_BIT_STRING_free(ku);
   return ok;
}

/* This builder deliberately has no generic roster callback parameter. Online
 * management revocation is enforced by the nonce/staple protocol above TLS. */
static SSL_CTX *management_build_ctx(const captured_pem_t *cert, const captured_pem_t *key,
                                     const captured_pem_t *ca)
{
   SSL_CTX *ctx = aimee_core_tls_server_context();
   if (!ctx)
      return NULL;
   long options = SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
   options |= SSL_OP_NO_RENEGOTIATION;
#endif
   SSL_CTX_set_options(ctx, options);
   SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
   SSL_CTX_set_verify_depth(ctx, 6);
   SSL_CTX_set_security_level(ctx, 2);
   SSL_CTX_set_post_handshake_auth(ctx, 0);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
   SSL_CTX_set_max_early_data(ctx, 0);
#endif
   SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);
   if (X509_VERIFY_PARAM_set_purpose(SSL_CTX_get0_param(ctx), X509_PURPOSE_SSL_CLIENT) != 1 ||
       ctx_use_captured_ca(ctx, ca) != 0 || ctx_use_captured_chain(ctx, cert) != 0 ||
       ctx_use_captured_key(ctx, key) != 0 || SSL_CTX_check_private_key(ctx) != 1 ||
       !exact_cert_eku(SSL_CTX_get0_certificate(ctx), NID_server_auth) ||
       !end_entity_key_usage(SSL_CTX_get0_certificate(ctx), 0))
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   return ctx;
}

static int management_init_captured(const captured_pem_t *cert, const captured_pem_t *key,
                                    const captured_pem_t *ca)
{
   int rc = -1;
   pthread_mutex_lock(&g_management_ctx_mu);
   if (g_management_ctx)
   {
      int same = CRYPTO_memcmp(cert->hash, g_management_cert_hash, 32) == 0 &&
                 CRYPTO_memcmp(key->hash, g_management_key_hash, 32) == 0 &&
                 CRYPTO_memcmp(ca->hash, g_management_ca_hash, 32) == 0;
      pthread_mutex_unlock(&g_management_ctx_mu);
      return same ? 0 : -1;
   }
   pthread_mutex_unlock(&g_management_ctx_mu);

   SSL_CTX *candidate = management_build_ctx(cert, key, ca);
   if (!candidate)
      return -1;

   pthread_mutex_lock(&g_management_ctx_mu);
   if (!g_management_ctx)
   {
      g_management_ctx = candidate;
      candidate = NULL;
      memcpy(g_management_cert_hash, cert->hash, 32);
      memcpy(g_management_key_hash, key->hash, 32);
      memcpy(g_management_ca_hash, ca->hash, 32);
      rc = 0;
   }
   else
   {
      rc = CRYPTO_memcmp(cert->hash, g_management_cert_hash, 32) == 0 &&
                   CRYPTO_memcmp(key->hash, g_management_key_hash, 32) == 0 &&
                   CRYPTO_memcmp(ca->hash, g_management_ca_hash, 32) == 0
               ? 0
               : -1;
   }
   pthread_mutex_unlock(&g_management_ctx_mu);
   SSL_CTX_free(candidate);
   if (rc == 0)
      aimee_log(LOG_INFO, "server.tls", "dedicated management mTLS enabled");
   return rc;
}

int server_tls_management_init(const char *cert_path, const char *key_path,
                               const char *client_ca_path)
{
   captured_pem_t cert = {0}, key = {0}, ca = {0};
   int rc = -1;
   if (capture_pem(cert_path, &cert) == 0 && capture_pem(key_path, &key) == 0 &&
       capture_pem(client_ca_path, &ca) == 0)
      rc = management_init_captured(&cert, &key, &ca);
   captured_pem_clear(&ca);
   captured_pem_clear(&key);
   captured_pem_clear(&cert);
   if (rc != 0)
      aimee_log(LOG_WARN, "server.tls", "dedicated management mTLS initialization failed");
   return rc;
}

int server_tls_management_init_vault(const char *cert_path, const char *key_pem,
                                     const char *client_ca_path)
{
   captured_pem_t cert = {0}, key = {0}, ca = {0};
   int rc = -1;
   if (capture_pem(cert_path, &cert) == 0 && capture_pem_value(key_pem, &key) == 0 &&
       capture_pem(client_ca_path, &ca) == 0)
      rc = management_init_captured(&cert, &key, &ca);
   captured_pem_clear(&ca);
   captured_pem_clear(&key);
   captured_pem_clear(&cert);
   if (rc != 0)
      aimee_log(LOG_WARN, "server.tls", "dedicated management Vault TLS initialization failed");
   return rc;
}

int server_tls_init(const char *cert_path, const char *key_path, int mtls_mode,
                    const char *client_ca_path)
{
   if (!cert_path || !cert_path[0])
      return -1;
   pthread_mutex_lock(&g_ctx_mu);
   if (g_ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      return 0;
   }
   SSL_CTX *ctx = tls_build_ctx(cert_path, key_path, mtls_mode, client_ca_path);
   if (!ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      return -1;
   }
   g_ctx = ctx;
   /* remember the paths so a SIGHUP reload can re-read the same files */
   int t1 = snprintf(g_cert_path, sizeof g_cert_path, "%s", cert_path);
   int t2 = snprintf(g_key_path, sizeof g_key_path, "%s", key_path ? key_path : "");
   snprintf(g_client_ca_path, sizeof g_client_ca_path, "%s", client_ca_path ? client_ca_path : "");
   g_mtls_mode = mtls_mode;
   if (t1 >= (int)sizeof g_cert_path || t2 >= (int)sizeof g_key_path)
      aimee_log(LOG_WARN, "server.tls",
                "TLS cert/key path too long to save — live SIGHUP cert reload will be a no-op "
                "(restart to pick up a renewed cert)");
   pthread_mutex_unlock(&g_ctx_mu);
   aimee_log(LOG_INFO, "server.tls", "native TLS enabled (cert %s)", cert_path);
   return 0;
}

int server_tls_reload(void)
{
   pthread_mutex_lock(&g_transition_mu);
   pthread_mutex_lock(&g_ctx_mu);
   if (!g_ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      pthread_mutex_unlock(&g_transition_mu);
      return 0; /* TLS not enabled -> nothing to reload */
   }
   char cert[MAX_PATH_LEN], key[MAX_PATH_LEN], ca[MAX_PATH_LEN];
   snprintf(cert, sizeof cert, "%s", g_cert_path);
   snprintf(key, sizeof key, "%s", g_key_path);
   snprintf(ca, sizeof ca, "%s", g_client_ca_path);
   int mtls = g_mtls_mode;
   pthread_mutex_unlock(&g_ctx_mu);

   /* Build the replacement OUTSIDE the lock (file I/O). Validate-or-keep: a cert that fails to
    * load leaves the running listener on the current cert (never a broken TLS endpoint). */
   SSL_CTX *nctx = tls_build_ctx(cert, key[0] ? key : NULL, mtls, ca[0] ? ca : NULL);
   if (!nctx)
   {
      aimee_log(LOG_WARN, "server.tls",
                "TLS reload: new cert/key failed to load — keeping the current cert");
      pthread_mutex_unlock(&g_transition_mu);
      return -1;
   }
   pthread_mutex_lock(&g_ctx_mu);
   SSL_CTX *old = g_ctx;
   g_ctx = nctx; /* new handshakes use the new cert */
   /* Free UNDER the lock: post-swap accepts read nctx (never old), and in-flight SSL objects
    * hold their own ref on `old` (SSL_new up-refs the SSL_CTX), so this drops only OUR ref —
    * `old` is destroyed when the last live SSL on it is freed. Doing it inside the critical
    * section closes any window where an accept could observe the pointer being freed. */
   SSL_CTX_free(old);
   pthread_mutex_unlock(&g_ctx_mu);
   pthread_mutex_unlock(&g_transition_mu);
   aimee_log(LOG_INFO, "server.tls", "TLS cert reloaded (cert %s)", cert);
   return 1;
}

int server_tls_mtls_mode(void)
{
   pthread_mutex_lock(&g_ctx_mu);
   int mode = g_mtls_mode;
   pthread_mutex_unlock(&g_ctx_mu);
   return mode;
}

SSL_CTX *server_tls_prepare_required(void)
{
   pthread_mutex_lock(&g_transition_mu);
   pthread_mutex_lock(&g_ctx_mu);
   if (!g_ctx || g_mtls_mode >= 2)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      pthread_mutex_unlock(&g_transition_mu);
      return NULL;
   }
   char cert[MAX_PATH_LEN], key[MAX_PATH_LEN], ca[MAX_PATH_LEN];
   snprintf(cert, sizeof(cert), "%s", g_cert_path);
   snprintf(key, sizeof(key), "%s", g_key_path);
   snprintf(ca, sizeof(ca), "%s", g_client_ca_path);
   pthread_mutex_unlock(&g_ctx_mu);
   SSL_CTX *prepared = tls_build_ctx(cert, key[0] ? key : NULL, 2, ca[0] ? ca : NULL);
   if (!prepared)
      pthread_mutex_unlock(&g_transition_mu);
   return prepared;
}

int server_tls_activate_required(SSL_CTX *prepared)
{
   if (!prepared)
      return -1;
   pthread_mutex_lock(&g_ctx_mu);
   if (!g_ctx)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      SSL_CTX_free(prepared);
      pthread_mutex_unlock(&g_transition_mu);
      return -1;
   }
   if (g_mtls_mode >= 2)
   {
      pthread_mutex_unlock(&g_ctx_mu);
      SSL_CTX_free(prepared);
      pthread_mutex_unlock(&g_transition_mu);
      return 0;
   }
   SSL_CTX *old = g_ctx;
   g_ctx = prepared;
   g_mtls_mode = 2;
   SSL_CTX_free(old);
   pthread_mutex_unlock(&g_ctx_mu);
   pthread_mutex_unlock(&g_transition_mu);
   aimee_log(LOG_INFO, "server.tls", "mTLS ramp advanced to required");
   return 1;
}

void server_tls_discard_prepared(SSL_CTX *prepared)
{
   if (prepared)
   {
      SSL_CTX_free(prepared);
      pthread_mutex_unlock(&g_transition_mu);
   }
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

static int exact_management_common_name(X509 *cert)
{
   static const unsigned char expected[] = "p5-kb-management";
   X509_NAME *subject = X509_get_subject_name(cert);
   if (!subject)
      return 0;
   int pos = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
   if (pos < 0 || X509_NAME_get_index_by_NID(subject, NID_commonName, pos) >= 0)
      return 0;
   X509_NAME_ENTRY *entry = X509_NAME_get_entry(subject, pos);
   ASN1_STRING *value = entry ? X509_NAME_ENTRY_get_data(entry) : NULL;
   return value && ASN1_STRING_length(value) == (int)sizeof(expected) - 1 &&
          CRYPTO_memcmp(ASN1_STRING_get0_data(value), expected, sizeof(expected) - 1) == 0;
}

static int exact_management_marker(X509 *cert)
{
   static const unsigned char marker[] = "aimee-p5-kb-management-v1";
   ASN1_OBJECT *oid = OBJ_txt2obj("1.3.6.1.4.1.55555.5.1", 1);
   if (!oid)
      return 0;
   int pos = X509_get_ext_by_OBJ(cert, oid, -1);
   X509_EXTENSION *ext = pos >= 0 ? X509_get_ext(cert, pos) : NULL;
   ASN1_OCTET_STRING *value = ext ? X509_EXTENSION_get_data(ext) : NULL;
   int ok = ext && !X509_EXTENSION_get_critical(ext) && X509_get_ext_by_OBJ(cert, oid, pos) < 0 &&
            value && ASN1_STRING_length(value) == (int)sizeof(marker) - 1 &&
            CRYPTO_memcmp(ASN1_STRING_get0_data(value), marker, sizeof(marker) - 1) == 0;
   ASN1_OBJECT_free(oid);
   return ok;
}

static int exact_management_client_eku(X509 *cert)
{
   int pos = X509_get_ext_by_NID(cert, NID_ext_key_usage, -1);
   if (pos < 0 || X509_get_ext_by_NID(cert, NID_ext_key_usage, pos) >= 0)
      return 0;
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
   int ok = eku && sk_ASN1_OBJECT_num(eku) == 1 &&
            OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == NID_client_auth &&
            X509_check_purpose(cert, X509_PURPOSE_SSL_CLIENT, 0) == 1;
   EXTENDED_KEY_USAGE_free(eku);
   return ok;
}

int server_tls_peer_cert(SSL *ssl, server_tls_peer_cert_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK)
      return 0;
   X509 *cert = SSL_get1_peer_certificate(ssl);
   if (!cert)
      return 0;
   int ok = 0;
   char *issuer = NULL, *serial = NULL;
   unsigned char digest[32], binding[32];
   unsigned int digest_len = 0;
   X509_NAME *subject = X509_get_subject_name(cert);
   ASN1_INTEGER *asn_serial = X509_get_serialNumber(cert);
   BIGNUM *bn = asn_serial ? ASN1_INTEGER_to_BN(asn_serial, NULL) : NULL;
   issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
   serial = bn ? BN_bn2hex(bn) : NULL;
   if (!subject ||
       X509_NAME_get_text_by_NID(subject, NID_commonName, out->cn, (int)sizeof(out->cn)) <= 0 ||
       !issuer || strlen(issuer) >= sizeof(out->issuer) || !serial || !serial[0] ||
       strlen(serial) >= sizeof(out->serial_norm) ||
       X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1 || digest_len != 32 ||
       SSL_export_keying_material(ssl, binding, sizeof(binding), "aimee-management-channel-v1",
                                  sizeof("aimee-management-channel-v1") - 1, NULL, 0, 0) != 1)
      goto done;
   snprintf(out->issuer, sizeof(out->issuer), "%s", issuer);
   for (size_t i = 0; serial[i]; i++)
      out->serial_norm[i] = (char)tolower((unsigned char)serial[i]);
   for (size_t i = 0; i < 32; i++)
   {
      snprintf(out->fingerprint + i * 2, 3, "%02x", digest[i]);
      snprintf(out->channel_binding + i * 2, 3, "%02x", binding[i]);
   }
   out->fingerprint[64] = out->channel_binding[64] = '\0';
   out->management_profile = exact_management_common_name(cert) && exact_management_marker(cert) &&
                             exact_management_client_eku(cert) && end_entity_key_usage(cert, 1);
   ok = 1;
done:
   OPENSSL_free(serial);
   OPENSSL_free(issuer);
   BN_free(bn);
   X509_free(cert);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok;
}

int server_tls_local_fingerprint(SSL *ssl, char out[65])
{
   if (!ssl || !out)
      return 0;
   out[0] = '\0';
   X509 *cert = SSL_get_certificate(ssl); /* owned by SSL */
   unsigned char md[32];
   unsigned int n = 0;
   if (!cert || X509_digest(cert, EVP_sha256(), md, &n) != 1 || n != 32)
      return 0;
   for (size_t i = 0; i < 32; i++)
      snprintf(out + i * 2, 3, "%02x", md[i]);
   out[64] = '\0';
   return 1;
}

int server_tls_local_cert(SSL *ssl, server_tls_peer_cert_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   X509 *cert = ssl ? SSL_get_certificate(ssl) : NULL; /* owned by SSL */
   ASN1_INTEGER *asn_serial = cert ? X509_get_serialNumber(cert) : NULL;
   BIGNUM *bn = asn_serial ? ASN1_INTEGER_to_BN(asn_serial, NULL) : NULL;
   char *issuer = cert ? X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0) : NULL;
   char *serial = bn && !BN_is_negative(bn) ? BN_bn2hex(bn) : NULL;
   unsigned char md[32];
   unsigned int md_n = 0;
   int ok = issuer && issuer[0] && strlen(issuer) < sizeof(out->issuer) && serial && serial[0] &&
            strlen(serial) < sizeof(out->serial_norm) &&
            X509_digest(cert, EVP_sha256(), md, &md_n) == 1 && md_n == 32;
   if (ok)
   {
      snprintf(out->issuer, sizeof(out->issuer), "%s", issuer);
      for (size_t i = 0; serial[i]; ++i)
         out->serial_norm[i] = (char)tolower((unsigned char)serial[i]);
      for (size_t i = 0; i < sizeof(md); ++i)
         snprintf(out->fingerprint + i * 2, 3, "%02x", md[i]);
      out->fingerprint[64] = 0;
   }
   OPENSSL_free(serial);
   OPENSSL_free(issuer);
   BN_free(bn);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok;
}

static SSL *tls_accept_with_ssl(int fd, SSL *ssl)
{
   if (fd < 0 || !ssl)
      return NULL;
   /* Bound the handshake (and subsequent blocking reads) so a stalled peer cannot
    * pin a per-conn worker thread indefinitely (the conn cap is small). */
   struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   int one = 1;
   (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   SSL_set_fd(ssl, fd);
   if (SSL_accept(ssl) != 1)
   {
      SSL_free(ssl);
      return NULL;
   }
   return ssl;
}

SSL *server_tls_accept(int fd)
{
   if (fd < 0)
      return NULL;
   /* Take the ctx lock only around SSL_new so its up-ref is atomic vs a live
    * generic cert reload's swap+free. The handshake runs outside the lock. */
   pthread_mutex_lock(&g_ctx_mu);
   SSL *ssl = g_ctx ? SSL_new(g_ctx) : NULL;
   pthread_mutex_unlock(&g_ctx_mu);
   return tls_accept_with_ssl(fd, ssl);
}

static SSL *server_tls_management_accept(int fd)
{
   if (fd < 0)
      return NULL;
   pthread_mutex_lock(&g_management_ctx_mu);
   SSL *ssl = g_management_ctx ? SSL_new(g_management_ctx) : NULL;
   pthread_mutex_unlock(&g_management_ctx_mu);
   return tls_accept_with_ssl(fd, ssl);
}

int server_tls_init_default(void)
{
   char cert[MAX_PATH_LEN], key[MAX_PATH_LEN];
   snprintf(cert, sizeof(cert), "%s/tls/server.crt", config_default_dir());
   snprintf(key, sizeof(key), "%s/tls/server.key", config_default_dir());
   int effective_mtls = pki_mtls_ramp_init(config_server_api_mtls());
   if (effective_mtls < 0)
      return -1;
   if (effective_mtls == 1)
      aimee_log(LOG_WARN, "server.tls",
                "mTLS migration is optional: bearer-only clients remain accepted until the "
                "durable roster is fully presented");
   /* Secure-by-default: a server identity certificate is required for every TLS
    * mode, independently of whether the listener also requests client certs.
    * Provision the stable self-signed identity on a fresh install rather than
    * disabling HTTPS precisely when optional mTLS is enabled for enrollment.
    * An operator-supplied cert at the same path remains authoritative. */
   pki_ensure_self_signed_server_cert(cert, key);
   /* When mTLS is on, ensure aimee's client CA exists (create-or-load) and the
    * revocation snapshot is loaded BEFORE server_tls_init loads the client CA
    * file and the verify callback starts consulting the snapshot. */
   if (effective_mtls > 0 && pki_ca_ensure() != 0)
      return -1;
   return server_tls_init(cert, NULL, effective_mtls, config_server_api_mtls_client_ca());
}

SSL *server_tls_begin(int fd)
{
   SSL *ssl = server_tls_accept(fd);
   if (ssl)
      server_conn_io_set_ssl(fd, ssl);
   return ssl;
}

SSL *server_tls_management_begin(int fd)
{
   SSL *ssl = server_tls_management_accept(fd);
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
