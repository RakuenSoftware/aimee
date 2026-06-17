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
#include "aimee_tls.h"

#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>

#include <stdlib.h>
#include <string.h>

#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
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
   /* mTLS client-cert presentation (slice 3b, follow-up): to present
    * <aimee_home>/tls/client.{crt,key} as this client's identity, build a
    * CERT_CONTEXT (PEM -> CertCreateCertificateContext) with an associated CNG
    * private key and set sc.cCreds/sc.paCred here. Deferred: EC-key import via
    * CryptoAPI is intricate and must be validated on real Windows (as the
    * native backend itself was). The OpenSSL backend presents the cert today. */

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
   free(t->dec);
   free(t->host);
   free(t);
}
