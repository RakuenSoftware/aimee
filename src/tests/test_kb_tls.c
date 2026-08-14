/* test_kb_tls.c — loopback mutual-TLS handshake over a socketpair, using certs
 * issued by the internal CA (kb_pki). Proves: a CA-issued client + server cert
 * complete a mutual handshake, the server extracts the client's scope from its
 * cert CN, and a peer holding a FOREIGN-CA cert is rejected on both sides. */
#include "kb_pki.h"
#include "kb_tls.h"

#include <openssl/ssl.h>
#include <openssl/pem.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
   SSL_CTX *ctx;
   int fd;
   int ok;
   char peer_cn[128];
} srv_arg_t;

static int cert_keys_equal(const char *a_pem, const char *b_pem)
{
   BIO *a_bio = BIO_new_mem_buf(a_pem, -1);
   BIO *b_bio = BIO_new_mem_buf(b_pem, -1);
   X509 *a = a_bio ? PEM_read_bio_X509(a_bio, NULL, NULL, NULL) : NULL;
   X509 *b = b_bio ? PEM_read_bio_X509(b_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *a_key = a ? X509_get_pubkey(a) : NULL;
   EVP_PKEY *b_key = b ? X509_get_pubkey(b) : NULL;
   int equal = a_key && b_key && EVP_PKEY_eq(a_key, b_key) == 1;
   EVP_PKEY_free(a_key);
   EVP_PKEY_free(b_key);
   X509_free(a);
   X509_free(b);
   BIO_free(a_bio);
   BIO_free(b_bio);
   return equal;
}

static void *server_thread(void *a)
{
   srv_arg_t *s = (srv_arg_t *)a;
   SSL *ssl = SSL_new(s->ctx);
   SSL_set_fd(ssl, s->fd);
   if (SSL_accept(ssl) == 1)
   {
      char issuer[KB_TLS_PEER_ISSUER_MAX + 1] = "";
      char serial[KB_TLS_PEER_SERIAL_MAX + 1] = "";
      char tiny[2] = "x";
      s->ok = kb_tls_peer_cn(ssl, s->peer_cn, sizeof(s->peer_cn)) == 0 &&
              kb_tls_peer_issuer(ssl, issuer, sizeof(issuer)) == 0 && issuer[0] &&
              kb_tls_peer_serial(ssl, serial, sizeof(serial)) == 0 && serial[0] &&
              kb_tls_peer_cn(ssl, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0' &&
              kb_tls_peer_issuer(ssl, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0' &&
              kb_tls_peer_serial(ssl, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0';
   }
   SSL_shutdown(ssl);
   SSL_free(ssl);
   return NULL;
}

/* Run a full handshake; returns 1 if BOTH sides succeeded, copying the client's
 * CN (as seen by the server) into peer_cn_out. */
static int handshake(SSL_CTX *server_ctx, SSL_CTX *client_ctx, char *peer_cn_out, size_t cap)
{
   int sv[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
   srv_arg_t sa;
   memset(&sa, 0, sizeof(sa));
   sa.ctx = server_ctx;
   sa.fd = sv[0];
   pthread_t th;
   assert(pthread_create(&th, NULL, server_thread, &sa) == 0);

   SSL *c = SSL_new(client_ctx);
   SSL_set_fd(c, sv[1]);
   int client_ok = (SSL_connect(c) == 1);
   SSL_shutdown(c);
   SSL_free(c);
   pthread_join(th, NULL);
   close(sv[0]);
   close(sv[1]);

   if (peer_cn_out && cap)
      snprintf(peer_cn_out, cap, "%s", sa.peer_cn);
   return client_ok && sa.ok;
}

int main(void)
{
   printf("kb_tls:\n");

   /* Our CA, a server cert, and a client cert scoped "project:alpha". */
   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0);
   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(&ca, "kb.local", 3600, scert, sizeof(scert), skey,
                                   sizeof(skey)) == 0);
   char ccert[KB_PKI_CERT_PEM_MAX], ckey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&ca, "project:alpha", 3600, ccert, sizeof(ccert), ckey,
                                   sizeof(ckey)) == 0);
   assert(!cert_keys_equal(scert, ccert));

   /* Pair roles are exact. A serverAuth leaf cannot be loaded as this hop's
    * client identity, and a clientAuth leaf cannot be loaded as its server. */
   assert(kb_tls_server_ctx(ca.cert_pem, ccert, ckey) == NULL);
   assert(kb_tls_client_ctx(ca.cert_pem, scert, skey) == NULL);

   SSL_CTX *server_ctx = kb_tls_server_ctx(ca.cert_pem, scert, skey);
   SSL_CTX *client_ctx = kb_tls_client_ctx(ca.cert_pem, ccert, ckey);
   assert(server_ctx && client_ctx);

   /* 1. Mutual handshake succeeds; the server reads the client's scope. */
   char cn[128] = "";
   assert(handshake(server_ctx, client_ctx, cn, sizeof(cn)) == 1);
   assert(strcmp(cn, "project:alpha") == 0);
   printf("  mutual_auth: ok\n");

   /* A fresh pair issues distinct pair-specific material for both roles. */
   char scert2[KB_PKI_CERT_PEM_MAX], skey2[KB_PKI_KEY_PEM_MAX];
   char ccert2[KB_PKI_CERT_PEM_MAX], ckey2[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(&ca, "kb.local", 3600, scert2, sizeof(scert2), skey2,
                                   sizeof(skey2)) == 0);
   assert(kb_pki_issue_client_cert(&ca, "project:alpha", 3600, ccert2, sizeof(ccert2), ckey2,
                                   sizeof(ckey2)) == 0);
   assert(strcmp(scert, scert2) != 0 && strcmp(ccert, ccert2) != 0);
   assert(!cert_keys_equal(scert2, ccert2));
   SSL_CTX *server_ctx2 = kb_tls_server_ctx(ca.cert_pem, scert2, skey2);
   SSL_CTX *client_ctx2 = kb_tls_client_ctx(ca.cert_pem, ccert2, ckey2);
   assert(server_ctx2 && client_ctx2 && handshake(server_ctx2, client_ctx2, NULL, 0) == 1);
   SSL_CTX_free(server_ctx2);
   SSL_CTX_free(client_ctx2);
   printf("  fresh_pair_material: ok\n");

   /* 2. A client whose cert is from a FOREIGN CA is rejected by the server. */
   {
      kb_pki_ca_t other;
      assert(kb_pki_ca_generate(&other) == 0);
      char ocert[KB_PKI_CERT_PEM_MAX], okey[KB_PKI_KEY_PEM_MAX];
      assert(kb_pki_issue_client_cert(&other, "project:evil", 3600, ocert, sizeof(ocert), okey,
                                      sizeof(okey)) == 0);
      SSL_CTX *evil_client = kb_tls_client_ctx(other.cert_pem, ocert, okey);
      assert(evil_client);
      assert(handshake(server_ctx, evil_client, NULL, 0) == 0); /* server rejects */
      SSL_CTX_free(evil_client);
      printf("  untrusted_client_rejected: ok\n");
   }

   /* 3. A client that trusts a DIFFERENT CA rejects our server. */
   {
      kb_pki_ca_t other;
      assert(kb_pki_ca_generate(&other) == 0);
      char ocert[KB_PKI_CERT_PEM_MAX], okey[KB_PKI_KEY_PEM_MAX];
      assert(kb_pki_issue_client_cert(&other, "c", 3600, ocert, sizeof(ocert), okey,
                                      sizeof(okey)) == 0);
      /* client presents a valid (other-CA) cert but trusts only `other` as the
       * server anchor, so it rejects our `ca`-signed server cert. */
      SSL_CTX *wrong_anchor = kb_tls_client_ctx(other.cert_pem, ocert, okey);
      assert(wrong_anchor);
      assert(handshake(server_ctx, wrong_anchor, NULL, 0) == 0); /* client rejects */
      SSL_CTX_free(wrong_anchor);
      printf("  wrong_server_anchor_rejected: ok\n");
   }

   /* context builders reject bad PEM. */
   assert(kb_tls_server_ctx(ca.cert_pem, "garbage", skey) == NULL);
   assert(kb_tls_client_ctx("garbage", ccert, ckey) == NULL);

   SSL_CTX_free(server_ctx);
   SSL_CTX_free(client_ctx);
   printf("All kb_tls tests passed.\n");
   return 0;
}
