/* aimee_tls_securetransport.c: macOS TLS client backend (WITH_TLS builds).
 *
 * Implements the aimee_tls.h 4-function contract on Apple Secure Transport
 * (SSLContextRef). Chosen over OpenSSL on macOS because the release artifact is a
 * universal (arm64+x86_64) binary and the system framework is already universal,
 * so no per-arch OpenSSL is needed; trust is evaluated against the Keychain with
 * no bundled CA bundle.
 *
 * Secure Transport is deprecated since macOS 10.15 but still ships; the single
 * translation unit is built with -Wno-deprecated-declarations (see CMakeLists.txt)
 * so the deprecation warnings don't trip -Werror. If Apple removes it, migrate to
 * Network.framework behind this same interface.
 *
 * Contract parity with the OpenSSL backend (aimee_tls.c):
 *   - TLS >= 1.2 (SSLSetProtocolVersionMin).
 *   - hostname verification on by default via SSLSetPeerDomainName (Secure
 *     Transport evaluates the chain AND the name during the handshake).
 *   - AIMEE_TLS_INSECURE=1 (read at connect time) disables ALL verification.
 *   - the opaque handle owns the TLS state; aimee_tls_free does NOT close the fd.
 */
#include "aimee_tls.h"

#include <Security/SecureTransport.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct aimee_tls
{
   SSLContextRef ctx;
   int fd; /* the SSLConnectionRef points here; not closed by this module */
};

static int tls_insecure(void)
{
   const char *v = getenv("AIMEE_TLS_INSECURE");
   return v && *v && strcmp(v, "0") != 0;
}

/* Secure Transport I/O callbacks over the blocking socket. They must fill/drain
 * the full requested length or report a status; *len is updated to what moved. */
static OSStatus st_read(SSLConnectionRef conn, void *data, size_t *len)
{
   int fd = *(const int *)conn;
   size_t want = *len, got = 0;
   OSStatus rc = noErr;
   while (got < want)
   {
      ssize_t n = read(fd, (char *)data + got, want - got);
      if (n > 0)
      {
         got += (size_t)n;
         continue;
      }
      if (n == 0)
      {
         rc = errSSLClosedGraceful;
         break;
      }
      if (errno == EINTR)
         continue;
      rc = (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock : errSSLClosedAbort;
      break;
   }
   *len = got;
   return rc;
}

static OSStatus st_write(SSLConnectionRef conn, const void *data, size_t *len)
{
   int fd = *(const int *)conn;
   size_t want = *len, sent = 0;
   OSStatus rc = noErr;
   while (sent < want)
   {
      ssize_t n = write(fd, (const char *)data + sent, want - sent);
      if (n > 0)
      {
         sent += (size_t)n;
         continue;
      }
      if (n < 0 && errno == EINTR)
         continue;
      rc = (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock : errSSLClosedAbort;
      break;
   }
   *len = sent;
   return rc;
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   aimee_tls_t *t = calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   t->fd = fd;

   t->ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
   if (!t->ctx)
   {
      free(t);
      return NULL;
   }
   /* mTLS client-cert presentation (slice 3b, follow-up): to present
    * <aimee_home>/tls/client.{crt,key} as this client's identity, build a
    * SecIdentityRef from the PEM (SecPKCS12Import / SecItem) and pass it to
    * SSLSetCertificate here. Deferred: identity construction must be validated
    * on real macOS. The OpenSSL backend presents the cert today. */
   if (SSLSetIOFuncs(t->ctx, st_read, st_write) != noErr ||
       SSLSetConnection(t->ctx, &t->fd) != noErr ||
       SSLSetProtocolVersionMin(t->ctx, kTLSProtocol12) != noErr)
   {
      CFRelease(t->ctx);
      free(t);
      return NULL;
   }

   int insecure = tls_insecure();
   if (insecure)
   {
      /* Break on server auth so we can accept the cert WITHOUT evaluating it. */
      SSLSetSessionOption(t->ctx, kSSLSessionOptionBreakOnServerAuth, true);
   }
   else
   {
      /* Fail closed if there is no hostname: without SSLSetPeerDomainName, Secure
       * Transport verifies the chain but NOT the name, accepting any otherwise-valid
       * cert (MITM). aimee_client.c always passes the URL host. */
      if (!host || !*host)
      {
         CFRelease(t->ctx);
         free(t);
         return NULL;
      }
      /* Set the expected name: Secure Transport then verifies chain + hostname
       * (SAN/CN, wildcards) against the Keychain trust during the handshake. */
      SSLSetPeerDomainName(t->ctx, host, strlen(host));
   }

   OSStatus rc;
   do
   {
      rc = SSLHandshake(t->ctx);
   } while (rc == errSSLWouldBlock || (insecure && rc == errSSLPeerAuthCompleted));

   if (rc != noErr)
   {
      SSLClose(t->ctx);
      CFRelease(t->ctx);
      free(t);
      return NULL;
   }
   return t;
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   if (!t || !t->ctx)
      return -1;
   size_t off = 0;
   while (off < len)
   {
      size_t wrote = 0;
      OSStatus rc = SSLWrite(t->ctx, (const char *)buf + off, len - off, &wrote);
      off += wrote;
      if (rc == noErr || rc == errSSLWouldBlock)
         continue; /* blocking transport: keep going until all bytes are out */
      return -1;
   }
   return 0;
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   if (!t || !t->ctx)
      return -1;
   /* SSLRead buffers undelivered plaintext internally, so no manual carry-over is
    * needed here (unlike Schannel). */
   size_t got = 0;
   OSStatus rc = SSLRead(t->ctx, buf, len, &got);
   if (got > 0)
      return (long)got; /* deliver available bytes even if the call also signalled */
   if (rc == noErr)
      return 0;
   if (rc == errSSLClosedGraceful || rc == errSSLClosedNoNotify)
      return 0; /* clean EOF */
   return -1;
}

void aimee_tls_free(aimee_tls_t *t)
{
   if (!t)
      return;
   if (t->ctx)
   {
      SSLClose(t->ctx); /* send close-notify; does not close the fd */
      CFRelease(t->ctx);
   }
   free(t);
}
