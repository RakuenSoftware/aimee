/* Exact P5 management-client certificate-profile tests over a real mTLS handshake. */
#include "kb_mgmt_status_peer.h"

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
   int marker_count;
   int marker_critical;
   const char *marker;
   const char *cn;
   const char *eku;
   int duplicate_cn;
} client_profile_t;

static EVP_PKEY *make_key(void)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
          EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1 && EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static void add_ext(X509 *issuer, X509 *cert, int nid, const char *value)
{
   X509V3_CTX ctx;
   X509V3_set_ctx_nodb(&ctx);
   X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
   X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
   assert(ext && X509_add_ext(cert, ext, -1) == 1);
   X509_EXTENSION_free(ext);
}

static void add_marker(X509 *cert, int critical, const char *text)
{
   ASN1_OBJECT *oid = OBJ_txt2obj("1.3.6.1.4.1.55555.5.1", 1);
   ASN1_OCTET_STRING *value = ASN1_OCTET_STRING_new();
   X509_EXTENSION *ext = NULL;
   assert(oid && value && ASN1_OCTET_STRING_set(value, (const unsigned char *)text,
                                                (int)strlen(text)) == 1);
   ext = X509_EXTENSION_create_by_OBJ(NULL, oid, critical, value);
   assert(ext && X509_add_ext(cert, ext, -1) == 1);
   X509_EXTENSION_free(ext);
   ASN1_OCTET_STRING_free(value);
   ASN1_OBJECT_free(oid);
}

static X509 *make_ca(EVP_PKEY *key, const char *cn)
{
   X509 *cert = X509_new();
   assert(cert && X509_set_version(cert, 2) == 1 &&
          ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1 &&
          X509_gmtime_adj(X509_getm_notBefore(cert), -60) &&
          X509_gmtime_adj(X509_getm_notAfter(cert), 3600) && X509_set_pubkey(cert, key) == 1);
   X509_NAME *name = X509_get_subject_name(cert);
   assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1,
                                     0) == 1 &&
          X509_set_issuer_name(cert, name) == 1);
   add_ext(cert, cert, NID_basic_constraints, "critical,CA:TRUE");
   add_ext(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
   assert(X509_sign(cert, key, EVP_sha256()) > 0);
   return cert;
}

static X509 *make_leaf(X509 *ca, EVP_PKEY *ca_key, EVP_PKEY *key, long serial, int server,
                       const client_profile_t *profile)
{
   X509 *cert = X509_new();
   assert(cert && X509_set_version(cert, 2) == 1 &&
          ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) == 1 &&
          X509_gmtime_adj(X509_getm_notBefore(cert), -60) &&
          X509_gmtime_adj(X509_getm_notAfter(cert), 3600) && X509_set_pubkey(cert, key) == 1);
   const char *cn = server ? "status.test" : profile->cn;
   X509_NAME *name = X509_get_subject_name(cert);
   assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1,
                                     0) == 1);
   if (!server && profile->duplicate_cn)
      assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                        (const unsigned char *)"p5-kb-management", -1, -1, 0) ==
             1);
   assert(X509_set_issuer_name(cert, X509_get_subject_name(ca)) == 1);
   add_ext(ca, cert, NID_basic_constraints, "critical,CA:FALSE");
   add_ext(ca, cert, NID_key_usage,
           server ? "critical,digitalSignature,keyEncipherment" : "critical,digitalSignature");
   add_ext(ca, cert, NID_ext_key_usage, server ? "serverAuth" : profile->eku);
   if (!server)
      for (int i = 0; i < profile->marker_count; i++)
         add_marker(cert, profile->marker_critical, profile->marker);
   assert(X509_sign(cert, ca_key, EVP_sha256()) > 0);
   return cert;
}

static int use_identity(SSL_CTX *ctx, X509 *cert, EVP_PKEY *key)
{
   return SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1 &&
          SSL_CTX_check_private_key(ctx) == 1;
}

static SSL_CTX *server_ctx(X509 *ca, X509 *cert, EVP_PKEY *key)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   assert(ctx && use_identity(ctx, cert, key) &&
          X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) == 1);
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
   return ctx;
}

static SSL_CTX *client_ctx(X509 *ca, X509 *cert, EVP_PKEY *key)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   assert(ctx && use_identity(ctx, cert, key) &&
          X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) == 1);
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   return ctx;
}

typedef struct
{
   SSL_CTX *ctx;
   int fd;
   int handshake_ok;
   int verified;
   kb_mgmt_status_peer_t peer;
} server_arg_t;

static void *accept_peer(void *opaque)
{
   server_arg_t *arg = opaque;
   SSL *ssl = SSL_new(arg->ctx);
   assert(ssl && SSL_set_fd(ssl, arg->fd) == 1);
   memset(&arg->peer, 0xa5, sizeof(arg->peer));
   arg->handshake_ok = SSL_accept(ssl) == 1;
   if (arg->handshake_ok)
      arg->verified = kb_mgmt_status_peer_verify(ssl, &arg->peer);
   else
      memset(&arg->peer, 0, sizeof(arg->peer));
   SSL_shutdown(ssl);
   SSL_free(ssl);
   return NULL;
}

static int all_zero(const void *p, size_t n)
{
   const unsigned char *b = p;
   for (size_t i = 0; i < n; i++)
      if (b[i])
         return 0;
   return 1;
}

static server_arg_t handshake(SSL_CTX *server, SSL_CTX *client)
{
   int fds[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
   server_arg_t arg = {.ctx = server, .fd = fds[0]};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, accept_peer, &arg) == 0);
   SSL *ssl = SSL_new(client);
   assert(ssl && SSL_set_fd(ssl, fds[1]) == 1);
   int client_ok = SSL_connect(ssl) == 1;
   SSL_shutdown(ssl);
   SSL_free(ssl);
   assert(pthread_join(thread, NULL) == 0);
   close(fds[0]);
   close(fds[1]);
   arg.handshake_ok = arg.handshake_ok && client_ok;
   return arg;
}

static void run_profile(X509 *ca, EVP_PKEY *ca_key, SSL_CTX *server, EVP_PKEY *client_key,
                        const client_profile_t *profile, int want_verified)
{
   static long serial = 0xA0;
   X509 *client_cert = make_leaf(ca, ca_key, client_key, serial++, 0, profile);
   SSL_CTX *client = client_ctx(ca, client_cert, client_key);
   server_arg_t result = handshake(server, client);
   assert(result.verified == want_verified);
   if (want_verified)
   {
      assert(result.handshake_ok && result.peer.issuer[0] && strlen(result.peer.fingerprint) == 64);
      assert(strcmp(result.peer.serial_norm, "a0") == 0);
      for (size_t i = 0; result.peer.serial_norm[i]; i++)
         assert(!isalpha((unsigned char)result.peer.serial_norm[i]) ||
                islower((unsigned char)result.peer.serial_norm[i]));
      for (size_t i = 0; result.peer.fingerprint[i]; i++)
         assert(isdigit((unsigned char)result.peer.fingerprint[i]) ||
                (result.peer.fingerprint[i] >= 'a' && result.peer.fingerprint[i] <= 'f'));
   }
   else
      assert(all_zero(&result.peer, sizeof(result.peer)));
   SSL_CTX_free(client);
   X509_free(client_cert);
}

int main(void)
{
   SSL_library_init();
   kb_mgmt_status_peer_t empty;
   memset(&empty, 0xa5, sizeof(empty));
   assert(kb_mgmt_status_peer_verify(NULL, &empty) == 0 && all_zero(&empty, sizeof(empty)));
   assert(kb_mgmt_status_peer_verify(NULL, NULL) == 0);

   EVP_PKEY *ca_key = make_key(), *server_key = make_key(), *client_key = make_key();
   X509 *ca = make_ca(ca_key, "test-management-ca");
   client_profile_t unused = {0};
   X509 *server_cert = make_leaf(ca, ca_key, server_key, 2, 1, &unused);
   SSL_CTX *server = server_ctx(ca, server_cert, server_key);
   const char *marker = "aimee-p5-kb-management-v1";

   client_profile_t valid = {1, 0, marker, "p5-kb-management", "clientAuth", 0};
   run_profile(ca, ca_key, server, client_key, &valid, 1);

   client_profile_t missing = {0, 0, marker, "p5-kb-management", "clientAuth", 0};
   client_profile_t wrong = {1, 0, "aimee-p5-kb-management-v2", "p5-kb-management",
                             "clientAuth", 0};
   client_profile_t critical = {1, 1, marker, "p5-kb-management", "clientAuth", 0};
   client_profile_t duplicate = {2, 0, marker, "p5-kb-management", "clientAuth", 0};
   client_profile_t wrong_cn = {1, 0, marker, "ordinary-client", "clientAuth", 0};
   client_profile_t duplicate_cn = {1, 0, marker, "p5-kb-management", "clientAuth", 1};
   client_profile_t extra_eku = {1, 0, marker, "p5-kb-management",
                                 "clientAuth,serverAuth", 0};
   run_profile(ca, ca_key, server, client_key, &missing, 0);
   run_profile(ca, ca_key, server, client_key, &wrong, 0);
   run_profile(ca, ca_key, server, client_key, &critical, 0);
   run_profile(ca, ca_key, server, client_key, &duplicate, 0);
   run_profile(ca, ca_key, server, client_key, &wrong_cn, 0);
   run_profile(ca, ca_key, server, client_key, &duplicate_cn, 0);
   run_profile(ca, ca_key, server, client_key, &extra_eku, 0);

   /* The exact profile is insufficient without a chain to the listener's trust
    * anchor: the TLS handshake rejects an identically-profiled foreign leaf. */
   EVP_PKEY *foreign_ca_key = make_key();
   X509 *foreign_ca = make_ca(foreign_ca_key, "foreign-management-ca");
   X509 *foreign_cert = make_leaf(foreign_ca, foreign_ca_key, client_key, 0xF0, 0, &valid);
   SSL_CTX *foreign_client = client_ctx(ca, foreign_cert, client_key);
   server_arg_t foreign_result = handshake(server, foreign_client);
   assert(!foreign_result.handshake_ok && !foreign_result.verified &&
          all_zero(&foreign_result.peer, sizeof(foreign_result.peer)));
   SSL_CTX_free(foreign_client);
   X509_free(foreign_cert);
   X509_free(foreign_ca);
   EVP_PKEY_free(foreign_ca_key);

   SSL_CTX_free(server);
   X509_free(server_cert);
   X509_free(ca);
   EVP_PKEY_free(client_key);
   EVP_PKEY_free(server_key);
   EVP_PKEY_free(ca_key);
   puts("kb_mgmt_status_peer: ok");
   return 0;
}
