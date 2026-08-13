#include <stdint.h>

#include "config_accessors.h"
#include "log.h"
#include "pki.h"
#include "server_conn_io.h"
#include "server_tls.h"

#include <assert.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Narrow link stubs: this test exercises only the dedicated context and exact
 * peer profile, never the generic config/roster path. */
static int g_default_mtls_mode;
static int g_server_cert_ensure_calls;
static int g_client_ca_ensure_calls;

const char *config_default_dir(void)
{
   return "/nonexistent";
}
int config_server_api_mtls(void)
{
   return g_default_mtls_mode;
}
const char *config_server_api_mtls_client_ca(void)
{
   return "/nonexistent/tls/client-ca.crt";
}
int pki_ca_ensure(void)
{
   g_client_ca_ensure_calls++;
   return -1;
}
int pki_is_revoked(const char *serial)
{
   (void)serial;
   return 1;
}
int pki_mtls_ramp_init(int mode)
{
   return mode;
}
int pki_ensure_self_signed_server_cert(const char *cert, const char *key)
{
   assert(strcmp(cert, "/nonexistent/tls/server.crt") == 0);
   assert(strcmp(key, "/nonexistent/tls/server.key") == 0);
   g_server_cert_ensure_calls++;
   return -1;
}
int pki_server_tls_key_load(char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   return -1;
}
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
void server_conn_io_set_ssl(int fd, SSL *ssl)
{
   (void)fd;
   (void)ssl;
}
void server_conn_io_clear(int fd)
{
   (void)fd;
}

static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   assert(fwrite(text, 1, strlen(text), f) == strlen(text));
   assert(fclose(f) == 0);
}

static void command(const char *cmd)
{
   assert(system(cmd) == 0);
}

static void read_text(const char *path, char *out, size_t cap)
{
   FILE *f = fopen(path, "rb");
   assert(f && cap > 1);
   size_t n = fread(out, 1, cap - 1, f);
   assert(!ferror(f) && feof(f) && fclose(f) == 0);
   out[n] = '\0';
}

typedef struct
{
   int fd;
   int profile;
   int management;
} server_arg_t;

static void *accept_one(void *opaque)
{
   server_arg_t *arg = opaque;
   SSL *ssl = arg->management ? server_tls_management_begin(arg->fd) : server_tls_begin(arg->fd);
   if (ssl)
   {
      if (arg->management)
      {
         server_tls_peer_cert_t peer;
         assert(server_tls_peer_cert(ssl, &peer) == 1);
         arg->profile = peer.management_profile;
      }
      else
      {
         char cn[257], serial[129];
         arg->profile = server_tls_peer_identity(ssl, cn, sizeof(cn), serial, sizeof(serial));
      }
      server_tls_end(arg->fd, ssl);
   }
   else
      arg->profile = -2;
   close(arg->fd);
   return NULL;
}

static int present_on(const char *ca, const char *cert, const char *key, int management)
{
   int sv[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
   server_arg_t arg = {.fd = sv[0], .profile = -1, .management = management};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, accept_one, &arg) == 0);

   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   assert(ctx);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   assert(SSL_CTX_load_verify_locations(ctx, ca, NULL) == 1);
   if (cert && key)
   {
      assert(SSL_CTX_use_certificate_chain_file(ctx, cert) == 1);
      assert(SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) == 1);
   }
   SSL *ssl = SSL_new(ctx);
   assert(ssl && SSL_set_fd(ssl, sv[1]) == 1);
   if (SSL_connect(ssl) == 1)
      SSL_shutdown(ssl);
   SSL_free(ssl);
   SSL_CTX_free(ctx);
   close(sv[1]);
   assert(pthread_join(thread, NULL) == 0);
   return arg.profile;
}

static int present(const char *ca, const char *cert, const char *key)
{
   return present_on(ca, cert, key, 1);
}

static int present_main(const char *ca, const char *cert, const char *key)
{
   return present_on(ca, cert, key, 0);
}

int main(void)
{
   signal(SIGPIPE, SIG_IGN);

   /* Fresh wizard installs use optional mTLS so the first bearer-authenticated
    * client can enroll its certificate. That mode must still provision the
    * server identity certificate needed to bring up HTTPS. Fail at the stubbed
    * client-CA step so this assertion stays independent of filesystem fixtures. */
   g_default_mtls_mode = 1;
   assert(server_tls_init_default() == -1);
   assert(g_server_cert_ensure_calls == 1);
   assert(g_client_ca_ensure_calls == 1);

   char dir[256];
   snprintf(dir, sizeof dir, "%s/aimee-mgmt-tls-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir));
   char ca[256], cakey[256], server[256], serverkey[256], client[256], clientkey[256], dual[256],
       dualkey[256], ca_server[256], ca_server_key[256], ca_client[256], ca_client_key[256],
       serverext[256], ext[256], dualext[256], ca_server_ext[256], ca_client_ext[256],
       other_ca[256], linkpath[256], cmd[4096];
#define PATH(name, suffix) snprintf(name, sizeof(name), "%s/%s", dir, suffix)
   PATH(ca, "ca.pem");
   PATH(cakey, "ca.key");
   PATH(server, "server.pem");
   PATH(serverkey, "server.key");
   PATH(client, "client.pem");
   PATH(clientkey, "client.key");
   PATH(dual, "dual.pem");
   PATH(dualkey, "dual.key");
   PATH(ca_server, "ca-server.pem");
   PATH(ca_server_key, "ca-server.key");
   PATH(ca_client, "ca-client.pem");
   PATH(ca_client_key, "ca-client.key");
   PATH(serverext, "server.ext");
   PATH(ext, "client.ext");
   PATH(dualext, "dual.ext");
   PATH(ca_server_ext, "ca-server.ext");
   PATH(ca_client_ext, "ca-client.ext");
   PATH(other_ca, "other-ca.pem");
   PATH(linkpath, "server-link.pem");
#undef PATH

   write_text(serverext,
              "basicConstraints=critical,CA:FALSE\n"
              "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n");
   write_text(ext, "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                   "extendedKeyUsage=clientAuth\n"
                   "1.3.6.1.4.1.55555.5.1=DER:"
                   "61696d65652d70352d6b622d6d616e6167656d656e742d7631\n");
   write_text(dualext, "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                       "extendedKeyUsage=clientAuth,serverAuth\n"
                       "1.3.6.1.4.1.55555.5.1=DER:"
                       "61696d65652d70352d6b622d6d616e6167656d656e742d7631\n");
   write_text(ca_server_ext,
              "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyCertSign\n"
              "extendedKeyUsage=serverAuth\n");
   write_text(ca_client_ext,
              "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyCertSign\n"
              "extendedKeyUsage=clientAuth\n"
              "1.3.6.1.4.1.55555.5.1=DER:"
              "61696d65652d70352d6b622d6d616e6167656d656e742d7631\n");
   snprintf(cmd, sizeof(cmd),
            "openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=management-ca -days 1 "
            "-keyout %s -out %s >/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=management-server -keyout %s "
            "-out %s/server.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/server.csr -CA %s -CAkey %s -CAcreateserial -days 1 "
            "-extfile %s -out %s >/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=p5-kb-management -keyout %s "
            "-out %s/client.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/client.csr -CA %s -CAkey %s -days 1 -extfile %s -out %s "
            ">/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=p5-kb-management -keyout %s "
            "-out %s/dual.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/dual.csr -CA %s -CAkey %s -days 1 -extfile %s -out %s "
            ">/dev/null 2>&1",
            cakey, ca, serverkey, dir, dir, ca, cakey, serverext, server, clientkey, dir, dir, ca,
            cakey, ext, client, dualkey, dir, dir, ca, cakey, dualext, dual);
   command(cmd);

   snprintf(cmd, sizeof(cmd),
            "openssl req -newkey rsa:2048 -nodes -subj /CN=management-server -keyout %s "
            "-out %s/ca-server.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/ca-server.csr -CA %s -CAkey %s -days 1 -extfile %s "
            "-out %s >/dev/null 2>&1 && "
            "openssl req -newkey rsa:2048 -nodes -subj /CN=p5-kb-management -keyout %s "
            "-out %s/ca-client.csr >/dev/null 2>&1 && "
            "openssl x509 -req -in %s/ca-client.csr -CA %s -CAkey %s -days 1 -extfile %s "
            "-out %s >/dev/null 2>&1",
            ca_server_key, dir, dir, ca, cakey, ca_server_ext, ca_server, ca_client_key, dir, dir,
            ca, cakey, ca_client_ext, ca_client);
   command(cmd);

   assert(symlink(server, linkpath) == 0);
   assert(server_tls_management_init(linkpath, serverkey, ca) == -1);
   assert(server_tls_management_init(ca_server, ca_server_key, ca) == -1);
   assert(server_tls_management_init(server, ca_server_key, ca) == -1);
   char server_key_pem[8192];
   read_text(serverkey, server_key_pem, sizeof(server_key_pem));
   assert(server_tls_management_init_vault(server, server_key_pem, ca) == 0);
   assert(server_tls_management_init_vault(server, server_key_pem, ca) == 0);
   OPENSSL_cleanse(server_key_pem, sizeof(server_key_pem));
   assert(present(ca, NULL, NULL) == -2);
   assert(present(ca, client, clientkey) == 1);
   assert(present(ca, dual, dualkey) == 0);
   assert(present(ca, ca_client, ca_client_key) != 1);

   /* The ordinary listener keeps the TLS handshake cert-optional even after
    * durable posture reaches required. HTTP authorization then admits only the
    * exact enrollment routes for a cert-less peer. The dedicated management
    * listener assertions above remain hard-required at the transport layer. */
   assert(server_tls_init(server, serverkey, 2, ca) == 0);
   assert(server_tls_mtls_mode() == 2);
   assert(present_main(ca, NULL, NULL) == 0);

   snprintf(cmd, sizeof(cmd), "cp %s %s && printf '\\n' >> %s", ca, other_ca, other_ca);
   command(cmd);
   assert(server_tls_management_init(server, serverkey, other_ca) == -1);

   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   command(cmd);
   puts("test_server_management_tls: ok");
   return 0;
}
