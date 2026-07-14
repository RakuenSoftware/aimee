/* test_aimee_tls_pin.c — TOFU pin-is-identity for the thin client's TLS.
 *
 * A pinned server certificate (<aimee_home>/remote-ca.pem, written by
 * `aimee remote set/trust`) must be accepted on an EXACT leaf match alone —
 * SSH known_hosts semantics — with the hostname/SAN check waived: a
 * containerized/NAT'd aimee-server cannot know its reachable LAN address at
 * cert-mint time, so its self-signed cert routinely lacks the SAN the client
 * dials (the 192.168.1.254 appliance failure). Any OTHER cert must still be
 * rejected, pinned or not. Real handshakes over a socketpair, mirroring
 * test_kb_tls.c. */
#include "aimee_tls.h"
#include "kb_pki.h"

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[256];

static void write_pin(const char *cert_pem)
{
   char p[320];
   snprintf(p, sizeof(p), "%s/remote-ca.pem", g_home);
   FILE *f = fopen(p, "w");
   assert(f);
   fputs(cert_pem, f);
   fclose(f);
}

static void remove_pin(void)
{
   char p[320];
   snprintf(p, sizeof(p), "%s/remote-ca.pem", g_home);
   unlink(p);
}

/* Plain server-auth TLS server context from PEM strings (no client-cert
 * requirement — the thin client presents none here). */
static SSL_CTX *server_ctx_from_pem(const char *cert_pem, const char *key_pem)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   assert(ctx);
   BIO *cb = BIO_new_mem_buf(cert_pem, -1);
   X509 *crt = PEM_read_bio_X509(cb, NULL, NULL, NULL);
   BIO_free(cb);
   BIO *kb = BIO_new_mem_buf(key_pem, -1);
   EVP_PKEY *key = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
   BIO_free(kb);
   assert(crt && key);
   assert(SSL_CTX_use_certificate(ctx, crt) == 1);
   assert(SSL_CTX_use_PrivateKey(ctx, key) == 1);
   X509_free(crt);
   EVP_PKEY_free(key);
   return ctx;
}

static void *server_thread(void *a)
{
   SSL *ssl = (SSL *)a;
   /* A rejected client aborts mid-handshake; SSL_accept failing is fine. */
   if (SSL_accept(ssl) == 1)
      SSL_shutdown(ssl);
   return NULL;
}

/* Full handshake: aimee_tls_connect() as the client against an in-process
 * server. Returns 1 when the client accepted the server's identity. */
static int client_accepts(SSL_CTX *server_ctx, const char *host)
{
   int sv[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
   SSL *s = SSL_new(server_ctx);
   assert(s);
   SSL_set_fd(s, sv[0]);
   pthread_t th;
   assert(pthread_create(&th, NULL, server_thread, s) == 0);

   aimee_tls_t *t = aimee_tls_connect(sv[1], host);
   int ok = (t != NULL);
   aimee_tls_free(t);
   /* Close the client fd BEFORE joining: a client that aborted the handshake
    * leaves the server blocked in SSL_accept until it sees EOF. */
   close(sv[1]);
   pthread_join(th, NULL);
   SSL_free(s);
   close(sv[0]);
   return ok;
}

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   signal(SIGPIPE, SIG_IGN); /* an aborted handshake writes into a closed pipe */
   printf("aimee_tls_pin:\n");

   /* Isolated home so remote-ca.pem is under test control; strict verification
    * (the insecure escape hatch must not mask a verify failure). */
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-tlspin-%d", (int)getpid());
   mkdir(g_home, 0700);
   setenv("AIMEE_HOME", g_home, 1);
   unsetenv("AIMEE_TLS_INSECURE");

   /* A self-signed-CA server cert for "kb.local" — its SANs cover kb.local and
    * loopback, NOT the address the client dials below (the NAT/appliance case). */
   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0);
   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(&ca, "kb.local", 3600, scert, sizeof(scert), skey,
                                   sizeof(skey)) == 0);
   SSL_CTX *server = server_ctx_from_pem(scert, skey);

   /* 1. Pin == leaf, dialed by an address ABSENT from the cert's SANs: the pin
    *    is the identity, so the handshake must succeed (the regression). */
   write_pin(scert);
   assert(client_accepts(server, "203.0.113.9") == 1);
   printf("  pin_match_san_mismatch_accepted: ok\n");

   /* 2. Pin == leaf, matching hostname: still accepted (sanity). */
   assert(client_accepts(server, "kb.local") == 1);
   printf("  pin_match_name_match_accepted: ok\n");

   /* 3. Pin holds a DIFFERENT cert: the server must be rejected even though its
    *    name matches — only the pinned leaf is acceptable in pinned mode. */
   {
      kb_pki_ca_t other;
      assert(kb_pki_ca_generate(&other) == 0);
      char ocert[KB_PKI_CERT_PEM_MAX], okey[KB_PKI_KEY_PEM_MAX];
      assert(kb_pki_issue_server_cert(&other, "kb.local", 3600, ocert, sizeof(ocert), okey,
                                      sizeof(okey)) == 0);
      write_pin(ocert);
      assert(client_accepts(server, "kb.local") == 0);
      printf("  pin_mismatch_rejected: ok\n");
   }

   /* 4. No pin: a self-signed server fails system-store verification. */
   remove_pin();
   assert(client_accepts(server, "kb.local") == 0);
   printf("  unpinned_selfsigned_rejected: ok\n");

   /* 5. Unparseable pin fails CLOSED (no silent fallback to the system store). */
   write_pin("not a certificate\n");
   assert(client_accepts(server, "kb.local") == 0);
   printf("  corrupt_pin_fails_closed: ok\n");

   SSL_CTX_free(server);
   printf("aimee_tls_pin: all tests passed\n");
   return 0;
}
