/* aimee_tls_schannel.c: Windows TLS client backend (WITH_TLS builds).
 *
 * Implements the aimee_tls.h 4-function contract on Schannel/SSPI, so the Windows
 * thin client speaks https:// with verification against the Windows certificate
 * store — no OpenSSL, no bundled CA bundle, single self-contained exe.
 *
 * Contract parity with the OpenSSL backend (aimee_tls.c):
 *   - TLS >= 1.2 (SCHANNEL_CRED.grbitEnabledProtocols = TLS 1.2[/1.3]; 1.0/1.1 off).
 *   - chain AND hostname verified post-handshake. Schannel does NOT check the host
 *     name itself, so we do CertGetCertificateChain +
 *     CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, pwszServerName=host).
 *   - AIMEE_TLS_INSECURE=1 (read at connect time) skips that verification entirely.
 *   - opaque handle owns the security context; aimee_tls_free does NOT close the fd.
 *
 * Read path keeps two carry-over buffers in the handle: leftover ciphertext
 * (SECBUFFER_EXTRA from DecryptMessage / the tail of the handshake) and
 * decrypted-but-undelivered plaintext (when the caller's buffer is smaller than a
 * record). Mid-stream SEC_I_RENEGOTIATE is handled by re-running the handshake.
 */
#define SECURITY_WIN32
/* Ensure CNG / modern crypt32 symbols (NCrypt*, CERT_NCRYPT_KEY_SPEC) are
 * exposed before any Windows header is pulled in. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "aimee_tls.h"
#include "aimee_home.h"

#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>
#include <ncrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
#endif
/* CNG / NCrypt fallbacks for older MinGW headers (same defensive pattern as the
 * SP_PROT_* defines above). */
#ifndef NCRYPT_PKCS8_PRIVATE_KEY_BLOB
#define NCRYPT_PKCS8_PRIVATE_KEY_BLOB L"PKCS8_PRIVATEKEY"
#endif
#ifndef MS_KEY_STORAGE_PROVIDER
#define MS_KEY_STORAGE_PROVIDER L"Microsoft Software Key Storage Provider"
#endif
#ifndef CERT_NCRYPT_KEY_SPEC
#define CERT_NCRYPT_KEY_SPEC 0xFFFFFFFF
#endif

#define ENC_CAP 32768 /* one TLS record fits comfortably; grown via recv loop */

struct aimee_tls
{
   int fd;
   CredHandle cred;
   CtxtHandle ctx;
   int have_cred, have_ctx;
   SecPkgContext_StreamSizes sizes;
   char *host; /* for hostname verification + renegotiation re-verify */

   unsigned char enc[ENC_CAP]; /* leftover ciphertext */
   size_t enc_len;

   unsigned char *dec; /* decrypted plaintext not yet delivered */
   size_t dec_off, dec_len, dec_cap;

   /* mTLS client identity (present when <aimee_home>/tls/client.{crt,key} loaded). */
   PCCERT_CONTEXT client_cert;
   NCRYPT_PROV_HANDLE client_prov;
   NCRYPT_KEY_HANDLE client_key;
};

static int tls_insecure(void)
{
   const char *v = getenv("AIMEE_TLS_INSECURE");
   return v && *v && strcmp(v, "0") != 0;
}

/* Blocking socket helpers. */
static int send_all(int fd, const void *buf, size_t len)
{
   const char *p = buf;
   size_t off = 0;
   while (off < len)
   {
      int n = send(fd, p + off, (int)(len - off), 0);
      if (n > 0)
      {
         off += (size_t)n;
         continue;
      }
      if (n == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
         continue;
      return -1;
   }
   return 0;
}

/* Append up to one recv() of ciphertext into t->enc. Returns bytes read, 0 on a
 * clean close, -1 on error. */
static int recv_more(aimee_tls_t *t)
{
   if (t->enc_len >= ENC_CAP)
      return -1; /* a single record should never exceed ENC_CAP */
   int n = recv(t->fd, (char *)t->enc + t->enc_len, (int)(ENC_CAP - t->enc_len), 0);
   if (n > 0)
   {
      t->enc_len += (size_t)n;
      return n;
   }
   if (n == 0)
      return 0;
   if (WSAGetLastError() == WSAEINTR)
      return recv_more(t);
   return -1;
}

/* Verify the server certificate chain + hostname against the Windows store.
 * Returns 0 on success, -1 on any failure. */
static int verify_server_cert(aimee_tls_t *t)
{
   /* Fail closed if there is no hostname to verify against: a NULL pwszServerName
    * makes CERT_CHAIN_POLICY_SSL skip the name check, which would accept any
    * otherwise-valid cert (MITM). aimee_client.c always passes the URL host. */
   if (!t->host || !t->host[0])
      return -1;

   PCCERT_CONTEXT cert = NULL;
   if (QueryContextAttributes(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert)
      return -1;

   int ok = -1;
   PCCERT_CHAIN_CONTEXT chain = NULL;
   CERT_CHAIN_PARA chain_para;
   memset(&chain_para, 0, sizeof(chain_para));
   chain_para.cbSize = sizeof(chain_para);

   if (CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore, &chain_para, 0, NULL, &chain))
   {
      /* Widen the hostname for the SSL policy. A DNS hostname is <= 253 chars, so
       * 256 wide chars suffices; a 0 return means truncation/encoding error -> fail
       * closed rather than verify against a truncated name. */
      wchar_t whost[256];
      if (MultiByteToWideChar(CP_UTF8, 0, t->host, -1, whost, 256) == 0)
      {
         CertFreeCertificateChain(chain);
         CertFreeCertificateContext(cert);
         return -1;
      }

      SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para;
      memset(&ssl_para, 0, sizeof(ssl_para));
      ssl_para.cbSize = sizeof(ssl_para);
      ssl_para.dwAuthType = AUTHTYPE_SERVER;
      ssl_para.pwszServerName = whost;

      CERT_CHAIN_POLICY_PARA policy_para;
      memset(&policy_para, 0, sizeof(policy_para));
      policy_para.cbSize = sizeof(policy_para);
      policy_para.pvExtraPolicyPara = &ssl_para;

      CERT_CHAIN_POLICY_STATUS policy_status;
      memset(&policy_status, 0, sizeof(policy_status));
      policy_status.cbSize = sizeof(policy_status);

      if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy_para,
                                           &policy_status) &&
          policy_status.dwError == 0)
         ok = 0;

      CertFreeCertificateChain(chain);
   }
   CertFreeCertificateContext(cert);
   return ok;
}

/* Drive the client handshake to completion. |initial| flags the first call (no
 * input token). Returns 0 on success, -1 on failure. Any ciphertext past the
 * Finished message is left in t->enc for the read path. */
static int do_handshake(aimee_tls_t *t, int initial)
{
   DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
               ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
   SECURITY_STATUS ss;
   int first = initial;

   for (;;)
   {
      SecBuffer inbuf[2];
      SecBufferDesc indesc;
      SecBuffer outbuf[1];
      SecBufferDesc outdesc;
      DWORD out_flags = 0;

      if (!first)
      {
         /* Need at least some bytes to feed ISC. */
         if (t->enc_len == 0)
         {
            int n = recv_more(t);
            if (n <= 0)
               return -1;
         }
      }

      inbuf[0].pvBuffer = t->enc;
      inbuf[0].cbBuffer = (unsigned long)t->enc_len;
      inbuf[0].BufferType = SECBUFFER_TOKEN;
      inbuf[1].pvBuffer = NULL;
      inbuf[1].cbBuffer = 0;
      inbuf[1].BufferType = SECBUFFER_EMPTY;
      indesc.ulVersion = SECBUFFER_VERSION;
      indesc.cBuffers = 2;
      indesc.pBuffers = inbuf;

      outbuf[0].pvBuffer = NULL;
      outbuf[0].cbBuffer = 0;
      outbuf[0].BufferType = SECBUFFER_TOKEN;
      outdesc.ulVersion = SECBUFFER_VERSION;
      outdesc.cBuffers = 1;
      outdesc.pBuffers = outbuf;

      ss = InitializeSecurityContextA(&t->cred, (first || !t->have_ctx) ? NULL : &t->ctx,
                                      (SEC_CHAR *)t->host, req, 0, 0, first ? NULL : &indesc, 0,
                                      &t->ctx, &outdesc, &out_flags, NULL);
      if (first)
         t->have_ctx = 1;
      first = 0;

      /* Send any token Schannel produced (even on CONTINUE / OK). */
      if ((ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED ||
           (FAILED(ss) && (out_flags & ISC_RET_EXTENDED_ERROR))) &&
          outbuf[0].cbBuffer && outbuf[0].pvBuffer)
      {
         int rc = send_all(t->fd, outbuf[0].pvBuffer, outbuf[0].cbBuffer);
         FreeContextBuffer(outbuf[0].pvBuffer);
         if (rc != 0)
            return -1;
      }
      else if (outbuf[0].pvBuffer)
      {
         FreeContextBuffer(outbuf[0].pvBuffer);
      }

      if (ss == SEC_E_INCOMPLETE_MESSAGE)
      {
         int n = recv_more(t);
         if (n <= 0)
            return -1;
         continue; /* feed the larger buffer back in */
      }

      /* Carry over any unconsumed ciphertext (SECBUFFER_EXTRA). */
      if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer)
      {
         memmove(t->enc, t->enc + (t->enc_len - inbuf[1].cbBuffer), inbuf[1].cbBuffer);
         t->enc_len = inbuf[1].cbBuffer;
      }
      else if (ss != SEC_E_INCOMPLETE_MESSAGE)
      {
         t->enc_len = 0;
      }

      if (ss == SEC_I_CONTINUE_NEEDED)
         continue;
      if (ss == SEC_E_OK)
         return 0;
      return -1; /* any hard error */
   }
}

/* Read a whole file (<= 1 MiB) into a malloc'd buffer; *out_len set on success. */
static unsigned char *read_small_file(const char *path, DWORD *out_len)
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
   unsigned char *b = malloc((size_t)sz);
   if (!b)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(b, 1, (size_t)sz, f);
   fclose(f);
   if (rd != (size_t)sz)
   {
      free(b);
      return NULL;
   }
   *out_len = (DWORD)sz;
   return b;
}

/* Decode the first base64 PEM block (-----BEGIN/END-----) to DER. */
static int pem_to_der(const unsigned char *pem, DWORD pem_len, unsigned char **der, DWORD *der_len)
{
   DWORD n = 0;
   if (!CryptStringToBinaryA((LPCSTR)pem, pem_len, CRYPT_STRING_BASE64HEADER, NULL, &n, NULL, NULL))
      return -1;
   unsigned char *d = malloc(n ? n : 1);
   if (!d)
      return -1;
   if (!CryptStringToBinaryA((LPCSTR)pem, pem_len, CRYPT_STRING_BASE64HEADER, d, &n, NULL, NULL))
   {
      free(d);
      return -1;
   }
   *der = d;
   *der_len = n;
   return 0;
}

/* Load <aimee_home>/tls/client.{crt,key} (PEM cert + PKCS#8 EC key) as this
 * client's identity: decode to DER, import the key into an ephemeral CNG store,
 * and bind it to the cert context. Returns 1 (loaded -> t->client_cert set),
 * 0 (no material configured), or -1 (present but failed to load). The server's
 * key is PKCS#8 (OpenSSL PEM_write_bio_PrivateKey), so NCRYPT_PKCS8_PRIVATE_KEY_BLOB
 * imports it directly. (NTFS ACLs, not POSIX mode bits, govern key secrecy here.) */
static int schannel_load_client_identity(aimee_tls_t *t)
{
   const char *home = aimee_home();
   if (!home || !*home)
      return 0;
   char crt[600], key[600];
   snprintf(crt, sizeof(crt), "%s/tls/client.crt", home);
   snprintf(key, sizeof(key), "%s/tls/client.key", home);
   if (GetFileAttributesA(crt) == INVALID_FILE_ATTRIBUTES ||
       GetFileAttributesA(key) == INVALID_FILE_ATTRIBUTES)
      return 0; /* no client-cert material configured */

   int rc = -1;
   unsigned char *cpem = NULL, *kpem = NULL, *cder = NULL, *kder = NULL;
   DWORD clen = 0, klen = 0, cdl = 0, kdl = 0;
   PCCERT_CONTEXT cert = NULL;
   NCRYPT_PROV_HANDLE prov = 0;
   NCRYPT_KEY_HANDLE hkey = 0;

#define DBG(...)                                                                                   \
   do                                                                                              \
   {                                                                                               \
      if (getenv("AIMEE_TLS_DEBUG"))                                                               \
         fprintf(stderr, "aimee.tls.schannel: " __VA_ARGS__);                                      \
   } while (0)

   cpem = read_small_file(crt, &clen);
   kpem = read_small_file(key, &klen);
   if (!cpem || !kpem)
   {
      DBG("read_small_file failed (cpem=%p kpem=%p)\n", (void *)cpem, (void *)kpem);
      goto done;
   }
   if (pem_to_der(cpem, clen, &cder, &cdl) != 0 || pem_to_der(kpem, klen, &kder, &kdl) != 0)
   {
      DBG("pem_to_der failed (cder=%p kder=%p)\n", (void *)cder, (void *)kder);
      goto done;
   }
   DBG("decoded cert=%lu der bytes, key=%lu der bytes\n", (unsigned long)cdl, (unsigned long)kdl);

   cert = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, cder, cdl);
   if (!cert)
   {
      DBG("CertCreateCertificateContext failed: 0x%lx\n", (unsigned long)GetLastError());
      goto done;
   }
   {
      SECURITY_STATUS ss = NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
      if (ss != ERROR_SUCCESS)
      {
         DBG("NCryptOpenStorageProvider failed: 0x%lx\n", (unsigned long)ss);
         goto done;
      }
      ss = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &hkey, kder, kdl, 0);
      if (ss != ERROR_SUCCESS)
      {
         DBG("NCryptImportKey(PKCS8) failed: 0x%lx\n", (unsigned long)ss);
         goto done;
      }
   }

   {
      CERT_KEY_CONTEXT kc;
      memset(&kc, 0, sizeof(kc));
      kc.cbSize = sizeof(kc);
      kc.hNCryptKey = hkey;
      kc.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
      if (!CertSetCertificateContextProperty(cert, CERT_KEY_CONTEXT_PROP_ID, 0, &kc))
      {
         DBG("CertSetCertificateContextProperty failed: 0x%lx\n", (unsigned long)GetLastError());
         goto done;
      }
   }
   DBG("client identity loaded OK\n");

   t->client_cert = cert;
   t->client_prov = prov;
   t->client_key = hkey;
   cert = NULL; /* ownership transferred to the handle */
   prov = 0;
   hkey = 0;
   rc = 1;

done:
   if (kder)
   {
      SecureZeroMemory(kder, kdl); /* DER holds the private key */
      free(kder);
   }
   if (kpem)
   {
      SecureZeroMemory(kpem, klen);
      free(kpem);
   }
   free(cder);
   free(cpem);
   if (hkey)
      NCryptFreeObject(hkey);
   if (prov)
      NCryptFreeObject(prov);
   if (cert)
      CertFreeCertificateContext(cert);
   return rc;
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   aimee_tls_t *t = calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   t->fd = fd;
   if (host && *host)
      t->host = _strdup(host);

   SCHANNEL_CRED sc;
   memset(&sc, 0, sizeof(sc));
   sc.dwVersion = SCHANNEL_CRED_VERSION;
   sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
#ifdef SP_PROT_TLS1_3_CLIENT
   sc.grbitEnabledProtocols |= SP_PROT_TLS1_3_CLIENT;
#endif
   /* We validate the chain + hostname ourselves post-handshake, so tell Schannel
    * not to auto-validate (and never use a machine default client cert). */
   sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;
   /* mTLS client-cert presentation: if <aimee_home>/tls/client.{crt,key} loads,
    * hand Schannel the cert (with its CNG key) so the server can identify this
    * client as a distinct cert:<CN> principal. Absent => plain client TLS. A
    * load failure (-1) leaves the handshake cert-less; the mTLS server then
    * rejects it, which is the correct signal for broken client-cert material. */
   if (schannel_load_client_identity(t) == 1)
   {
      sc.cCreds = 1;
      sc.paCred = &t->client_cert;
   }

   if (AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc,
                                 NULL, NULL, &t->cred, NULL) != SEC_E_OK)
   {
      aimee_tls_free(t);
      return NULL;
   }
   t->have_cred = 1;

   if (do_handshake(t, 1) != 0)
   {
      aimee_tls_free(t);
      return NULL;
   }
   if (!tls_insecure() && verify_server_cert(t) != 0)
   {
      aimee_tls_free(t);
      return NULL;
   }
   if (QueryContextAttributes(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK)
   {
      aimee_tls_free(t);
      return NULL;
   }
   return t;
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   if (!t || !t->have_ctx)
      return -1;
   const unsigned char *p = buf;
   size_t off = 0;
   unsigned long maxmsg = t->sizes.cbMaximumMessage;
   if (maxmsg == 0)
      return -1; /* guard against a zero chunk size -> would never make progress */
   size_t reclen = (size_t)t->sizes.cbHeader + maxmsg + t->sizes.cbTrailer;
   unsigned char *rec = malloc(reclen);
   if (!rec)
      return -1;

   int rc = 0;
   while (off < len)
   {
      unsigned long chunk = (unsigned long)((len - off) < maxmsg ? (len - off) : maxmsg);
      memcpy(rec + t->sizes.cbHeader, p + off, chunk);

      SecBuffer b[4];
      b[0].pvBuffer = rec;
      b[0].cbBuffer = t->sizes.cbHeader;
      b[0].BufferType = SECBUFFER_STREAM_HEADER;
      b[1].pvBuffer = rec + t->sizes.cbHeader;
      b[1].cbBuffer = chunk;
      b[1].BufferType = SECBUFFER_DATA;
      b[2].pvBuffer = rec + t->sizes.cbHeader + chunk;
      b[2].cbBuffer = t->sizes.cbTrailer;
      b[2].BufferType = SECBUFFER_STREAM_TRAILER;
      b[3].pvBuffer = NULL;
      b[3].cbBuffer = 0;
      b[3].BufferType = SECBUFFER_EMPTY;
      SecBufferDesc d;
      d.ulVersion = SECBUFFER_VERSION;
      d.cBuffers = 4;
      d.pBuffers = b;

      if (EncryptMessage(&t->ctx, 0, &d, 0) != SEC_E_OK)
      {
         rc = -1;
         break;
      }
      if (send_all(t->fd, rec, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer) != 0)
      {
         rc = -1;
         break;
      }
      off += chunk;
   }
   free(rec);
   return rc;
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   if (!t || !t->have_ctx)
      return -1;
   if (len == 0)
      return 0;

   /* Deliver any buffered plaintext first. */
   if (t->dec_len > t->dec_off)
   {
      size_t n = t->dec_len - t->dec_off;
      if (n > len)
         n = len;
      memcpy(buf, t->dec + t->dec_off, n);
      t->dec_off += n;
      if (t->dec_off >= t->dec_len)
         t->dec_off = t->dec_len = 0;
      return (long)n;
   }

   for (;;)
   {
      if (t->enc_len == 0)
      {
         int n = recv_more(t);
         if (n == 0)
            return 0; /* EOF */
         if (n < 0)
            return -1;
      }

      SecBuffer b[4];
      b[0].pvBuffer = t->enc;
      b[0].cbBuffer = (unsigned long)t->enc_len;
      b[0].BufferType = SECBUFFER_DATA;
      b[1].BufferType = SECBUFFER_EMPTY;
      b[2].BufferType = SECBUFFER_EMPTY;
      b[3].BufferType = SECBUFFER_EMPTY;
      SecBufferDesc d;
      d.ulVersion = SECBUFFER_VERSION;
      d.cBuffers = 4;
      d.pBuffers = b;

      SECURITY_STATUS ss = DecryptMessage(&t->ctx, &d, 0, NULL);

      if (ss == SEC_E_INCOMPLETE_MESSAGE)
      {
         int n = recv_more(t);
         if (n <= 0)
            return (n == 0) ? 0 : -1;
         continue;
      }
      if (ss == SEC_I_CONTEXT_EXPIRED)
         return 0; /* peer sent close-notify */
      if (ss == SEC_I_RENEGOTIATE)
      {
         /* Server asked to renegotiate: continue the handshake, then re-verify. */
         if (do_handshake(t, 0) != 0)
            return -1;
         if (!tls_insecure() && verify_server_cert(t) != 0)
            return -1;
         continue;
      }
      if (ss != SEC_E_OK)
         return -1;

      /* Locate decrypted plaintext + leftover ciphertext. */
      SecBuffer *data = NULL, *extra = NULL;
      for (int i = 0; i < 4; i++)
      {
         if (b[i].BufferType == SECBUFFER_DATA && !data)
            data = &b[i];
         else if (b[i].BufferType == SECBUFFER_EXTRA && !extra)
            extra = &b[i];
      }

      size_t produced = data ? data->cbBuffer : 0;
      long delivered = 0;
      if (produced)
      {
         size_t give = produced < len ? produced : len;
         memcpy(buf, data->pvBuffer, give);
         delivered = (long)give;
         if (give < produced)
         {
            /* Stash the remainder for the next read. */
            size_t rem = produced - give;
            if (rem > t->dec_cap)
            {
               unsigned char *nb = realloc(t->dec, rem);
               if (!nb)
                  return -1;
               t->dec = nb;
               t->dec_cap = rem;
            }
            memcpy(t->dec, (unsigned char *)data->pvBuffer + give, rem);
            t->dec_off = 0;
            t->dec_len = rem;
         }
      }

      /* Carry leftover ciphertext to the front of enc for the next record. */
      if (extra && extra->cbBuffer)
      {
         memmove(t->enc, extra->pvBuffer, extra->cbBuffer);
         t->enc_len = extra->cbBuffer;
      }
      else
      {
         t->enc_len = 0;
      }

      if (delivered > 0)
         return delivered;
      /* A record with zero application bytes (e.g. a handshake message): loop. */
   }
}

void aimee_tls_free(aimee_tls_t *t)
{
   if (!t)
      return;
   if (t->have_ctx)
   {
      /* Best-effort close-notify. */
      DWORD type = SCHANNEL_SHUTDOWN;
      SecBuffer sb;
      sb.pvBuffer = &type;
      sb.cbBuffer = sizeof(type);
      sb.BufferType = SECBUFFER_TOKEN;
      SecBufferDesc sd;
      sd.ulVersion = SECBUFFER_VERSION;
      sd.cBuffers = 1;
      sd.pBuffers = &sb;
      if (ApplyControlToken(&t->ctx, &sd) == SEC_E_OK)
      {
         DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                     ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
         SecBuffer ob;
         ob.pvBuffer = NULL;
         ob.cbBuffer = 0;
         ob.BufferType = SECBUFFER_TOKEN;
         SecBufferDesc od;
         od.ulVersion = SECBUFFER_VERSION;
         od.cBuffers = 1;
         od.pBuffers = &ob;
         DWORD of = 0;
         if (InitializeSecurityContextA(&t->cred, &t->ctx, NULL, req, 0, 0, NULL, 0, &t->ctx, &od,
                                        &of, NULL) == SEC_E_OK &&
             ob.cbBuffer && ob.pvBuffer)
         {
            (void)send_all(t->fd, ob.pvBuffer, ob.cbBuffer);
            FreeContextBuffer(ob.pvBuffer);
         }
      }
      DeleteSecurityContext(&t->ctx);
   }
   if (t->have_cred)
      FreeCredentialsHandle(&t->cred);
   if (t->client_cert)
      CertFreeCertificateContext(t->client_cert);
   if (t->client_key)
      NCryptFreeObject(t->client_key);
   if (t->client_prov)
      NCryptFreeObject(t->client_prov);
   free(t->dec);
   free(t->host);
   free(t);
}
