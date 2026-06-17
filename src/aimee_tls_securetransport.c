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
#define __STDC_WANT_LIB_EXT1__ 1 /* expose memset_s for a non-elidable key wipe */
#include "aimee_tls.h"
#include "aimee_home.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct aimee_tls
{
   SSLContextRef ctx;
   int fd; /* the SSLConnectionRef points here; not closed by this module */
   /* Transient keychain holding the mTLS client identity (deleted on free) so the
    * imported private key never lands in the user's login keychain. */
   SecKeychainRef client_keychain;
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

/* Load the mTLS client identity from <aimee_home>/tls/client.p12 for
 * SSLSetCertificate. SSLSetCertificate wants a CFArray whose first element is a
 * SecIdentityRef; the on-disk path to one is SecPKCS12Import. To avoid leaking
 * the private key into the user's login keychain (plain SecPKCS12Import imports
 * there and it persists across runs), import into a TRANSIENT keychain that is
 * deleted on aimee_tls_free. Export the PEM client.{crt,key} the OpenSSL/Schannel
 * backends use to client.p12 via `openssl pkcs12 -export`. NOTE: macOS
 * SecPKCS12Import requires a NON-EMPTY passphrase (an empty one fails
 * errSecAuthFailed), so the .p12 must be passphrase-protected and the passphrase
 * supplied via AIMEE_TLS_CLIENT_P12_PASS. Returns a retained CFArrayRef
 * [identity] (caller releases after SSLSetCertificate) and sets *out_kc to the
 * transient keychain, or NULL/none on absence or import failure. */
static CFArrayRef securetransport_load_client_identity(SecKeychainRef *out_kc)
{
   *out_kc = NULL;
   const char *home = aimee_home();
   if (!home || !*home)
      return NULL;
   char path[700];
   snprintf(path, sizeof(path), "%s/tls/client.p12", home);

   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL; /* no client-cert material configured */
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz <= 0 || sz > (1 << 20) || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   unsigned char *buf = malloc((size_t)sz);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   if (rd != (size_t)sz)
   {
      free(buf);
      return NULL;
   }

   CFDataRef data = CFDataCreate(NULL, buf, sz);
   memset_s(buf, (rsize_t)sz, 0, (rsize_t)sz); /* PKCS#12 holds the private key */
   free(buf);
   if (!data)
      return NULL;

   /* Create the transient keychain under the temp dir, unique per process. */
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !*tmp)
      tmp = "/tmp";
   char kcpath[800];
   snprintf(kcpath, sizeof(kcpath), "%s/aimee-mtls-%d.keychain", tmp, (int)getpid());
   unlink(kcpath); /* SecKeychainCreate fails if the file already exists */
   static const char kcpw[] = "aimee-transient-mtls";
   SecKeychainRef kc = NULL;
   if (SecKeychainCreate(kcpath, (UInt32)(sizeof(kcpw) - 1), kcpw, false, NULL, &kc) !=
           errSecSuccess ||
       !kc)
   {
      CFRelease(data);
      return NULL;
   }

   const char *passenv = getenv("AIMEE_TLS_CLIENT_P12_PASS");
   CFStringRef pass =
       CFStringCreateWithCString(NULL, passenv ? passenv : "", kCFStringEncodingUTF8);
   const void *keys[] = {kSecImportExportPassphrase, kSecImportExportKeychain};
   const void *vals[] = {pass, kc};
   CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
   CFArrayRef items = NULL;
   OSStatus st = opts ? SecPKCS12Import(data, opts, &items) : errSecParam;
   CFArrayRef result = NULL;
   if (st == errSecSuccess && items && CFArrayGetCount(items) > 0)
   {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, 0);
      SecIdentityRef ident = (SecIdentityRef)CFDictionaryGetValue(item, kSecImportItemIdentity);
      if (ident)
      {
         const void *certs[] = {ident};
         result = CFArrayCreate(NULL, certs, 1, &kCFTypeArrayCallBacks);
      }
   }
   if (items)
      CFRelease(items);
   if (opts)
      CFRelease(opts);
   if (pass)
      CFRelease(pass);
   CFRelease(data);

   if (result)
   {
      *out_kc = kc; /* keep alive for the handshake; deleted on aimee_tls_free */
   }
   else
   {
      SecKeychainDelete(kc);
      CFRelease(kc);
   }
   return result;
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
   /* mTLS client-cert presentation: if <aimee_home>/tls/client.p12 holds an
    * identity, present it so the server can identify this client as a distinct
    * cert:<CN> principal. Absent => plain client TLS. SSLSetCertificate retains
    * the array contents, so we release our reference immediately after; the
    * transient keychain backing the identity is held in t and deleted on free. */
   CFArrayRef client_identity = securetransport_load_client_identity(&t->client_keychain);
   if (client_identity)
   {
      SSLSetCertificate(t->ctx, client_identity);
      CFRelease(client_identity);
   }
   if (SSLSetIOFuncs(t->ctx, st_read, st_write) != noErr ||
       SSLSetConnection(t->ctx, &t->fd) != noErr ||
       SSLSetProtocolVersionMin(t->ctx, kTLSProtocol12) != noErr)
   {
      aimee_tls_free(t);
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
      aimee_tls_free(t); /* also sends close-notify + deletes the transient keychain */
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
   if (t->client_keychain)
   {
      SecKeychainDelete(t->client_keychain); /* remove the transient .keychain + its key */
      CFRelease(t->client_keychain);
   }
   free(t);
}
