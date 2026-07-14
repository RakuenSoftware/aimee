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
#include "platform_net.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

   /* Create the transient keychain under the temp dir, unique per connection
    * (pid + atomic counter) so concurrent connections in one process don't race
    * on the same path. */
   static volatile int s_ctr;
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !*tmp)
      tmp = "/tmp";
   char kcpath[800];
   snprintf(kcpath, sizeof(kcpath), "%s/aimee-mtls-%d-%d.keychain", tmp, (int)getpid(),
            __sync_add_and_fetch(&s_ctr, 1));
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

/* Resolve <aimee_home>/remote-ca.pem — the pinned server certificate written by
 * `aimee remote set/trust`. Returns 1 (and fills |out|) when the file exists,
 * else 0. Mirrors pinned_ca_path() in the OpenSSL backend (aimee_tls.c). */
static int pinned_ca_path(char *out, size_t n)
{
   const char *home = aimee_home();
   if (!home || !*home || !out || n == 0)
      return 0;
   snprintf(out, n, "%s/remote-ca.pem", home);
   struct stat st;
   return (stat(out, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int b64_val(int c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   return -1; /* whitespace / non-alphabet (skipped by the decoder) */
}

/* Decode base64 |in| (in_len bytes), skipping whitespace and stopping at the
 * first '=' pad, into a malloc'd buffer; sets *out_len. Caller frees. NULL on
 * OOM. */
static unsigned char *b64_decode(const char *in, size_t in_len, size_t *out_len)
{
   unsigned char *out = malloc(in_len / 4 * 3 + 3);
   if (!out)
      return NULL;
   size_t o = 0;
   int quad[4], qn = 0;
   for (size_t i = 0; i < in_len; i++)
   {
      int c = (unsigned char)in[i];
      if (c == '=')
         break;
      int v = b64_val(c);
      if (v < 0)
         continue;
      quad[qn++] = v;
      if (qn == 4)
      {
         out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
         out[o++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
         out[o++] = (unsigned char)((quad[2] << 6) | quad[3]);
         qn = 0;
      }
   }
   if (qn == 2)
      out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
   else if (qn == 3)
   {
      out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
      out[o++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
   }
   *out_len = o;
   return out;
}

/* Load the first CERTIFICATE block from a PEM file at |path|, decode it to DER,
 * and wrap it as a SecCertificateRef. Returns a retained ref (caller releases)
 * or NULL on any failure. */
static SecCertificateRef securetransport_load_pinned_cert(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
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
   char *buf = malloc((size_t)sz + 1);
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
   buf[sz] = '\0'; /* NUL-terminate so the PEM markers can be located with strstr */

   SecCertificateRef cert = NULL;
   static const char begin[] = "-----BEGIN CERTIFICATE-----";
   static const char end[] = "-----END CERTIFICATE-----";
   char *b = strstr(buf, begin);
   if (b)
   {
      b += sizeof(begin) - 1; /* body starts after the BEGIN marker line */
      char *e = strstr(b, end);
      if (e && e > b)
      {
         size_t der_len = 0;
         unsigned char *der = b64_decode(b, (size_t)(e - b), &der_len);
         if (der)
         {
            CFDataRef data = CFDataCreate(NULL, der, (CFIndex)der_len);
            if (data)
            {
               cert = SecCertificateCreateWithData(NULL, data); /* NULL on bad DER */
               CFRelease(data);
            }
            free(der);
         }
      }
   }
   free(buf);
   return cert;
}

/* During a BreakOnServerAuth handshake, decide the pinned-mode trust: the
 * identity is an EXACT leaf match against the TOFU-pinned certificate (SSH
 * known_hosts semantics), matching the OpenSSL and Windows backends. The
 * hostname/SAN policy is deliberately NOT consulted: a containerized/NAT'd
 * server cannot know its reachable address at cert-mint time, so its
 * self-signed cert routinely lacks the SAN the client dials — while the
 * byte-exact pin is a stronger identity than any name match (a MITM cannot
 * present the pinned cert without its private key; any other cert, even a
 * validly-issued one, fails the DER compare). Returns 1 if trusted, 0 not.
 * (|host| is retained in the signature for symmetry/logging call sites.) */
static int securetransport_eval_pinned_trust(SSLContextRef ctx, const char *host,
                                             SecCertificateRef pinned)
{
   (void)host;
   SecTrustRef trust = NULL;
   if (SSLCopyPeerTrust(ctx, &trust) != noErr || !trust)
      return 0;

   int ok = 0;
   SecCertificateRef leaf =
       (SecTrustGetCertificateCount(trust) > 0) ? SecTrustGetCertificateAtIndex(trust, 0) : NULL;
   if (leaf && pinned)
   {
      CFDataRef leaf_der = SecCertificateCopyData(leaf);
      CFDataRef pin_der = SecCertificateCopyData(pinned);
      if (leaf_der && pin_der && CFDataGetLength(leaf_der) == CFDataGetLength(pin_der) &&
          memcmp(CFDataGetBytePtr(leaf_der), CFDataGetBytePtr(pin_der),
                 (size_t)CFDataGetLength(leaf_der)) == 0)
         ok = 1;
      if (leaf_der)
         CFRelease(leaf_der);
      if (pin_der)
         CFRelease(pin_der);
   }
   CFRelease(trust);
   return ok;
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
   /* When a pinned server cert is recorded for this remote, trust the system
    * store AND that cert. The Keychain auto-eval can't be handed an extra anchor,
    * so switch to the BreakOnServerAuth flow and evaluate manually below.
    * Verification stays ON (chain + hostname/SAN). */
   SecCertificateRef pinned = NULL;
   if (!insecure)
   {
      char pin[700];
      if (pinned_ca_path(pin, sizeof(pin)))
         pinned = securetransport_load_pinned_cert(pin); /* NULL if unparseable */
   }

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
         if (pinned)
            CFRelease(pinned);
         CFRelease(t->ctx);
         free(t);
         return NULL;
      }
      /* Set the expected name (also drives SNI). For the non-pinned path Secure
       * Transport then verifies chain + hostname (SAN/CN, wildcards) against the
       * Keychain trust during the handshake. */
      SSLSetPeerDomainName(t->ctx, host, strlen(host));
      if (pinned)
      {
         /* Defer trust to our manual, pinned-anchor evaluation below. */
         SSLSetSessionOption(t->ctx, kSSLSessionOptionBreakOnServerAuth, true);
      }
   }

   OSStatus rc;
   for (;;)
   {
      rc = SSLHandshake(t->ctx);
      if (rc == errSSLWouldBlock)
         continue;
      if (rc == errSSLPeerAuthCompleted)
      {
         if (insecure)
            continue; /* accept without evaluation */
         if (pinned)
         {
            /* Manually evaluate against system + pinned anchors with a hostname
             * policy. On success resume the handshake to completion; on failure
             * force a hard error so the connect fails closed (MITM-rejecting). */
            if (securetransport_eval_pinned_trust(t->ctx, host, pinned))
               continue;
            rc = errSSLXCertChainInvalid;
            break;
         }
         /* Non-pinned secure path never sets BreakOnServerAuth, so this branch is
          * unreachable there; break defensively. */
         break;
      }
      break;
   }
   if (pinned)
      CFRelease(pinned);

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

/* Base64-encode |der| (der_len bytes) and wrap it as a PEM CERTIFICATE block
 * (64-column lines, with header/footer and trailing newline). Returns a malloc'd
 * NUL-terminated string (caller frees) or NULL on OOM. */
static char *der_to_pem(const unsigned char *der, size_t der_len)
{
   static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   size_t enc_len = (der_len + 2) / 3 * 4;
   char *enc = malloc(enc_len + 1);
   if (!enc)
      return NULL;
   size_t o = 0, i = 0;
   while (i + 3 <= der_len)
   {
      unsigned v = ((unsigned)der[i] << 16) | ((unsigned)der[i + 1] << 8) | der[i + 2];
      enc[o++] = b64[(v >> 18) & 63];
      enc[o++] = b64[(v >> 12) & 63];
      enc[o++] = b64[(v >> 6) & 63];
      enc[o++] = b64[v & 63];
      i += 3;
   }
   if (der_len - i == 1)
   {
      unsigned v = (unsigned)der[i] << 16;
      enc[o++] = b64[(v >> 18) & 63];
      enc[o++] = b64[(v >> 12) & 63];
      enc[o++] = '=';
      enc[o++] = '=';
   }
   else if (der_len - i == 2)
   {
      unsigned v = ((unsigned)der[i] << 16) | ((unsigned)der[i + 1] << 8);
      enc[o++] = b64[(v >> 18) & 63];
      enc[o++] = b64[(v >> 12) & 63];
      enc[o++] = b64[(v >> 6) & 63];
      enc[o++] = '=';
   }
   enc[o] = '\0';

   static const char hdr[] = "-----BEGIN CERTIFICATE-----\n";
   static const char ftr[] = "-----END CERTIFICATE-----\n";
   size_t lines = (o + 63) / 64; /* one '\n' per wrapped 64-col line */
   char *pem = malloc((sizeof(hdr) - 1) + o + lines + (sizeof(ftr) - 1) + 1);
   if (!pem)
   {
      free(enc);
      return NULL;
   }
   size_t p = 0;
   memcpy(pem + p, hdr, sizeof(hdr) - 1);
   p += sizeof(hdr) - 1;
   for (size_t j = 0; j < o; j += 64)
   {
      size_t chunk = (o - j < 64) ? (o - j) : 64;
      memcpy(pem + p, enc + j, chunk);
      p += chunk;
      pem[p++] = '\n';
   }
   memcpy(pem + p, ftr, sizeof(ftr) - 1);
   p += sizeof(ftr) - 1;
   pem[p] = '\0';
   free(enc);
   return pem;
}

/* Open a fresh, NON-verifying TLS connection to host:port (trust-on-first-use:
 * we are fetching the cert to surface its fingerprint and pin it, not trusting
 * it yet — mirrors the OpenSSL backend). Emits the leaf cert as PEM in *pem_out
 * (malloc'd, caller frees) and its SHA-256 as uppercase colon-hex in fp_out.
 * Returns 0 on success, -1 on failure (with *pem_out=NULL, fp_out[0]=0). */
int aimee_tls_fetch_peer_cert(const char *host, const char *port, char **pem_out, char *fp_out,
                              size_t fp_n)
{
   if (pem_out)
      *pem_out = NULL;
   if (fp_out && fp_n)
      fp_out[0] = '\0';
   if (!host || !*host || !port || !*port || !pem_out)
      return -1;

   int fd = platform_net_connect(host, port, 10000);
   if (fd < 0)
      return -1;

   SSLContextRef ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
   if (!ctx)
   {
      platform_net_close(fd);
      return -1;
   }

   int rc = -1;
   if (SSLSetIOFuncs(ctx, st_read, st_write) == noErr && SSLSetConnection(ctx, &fd) == noErr &&
       SSLSetProtocolVersionMin(ctx, kTLSProtocol12) == noErr &&
       SSLSetSessionOption(ctx, kSSLSessionOptionBreakOnServerAuth, true) == noErr)
   {
      /* SNI: many servers select a cert by it. (For an IP literal this is not a
       * valid SNI host, but the server we are fetching from ignores it.) */
      SSLSetPeerDomainName(ctx, host, strlen(host));

      OSStatus h;
      do
      {
         h = SSLHandshake(ctx);
      } while (h == errSSLWouldBlock || h == errSSLPeerAuthCompleted); /* accept w/o eval */

      if (h == noErr)
      {
         SecTrustRef trust = NULL;
         if (SSLCopyPeerTrust(ctx, &trust) == noErr && trust)
         {
            /* Leaf is index 0; SecTrustGetCertificateAtIndex is deprecated on
             * macOS 12 but the TU is built -Wno-deprecated-declarations and it
             * returns a non-owned ref (no release). */
            SecCertificateRef leaf = SecTrustGetCertificateAtIndex(trust, 0);
            if (leaf)
            {
               CFDataRef der = SecCertificateCopyData(leaf);
               if (der)
               {
                  const unsigned char *bytes = CFDataGetBytePtr(der);
                  size_t dlen = (size_t)CFDataGetLength(der);
                  char *pem = der_to_pem(bytes, dlen);
                  if (pem)
                  {
                     *pem_out = pem;
                     rc = 0;
                     if (fp_out && fp_n)
                     {
                        unsigned char md[CC_SHA256_DIGEST_LENGTH];
                        CC_SHA256(bytes, (CC_LONG)dlen, md);
                        size_t o = 0;
                        for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH && o + 4 < fp_n; i++)
                           o += (size_t)snprintf(fp_out + o, fp_n - o, i ? ":%02X" : "%02X", md[i]);
                     }
                  }
                  CFRelease(der);
               }
            }
            CFRelease(trust);
         }
      }
      SSLClose(ctx);
   }

   CFRelease(ctx);
   platform_net_close(fd); /* the SSLConnectionRef does not own the fd */
   return rc;
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
