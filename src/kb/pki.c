/* pki.c: aimee-kb's internal certificate authority. See kb_pki.h.
 * (distributed-mode-auth proposal, PKI phase.)
 *
 * Self-signed CA + client-cert issuance + sha256 CA fingerprint + full X509
 * chain verification, all over PEM strings. RSA-2048, SHA-256 signatures. */
#include "kb_pki.h"
#include "../modules/vault/vault_server_key.h"
#include "../modules/vault/vault_crypto.h"
#include <openssl/crypto.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int kb_pki_ca_load_custodied(const char *dir, kb_pki_ca_t *out);

static kb_pki_ca_load_result_t custodied_read(const char *path, char *out, size_t cap)
{
   if (!path || !out || cap < 2)
      return KB_PKI_CA_LOAD_INVALID;
   OPENSSL_cleanse(out, cap);
   int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return errno == ELOOP || errno == ENOTDIR ? KB_PKI_CA_LOAD_INTEGRITY
                                                : KB_PKI_CA_LOAD_UNAVAILABLE;
   struct stat st;
   if (fstat(fd, &st) || !S_ISREG(st.st_mode))
   {
      close(fd);
      return KB_PKI_CA_LOAD_INTEGRITY;
   }
   size_t used = 0;
   while (used < cap - 1)
   {
      ssize_t n = read(fd, out + used, cap - 1 - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         close(fd);
         OPENSSL_cleanse(out, cap);
         return KB_PKI_CA_LOAD_UNAVAILABLE;
      }
   }
   char extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   int close_error = close(fd) != 0;
   if (more < 0 || close_error)
   {
      OPENSSL_cleanse(out, cap);
      return KB_PKI_CA_LOAD_UNAVAILABLE;
   }
   if (!used || more > 0)
   {
      OPENSSL_cleanse(out, cap);
      return KB_PKI_CA_LOAD_INTEGRITY;
   }
   if (memchr(out, 0, used))
   {
      OPENSSL_cleanse(out, cap);
      return KB_PKI_CA_LOAD_INTEGRITY;
   }
   out[used] = 0;
   return KB_PKI_CA_LOAD_OK;
}

static int hx(const uint8_t *in, size_t n, char *out, size_t cap)
{
   static const char d[] = "0123456789abcdef";
   if (cap < n * 2 + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
   {
      out[i * 2] = d[in[i] >> 4];
      out[i * 2 + 1] = d[in[i] & 15];
   }
   out[n * 2] = 0;
   return 0;
}
static int unhx(const char *in, size_t n, uint8_t *out, size_t cap)
{
   if ((n & 1) || cap < n / 2)
      return -1;
   for (size_t i = 0; i < n / 2; i++)
   {
      unsigned char ac = (unsigned char)in[i * 2], bc = (unsigned char)in[i * 2 + 1];
      if (!((ac >= '0' && ac <= '9') || (ac >= 'a' && ac <= 'f')) ||
          !((bc >= '0' && bc <= '9') || (bc >= 'a' && bc <= 'f')))
         return -1;
      int a = ac <= '9' ? ac - '0' : ac - 'a' + 10;
      int b = bc <= '9' ? bc - '0' : bc - 'a' + 10;
      out[i] = (uint8_t)((a << 4) | b);
   }
   return 0;
}

/* --- small PEM <-> object helpers --- */

/* Serialize an X509 cert to PEM into out[cap]. Returns 0 / -1. */
static int pem_from_x509(X509 *cert, char *out, size_t cap)
{
   BIO *bio = BIO_new(BIO_s_mem());
   if (!bio)
      return -1;
   int rc = -1;
   if (PEM_write_bio_X509(bio, cert) == 1)
   {
      BUF_MEM *bm = NULL;
      BIO_get_mem_ptr(bio, &bm);
      if (bm && bm->length > 0 && bm->length < cap)
      {
         memcpy(out, bm->data, bm->length);
         out[bm->length] = '\0';
         rc = 0;
      }
   }
   BIO_free(bio);
   return rc;
}

/* Serialize a private key to unencrypted PKCS#8 PEM into out[cap]. 0 / -1. */
static int pem_from_key(EVP_PKEY *key, char *out, size_t cap)
{
   BIO *bio = BIO_new(BIO_s_mem());
   if (!bio)
      return -1;
   int rc = -1;
   if (PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) == 1)
   {
      BUF_MEM *bm = NULL;
      BIO_get_mem_ptr(bio, &bm);
      if (bm && bm->length > 0 && bm->length < cap)
      {
         memcpy(out, bm->data, bm->length);
         out[bm->length] = '\0';
         rc = 0;
      }
   }
   BIO_free(bio);
   return rc;
}

static X509 *x509_from_pem(const char *pem)
{
   if (!pem || !pem[0])
      return NULL;
   BIO *bio = BIO_new_mem_buf(pem, -1);
   if (!bio)
      return NULL;
   X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
   BIO_free(bio);
   return cert;
}

static EVP_PKEY *key_from_pem(const char *pem)
{
   if (!pem || !pem[0])
      return NULL;
   BIO *bio = BIO_new_mem_buf(pem, -1);
   if (!bio)
      return NULL;
   EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
   BIO_free(bio);
   return key;
}

/* Add an X509v3 extension (by NID) configured from `value` to `subject`,
 * with `issuer` as the extension context. Returns 1 on success, 0 on failure. */
static int add_ext(X509 *issuer, X509 *subject, int nid, const char *value)
{
   X509V3_CTX ctx;
   X509V3_set_ctx_nodb(&ctx);
   X509V3_set_ctx(&ctx, issuer, subject, NULL, NULL, 0);
   X509_EXTENSION *ex = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
   if (!ex)
      return 0;
   int rc = X509_add_ext(subject, ex, -1);
   X509_EXTENSION_free(ex);
   return rc == 1;
}

static int add_management_profile_ext(X509 *cert)
{
   ASN1_OBJECT *oid = OBJ_txt2obj("1.3.6.1.4.1.55555.5.1", 1);
   ASN1_OCTET_STRING *value = ASN1_OCTET_STRING_new();
   static const unsigned char marker[] = "aimee-p5-kb-management-v1";
   X509_EXTENSION *ext = NULL;
   int ok = oid && value && ASN1_OCTET_STRING_set(value, marker, sizeof(marker) - 1) == 1 &&
            (ext = X509_EXTENSION_create_by_OBJ(NULL, oid, 0, value)) != NULL &&
            X509_add_ext(cert, ext, -1) == 1;
   X509_EXTENSION_free(ext);
   ASN1_OCTET_STRING_free(value);
   ASN1_OBJECT_free(oid);
   return ok;
}

/* Set a random 64-bit positive serial number on `cert`. Returns 1/0. */
static int set_random_serial(X509 *cert)
{
   unsigned char bytes[8];
   if (RAND_bytes(bytes, sizeof(bytes)) != 1)
      return 0;
   bytes[0] &= 0x7F; /* keep it positive */
   BIGNUM *bn = BN_bin2bn(bytes, sizeof(bytes), NULL);
   if (!bn)
      return 0;
   ASN1_INTEGER *serial = BN_to_ASN1_INTEGER(bn, NULL);
   BN_free(bn);
   if (!serial)
      return 0;
   int rc = X509_set_serialNumber(cert, serial);
   ASN1_INTEGER_free(serial);
   return rc == 1;
}

/* --- CA generation --- */

int kb_pki_ca_generate(kb_pki_ca_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   int rc = -1;
   EVP_PKEY *key = EVP_RSA_gen(2048);
   X509 *cert = X509_new();
   if (!key || !cert)
      goto done;

   X509_set_version(cert, 2); /* X509 v3 */
   if (!set_random_serial(cert))
      goto done;
   X509_gmtime_adj(X509_getm_notBefore(cert), 0);
   X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365 * 10); /* ~10 years */
   if (X509_set_pubkey(cert, key) != 1)
      goto done;

   X509_NAME *name = X509_get_subject_name(cert);
   if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"aimee-kb-ca",
                                  -1, -1, 0) != 1)
      goto done;
   if (X509_set_issuer_name(cert, name) != 1) /* self-signed */
      goto done;

   /* CA extensions. subjectKeyIdentifier first (authorityKeyIdentifier can
    * reference it). issuer == subject for a self-signed CA. */
   if (!add_ext(cert, cert, NID_basic_constraints, "critical,CA:TRUE") ||
       !add_ext(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign") ||
       !add_ext(cert, cert, NID_subject_key_identifier, "hash") ||
       !add_ext(cert, cert, NID_authority_key_identifier, "keyid:always"))
      goto done;

   if (X509_sign(cert, key, EVP_sha256()) == 0)
      goto done;

   if (pem_from_x509(cert, out->cert_pem, sizeof(out->cert_pem)) != 0 ||
       pem_from_key(key, out->key_pem, sizeof(out->key_pem)) != 0)
      goto done;
   rc = 0;

done:
   if (rc != 0)
      memset(out, 0, sizeof(*out));
   X509_free(cert);
   EVP_PKEY_free(key);
   return rc;
}

/* --- CA fingerprint: sha256 of the cert DER --- */

int kb_pki_ca_fingerprint(const char *ca_cert_pem, char *hex_out, size_t cap)
{
   if (!hex_out || cap < KB_PKI_FP_HEX)
      return -1;
   X509 *cert = x509_from_pem(ca_cert_pem);
   if (!cert)
      return -1;
   unsigned char md[EVP_MAX_MD_SIZE];
   unsigned int mdlen = 0;
   int rc = -1;
   /* X509_digest hashes the DER encoding — the standard cert fingerprint. */
   if (X509_digest(cert, EVP_sha256(), md, &mdlen) == 1 && mdlen == 32)
   {
      for (unsigned int i = 0; i < mdlen; i++)
         snprintf(hex_out + i * 2, 3, "%02x", md[i]);
      hex_out[mdlen * 2] = '\0';
      rc = 0;
   }
   X509_free(cert);
   return rc;
}

int kb_pki_cert_metadata(const char *cert_pem, char *issuer_out, size_t issuer_cap,
                         char *serial_out, size_t serial_cap)
{
   if ((issuer_out && issuer_cap == 0) || (serial_out && serial_cap == 0))
      return -1;
   X509 *cert = x509_from_pem(cert_pem);
   if (!cert)
      return -1;
   int rc = -1;
   char *issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
   const ASN1_INTEGER *asn1_serial = X509_get0_serialNumber(cert);
   BIGNUM *bn = asn1_serial ? ASN1_INTEGER_to_BN(asn1_serial, NULL) : NULL;
   char *serial = bn ? BN_bn2hex(bn) : NULL;
   if (!issuer || !issuer[0] || !serial || !serial[0])
      goto done;
   if ((issuer_out && strlen(issuer) >= issuer_cap) || (serial_out && strlen(serial) >= serial_cap))
      goto done;
   if (issuer_out)
      memcpy(issuer_out, issuer, strlen(issuer) + 1);
   if (serial_out)
      memcpy(serial_out, serial, strlen(serial) + 1);
   rc = 0;
done:
   OPENSSL_free(serial);
   BN_free(bn);
   OPENSSL_free(issuer);
   X509_free(cert);
   return rc;
}

/* --- client cert issuance --- */

int kb_pki_issue_client_cert(const kb_pki_ca_t *ca, const char *subject_cn, long valid_secs,
                             char *cert_pem_out, size_t cert_cap, char *key_pem_out, size_t key_cap)
{
   if (!ca || !subject_cn || !subject_cn[0] || !cert_pem_out || !key_pem_out || valid_secs <= 0)
      return -1;

   int rc = -1;
   EVP_PKEY *ca_key = key_from_pem(ca->key_pem);
   X509 *ca_cert = x509_from_pem(ca->cert_pem);
   EVP_PKEY *client_key = EVP_RSA_gen(2048);
   X509 *cert = X509_new();
   if (!ca_key || !ca_cert || !client_key || !cert)
      goto done;

   X509_set_version(cert, 2);
   if (!set_random_serial(cert))
      goto done;
   X509_gmtime_adj(X509_getm_notBefore(cert), 0);
   X509_gmtime_adj(X509_getm_notAfter(cert), valid_secs);
   if (X509_set_pubkey(cert, client_key) != 1)
      goto done;

   X509_NAME *name = X509_get_subject_name(cert);
   if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)subject_cn, -1,
                                  -1, 0) != 1)
      goto done;
   /* Issuer is the CA's subject. */
   if (X509_set_issuer_name(cert, X509_get_subject_name(ca_cert)) != 1)
      goto done;

   if (!add_ext(ca_cert, cert, NID_basic_constraints, "critical,CA:FALSE") ||
       !add_ext(ca_cert, cert, NID_key_usage, "critical,digitalSignature") ||
       !add_ext(ca_cert, cert, NID_ext_key_usage, "clientAuth") ||
       !add_ext(ca_cert, cert, NID_subject_key_identifier, "hash") ||
       !add_ext(ca_cert, cert, NID_authority_key_identifier, "keyid:always"))
      goto done;

   /* Signed by the CA key — this is what makes the cert chain to the CA. */
   if (X509_sign(cert, ca_key, EVP_sha256()) == 0)
      goto done;

   if (pem_from_x509(cert, cert_pem_out, cert_cap) != 0 ||
       pem_from_key(client_key, key_pem_out, key_cap) != 0)
      goto done;
   rc = 0;

done:
   if (rc != 0)
   {
      if (cert_cap)
         cert_pem_out[0] = '\0';
      if (key_cap)
         key_pem_out[0] = '\0';
   }
   X509_free(cert);
   X509_free(ca_cert);
   EVP_PKEY_free(client_key);
   EVP_PKEY_free(ca_key);
   return rc;
}

/* --- server cert issuance (for the kb's mTLS listener) --- */

int kb_pki_issue_server_cert(const kb_pki_ca_t *ca, const char *host, long valid_secs,
                             char *cert_pem_out, size_t cert_cap, char *key_pem_out, size_t key_cap)
{
   if (!ca || !host || !host[0] || !cert_pem_out || !key_pem_out || valid_secs <= 0)
      return -1;

   int rc = -1;
   EVP_PKEY *ca_key = key_from_pem(ca->key_pem);
   X509 *ca_cert = x509_from_pem(ca->cert_pem);
   EVP_PKEY *srv_key = EVP_RSA_gen(2048);
   X509 *cert = X509_new();
   if (!ca_key || !ca_cert || !srv_key || !cert)
      goto done;

   X509_set_version(cert, 2);
   if (!set_random_serial(cert))
      goto done;
   X509_gmtime_adj(X509_getm_notBefore(cert), 0);
   X509_gmtime_adj(X509_getm_notAfter(cert), valid_secs);
   if (X509_set_pubkey(cert, srv_key) != 1)
      goto done;

   X509_NAME *name = X509_get_subject_name(cert);
   if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)host, -1, -1,
                                  0) != 1)
      goto done;
   if (X509_set_issuer_name(cert, X509_get_subject_name(ca_cert)) != 1)
      goto done;

   /* subjectAltName so TLS hostname verification matches: IP:<host> when it is a
    * literal address, else DNS:<host>. */
   {
      unsigned char tmp[16];
      const char *kind = (inet_pton(AF_INET, host, tmp) == 1 || inet_pton(AF_INET6, host, tmp) == 1)
                             ? "IP"
                             : "DNS";
      char san[300];
      snprintf(san, sizeof(san), "%s:%s", kind, host);
      if (!add_ext(ca_cert, cert, NID_subject_alt_name, san))
         goto done;
   }

   if (!add_ext(ca_cert, cert, NID_basic_constraints, "critical,CA:FALSE") ||
       !add_ext(ca_cert, cert, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
       !add_ext(ca_cert, cert, NID_ext_key_usage, "serverAuth") ||
       !add_ext(ca_cert, cert, NID_subject_key_identifier, "hash") ||
       !add_ext(ca_cert, cert, NID_authority_key_identifier, "keyid:always"))
      goto done;

   if (X509_sign(cert, ca_key, EVP_sha256()) == 0)
      goto done;

   if (pem_from_x509(cert, cert_pem_out, cert_cap) != 0 ||
       pem_from_key(srv_key, key_pem_out, key_cap) != 0)
      goto done;
   rc = 0;

done:
   if (rc != 0)
   {
      if (cert_cap)
         cert_pem_out[0] = '\0';
      if (key_cap)
         key_pem_out[0] = '\0';
   }
   X509_free(cert);
   X509_free(ca_cert);
   EVP_PKEY_free(srv_key);
   EVP_PKEY_free(ca_key);
   return rc;
}

/* --- CSR signing (client keeps its private key; server controls the subject) --- */

/* Parse a PEM CSR, verify its self-signature (proof of private-key possession),
 * and require an acceptable key (RSA >= 2048). Returns a newly-allocated
 * X509_REQ* (caller frees) on success, or NULL on any failure. */
static X509_REQ *csr_parse_verify(const char *csr_pem)
{
   if (!csr_pem || !csr_pem[0])
      return NULL;
   X509_REQ *req = NULL;
   BIO *bio = BIO_new_mem_buf(csr_pem, -1);
   if (bio)
   {
      req = PEM_read_bio_X509_REQ(bio, NULL, NULL, NULL);
      BIO_free(bio);
   }
   if (!req)
      return NULL;
   EVP_PKEY *pub = X509_REQ_get_pubkey(req);
   int ok = pub && X509_REQ_verify(req, pub) == 1 && EVP_PKEY_get_base_id(pub) == EVP_PKEY_RSA &&
            EVP_PKEY_get_bits(pub) >= 2048;
   EVP_PKEY_free(pub);
   if (!ok)
   {
      X509_REQ_free(req);
      return NULL;
   }
   return req;
}

int kb_pki_csr_validate(const char *csr_pem)
{
   X509_REQ *req = csr_parse_verify(csr_pem);
   if (!req)
      return -1;
   X509_REQ_free(req);
   return 0;
}

int kb_pki_sign_csr_profile(const kb_pki_ca_t *ca, const char *csr_pem, const char *subject_cn,
                            long valid_secs, kb_pki_csr_profile_t profile, char *cert_pem_out,
                            size_t cert_cap)
{
   if (!ca || !subject_cn || !subject_cn[0] || !cert_pem_out || valid_secs <= 0 ||
       (profile != KB_PKI_CSR_CLIENT_AUTH && profile != KB_PKI_CSR_SERVER_AUTH &&
        profile != KB_PKI_CSR_KB_MANAGEMENT_CLIENT))
      return -1;

   int rc = -1;
   EVP_PKEY *ca_key = key_from_pem(ca->key_pem);
   X509 *ca_cert = x509_from_pem(ca->cert_pem);
   X509 *cert = X509_new();
   /* Parse + verify the CSR (self-signature + key strength) in one place. */
   X509_REQ *req = csr_parse_verify(csr_pem);
   EVP_PKEY *req_pub = NULL;

   if (!ca_key || !ca_cert || !cert || !req)
      goto done;

   req_pub = X509_REQ_get_pubkey(req);
   if (!req_pub)
      goto done;

   X509_set_version(cert, 2);
   if (!set_random_serial(cert))
      goto done;
   X509_gmtime_adj(X509_getm_notBefore(cert), 0);
   X509_gmtime_adj(X509_getm_notAfter(cert), valid_secs);
   /* Bind the CSR's PUBLIC KEY, but a SERVER-CONTROLLED subject (the CSR's own
    * subject is ignored — verify-then-trust). */
   if (X509_set_pubkey(cert, req_pub) != 1)
      goto done;

   X509_NAME *name = X509_get_subject_name(cert);
   if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)subject_cn, -1,
                                  -1, 0) != 1)
      goto done;
   if (X509_set_issuer_name(cert, X509_get_subject_name(ca_cert)) != 1)
      goto done;

   if (profile == KB_PKI_CSR_SERVER_AUTH)
   {
      unsigned char tmp[16];
      const char *kind =
          (inet_pton(AF_INET, subject_cn, tmp) == 1 || inet_pton(AF_INET6, subject_cn, tmp) == 1)
              ? "IP"
              : "DNS";
      char san[300];
      int n = snprintf(san, sizeof(san), "%s:%s", kind, subject_cn);
      if (n < 0 || (size_t)n >= sizeof(san) || !add_ext(ca_cert, cert, NID_subject_alt_name, san))
         goto done;
   }
   else if (profile == KB_PKI_CSR_KB_MANAGEMENT_CLIENT && !add_management_profile_ext(cert))
      goto done;

   const char *key_usage = profile == KB_PKI_CSR_SERVER_AUTH
                               ? "critical,digitalSignature,keyEncipherment"
                               : "critical,digitalSignature";
   const char *eku = profile == KB_PKI_CSR_SERVER_AUTH ? "serverAuth" : "clientAuth";
   if (!add_ext(ca_cert, cert, NID_basic_constraints, "critical,CA:FALSE") ||
       !add_ext(ca_cert, cert, NID_key_usage, key_usage) ||
       !add_ext(ca_cert, cert, NID_ext_key_usage, eku) ||
       !add_ext(ca_cert, cert, NID_subject_key_identifier, "hash") ||
       !add_ext(ca_cert, cert, NID_authority_key_identifier, "keyid:always"))
      goto done;

   if (X509_sign(cert, ca_key, EVP_sha256()) == 0)
      goto done;

   if (pem_from_x509(cert, cert_pem_out, cert_cap) != 0)
      goto done;
   rc = 0;

done:
   if (rc != 0 && cert_cap)
      cert_pem_out[0] = '\0';
   EVP_PKEY_free(req_pub);
   X509_REQ_free(req);
   X509_free(cert);
   X509_free(ca_cert);
   EVP_PKEY_free(ca_key);
   return rc;
}

int kb_pki_sign_server_role_csrs(const kb_pki_ca_t *ca, const char *client_csr_pem,
                                 const char *client_subject, const char *server_csr_pem,
                                 const char *server_subject, long valid_secs, char *client_cert_out,
                                 size_t client_cert_cap, char *server_cert_out,
                                 size_t server_cert_cap)
{
   if (client_cert_out && client_cert_cap)
      client_cert_out[0] = '\0';
   if (server_cert_out && server_cert_cap)
      server_cert_out[0] = '\0';
   if (!ca || !client_csr_pem || !client_subject || !server_csr_pem || !server_subject ||
       !client_cert_out || !client_cert_cap || !server_cert_out || !server_cert_cap)
      return -1;

   X509_REQ *client_req = csr_parse_verify(client_csr_pem);
   X509_REQ *server_req = csr_parse_verify(server_csr_pem);
   EVP_PKEY *client_key = client_req ? X509_REQ_get_pubkey(client_req) : NULL;
   EVP_PKEY *server_key = server_req ? X509_REQ_get_pubkey(server_req) : NULL;
   int distinct = client_key && server_key && EVP_PKEY_eq(client_key, server_key) == 0;
   EVP_PKEY_free(client_key);
   EVP_PKEY_free(server_key);
   X509_REQ_free(client_req);
   X509_REQ_free(server_req);
   if (!distinct)
      return -1;

   if (kb_pki_sign_csr_profile(ca, client_csr_pem, client_subject, valid_secs,
                               KB_PKI_CSR_CLIENT_AUTH, client_cert_out, client_cert_cap) != 0 ||
       kb_pki_sign_csr_profile(ca, server_csr_pem, server_subject, valid_secs,
                               KB_PKI_CSR_SERVER_AUTH, server_cert_out, server_cert_cap) != 0)
   {
      OPENSSL_cleanse(client_cert_out, client_cert_cap);
      OPENSSL_cleanse(server_cert_out, server_cert_cap);
      return -1;
   }
   return 0;
}

int kb_pki_sign_csr(const kb_pki_ca_t *ca, const char *csr_pem, const char *subject_cn,
                    long valid_secs, char *cert_pem_out, size_t cert_cap)
{
   return kb_pki_sign_csr_profile(ca, csr_pem, subject_cn, valid_secs, KB_PKI_CSR_CLIENT_AUTH,
                                  cert_pem_out, cert_cap);
}

int kb_pki_sign_kb_management_csr(const kb_pki_ca_t *ca, const char *csr_pem, long valid_secs,
                                  char *cert_pem_out, size_t cert_cap)
{
   return kb_pki_sign_csr_profile(ca, csr_pem, "p5-kb-management", valid_secs,
                                  KB_PKI_CSR_KB_MANAGEMENT_CLIENT, cert_pem_out, cert_cap);
}

/* --- chain verification --- */

int kb_pki_verify_client_cert(const char *ca_cert_pem, const char *client_cert_pem)
{
   X509 *ca = x509_from_pem(ca_cert_pem);
   X509 *client = x509_from_pem(client_cert_pem);
   X509_STORE *store = NULL;
   X509_STORE_CTX *ctx = NULL;
   int trusted = 0;

   if (!ca || !client)
      goto done;
   store = X509_STORE_new();
   ctx = X509_STORE_CTX_new();
   if (!store || !ctx)
      goto done;
   if (X509_STORE_add_cert(store, ca) != 1)
      goto done;
   if (X509_STORE_CTX_init(ctx, store, client, NULL) != 1)
      goto done;
   /* Full validation: signature chain to a trusted root, validity window, and
    * CA basic constraints on the issuer. */
   trusted = (X509_verify_cert(ctx) == 1);

done:
   X509_STORE_CTX_free(ctx);
   X509_STORE_free(store);
   X509_free(client);
   X509_free(ca);
   return trusted ? 1 : 0;
}

/* --- CA persistence (proposal invariant 3) --- */

/* Build "<dir>/<name>" into out[cap]. Returns 0 on success, -1 on overflow. */
static int join_path(const char *dir, const char *name, char *out, size_t cap)
{
   int n = snprintf(out, cap, "%s/%s", dir, name);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   return 0;
}

/* Read file at `path` into buf[cap] as a NUL-terminated string. Returns the
 * number of bytes read on success (>= 1, excludes the trailing NUL), or -1 on
 * any error (missing, unreadable, empty, or does not fit including the NUL). */
static ssize_t read_text_file(const char *path, char *buf, size_t cap)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return -1;
   ssize_t total = 0;
   while ((size_t)total < cap - 1)
   {
      ssize_t n = read(fd, buf + total, cap - 1 - (size_t)total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         close(fd);
         return -1;
      }
      if (n == 0)
         break;
      total += n;
   }
   /* Probe for one more byte: a file that fills the buffer exactly-or-beyond is
    * too large, and is rejected rather than silently truncated. */
   char extra;
   ssize_t probe = read(fd, &extra, 1);
   close(fd);
   if (probe > 0)
      return -1; /* file larger than cap-1 bytes */
   if (total <= 0)
      return -1;
   buf[total] = '\0';
   return total;
}

int kb_pki_ca_save(const char *dir, const kb_pki_ca_t *ca)
{
   if (!dir || !dir[0] || !ca)
      return -1;
   if (!ca->cert_pem[0] || !ca->key_pem[0])
      return -1;

   if (mkdir(dir, 0700) != 0 && errno != EEXIST)
      return -1;

   char cert_path[1024], key_path[1024];
   if (join_path(dir, "ca.pem", cert_path, sizeof(cert_path)) != 0)
      return -1;
   if (join_path(dir, "ca-key.pem", key_path, sizeof(key_path)) != 0)
      return -1;

   int rc = -1;
   int cert_fd = -1, key_fd = -1;
   size_t cert_len = strlen(ca->cert_pem);
   size_t key_len = strlen(ca->key_pem);

   cert_fd = open(cert_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (cert_fd < 0)
      goto done;
   if (write(cert_fd, ca->cert_pem, cert_len) != (ssize_t)cert_len)
      goto done;

   key_fd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (key_fd < 0)
      goto done;
   if (write(key_fd, ca->key_pem, key_len) != (ssize_t)key_len)
      goto done;

   rc = 0;

done:
   if (cert_fd >= 0)
      close(cert_fd);
   if (key_fd >= 0)
      close(key_fd);
   return rc;
}

int kb_pki_ca_load(const char *dir, kb_pki_ca_t *out)
{
   if (!dir || !dir[0] || !out)
      return -1;

   char vault_path[1024];
   if (join_path(dir, "ca-key.vault", vault_path, sizeof(vault_path)) == 0 &&
       access(vault_path, F_OK) == 0)
      return kb_pki_ca_load_custodied(dir, out);

   char cert_path[1024], key_path[1024];
   if (join_path(dir, "ca.pem", cert_path, sizeof(cert_path)) != 0)
      return -1;
   if (join_path(dir, "ca-key.pem", key_path, sizeof(key_path)) != 0)
      return -1;

   if (read_text_file(cert_path, out->cert_pem, sizeof(out->cert_pem)) < 0)
      return -1;
   if (read_text_file(key_path, out->key_pem, sizeof(out->key_pem)) < 0)
      return -1;
   return 0;
}

int kb_pki_ca_load_or_create(const char *dir, kb_pki_ca_t *out, int *created)
{
   if (!dir || !dir[0] || !out)
      return -1;

   if (kb_pki_ca_load(dir, out) == 0)
   {
      if (created)
         *created = 0;
      return 0;
   }

   if (kb_pki_ca_generate(out) != 0)
      return -1;
   if (kb_pki_ca_save(dir, out) != 0)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   if (created)
      *created = 1;
   return 0;
}

int kb_pki_ca_save_custodied(const char *dir, const kb_pki_ca_t *ca)
{
   if (!dir || !dir[0] || !ca || !ca->cert_pem[0] || !ca->key_pem[0])
      return -1;
   if (mkdir(dir, 0700) != 0 && errno != EEXIST)
      return -1;
   char cp[1024], ep[1024];
   if (join_path(dir, "ca.pem", cp, sizeof(cp)) || join_path(dir, "ca-key.vault", ep, sizeof(ep)))
      return -1;
   uint8_t kek[VAULT_KEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
   uint8_t ct[KB_PKI_KEY_PEM_MAX];
   char nh[VAULT_GCM_NONCE_LEN * 2 + 1], th[VAULT_GCM_TAG_LEN * 2 + 1];
   char ch[KB_PKI_KEY_PEM_MAX * 2 + 1];
   int rc = -1;
   if (vault_server_kek(kek) != 0 ||
       vault_secret_encrypt(kek, (const uint8_t *)"aimee-kb-ca-key-v1", 18,
                            (const uint8_t *)ca->key_pem, strlen(ca->key_pem), nonce, ct, tag) != 0)
      goto done;
   if (hx(nonce, sizeof(nonce), nh, sizeof(nh)) || hx(tag, sizeof(tag), th, sizeof(th)) ||
       hx(ct, strlen(ca->key_pem), ch, sizeof(ch)))
      goto done;
   int fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (fd < 0)
      goto done;
   size_t cl = strlen(ca->cert_pem);
   if (write(fd, ca->cert_pem, cl) != (ssize_t)cl)
   {
      close(fd);
      goto done;
   }
   close(fd);
   fd = open(ep, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      goto done;
   dprintf(fd, "AIMEE-CA-VAULT-V1\n%s\n%s\n%s\n", nh, th, ch);
   if (fsync(fd) != 0)
   {
      close(fd);
      goto done;
   }
   close(fd);
   rc = 0;
done:
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(ct, sizeof(ct));
   return rc;
}

kb_pki_ca_load_result_t kb_pki_ca_load_custodied_ex(const char *dir, kb_pki_ca_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!dir || !dir[0] || !out)
      return KB_PKI_CA_LOAD_INVALID;
   char cp[1024], ep[1024], buf[KB_PKI_KEY_PEM_MAX * 2 + 256];
   size_t key_bytes = 0;
   if (join_path(dir, "ca.pem", cp, sizeof(cp)) || join_path(dir, "ca-key.vault", ep, sizeof(ep)))
      return KB_PKI_CA_LOAD_INVALID;
   kb_pki_ca_load_result_t result = custodied_read(cp, out->cert_pem, sizeof(out->cert_pem));
   if (result != KB_PKI_CA_LOAD_OK)
      goto failed;
   result = custodied_read(ep, buf, sizeof(buf));
   if (result != KB_PKI_CA_LOAD_OK)
   {
      if (result == KB_PKI_CA_LOAD_INTEGRITY)
         goto failed;
      struct stat encrypted;
      if (lstat(ep, &encrypted) == 0)
      {
         /* A present but unreadable record remains an availability failure;
          * do not turn EACCES or transient I/O into a corruption oracle. */
         goto failed;
      }
      if (errno != ENOENT)
      {
         result = KB_PKI_CA_LOAD_UNAVAILABLE;
         goto failed;
      }
      /* One-way migration compatibility: an existing legacy key is accepted
       * only when no encrypted artifact exists; new writes never create it. */
      char legacy[1024];
      if (join_path(dir, "ca-key.pem", legacy, sizeof(legacy)) != 0 ||
          (result = custodied_read(legacy, out->key_pem, sizeof(out->key_pem))) !=
              KB_PKI_CA_LOAD_OK)
         goto failed;
      key_bytes = strlen(out->key_pem);
      goto validate;
   }
   size_t envelope_len = strlen(buf), newline_count = 0;
   for (size_t i = 0; i < envelope_len; ++i)
      newline_count += buf[i] == '\n';
   if (!envelope_len || buf[envelope_len - 1] != '\n' || newline_count != 4 ||
       strstr(buf, "\n\n") || strchr(buf, '\r'))
   {
      result = KB_PKI_CA_LOAD_INTEGRITY;
      goto failed;
   }
   char *save = NULL;
   char *a = strtok_r(buf, "\n", &save), *n = strtok_r(NULL, "\n", &save),
        *t = strtok_r(NULL, "\n", &save), *c = strtok_r(NULL, "\n", &save);
   char *trailing = strtok_r(NULL, "\n", &save);
   if (!a || !n || !t || !c || trailing || strcmp(a, "AIMEE-CA-VAULT-V1"))
   {
      result = KB_PKI_CA_LOAD_INTEGRITY;
      goto failed;
   }
   uint8_t kek[VAULT_KEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN],
       ct[KB_PKI_KEY_PEM_MAX];
   memset(kek, 0, sizeof(kek));
   memset(ct, 0, sizeof(ct));
   size_t cn = strlen(c);
   if (strlen(n) != sizeof(nonce) * 2 || strlen(t) != sizeof(tag) * 2 || !cn || (cn & 1) ||
       cn / 2 >= sizeof(out->key_pem) || unhx(n, strlen(n), nonce, sizeof(nonce)) ||
       unhx(t, strlen(t), tag, sizeof(tag)) || unhx(c, cn, ct, sizeof(ct)))
   {
      result = KB_PKI_CA_LOAD_INTEGRITY;
      goto decrypt_done;
   }
   if (vault_server_kek(kek) != 0)
   {
      result = KB_PKI_CA_LOAD_UNAVAILABLE;
      goto decrypt_done;
   }
   if (vault_secret_decrypt(kek, (const uint8_t *)"aimee-kb-ca-key-v1", 18, nonce, ct, cn / 2, tag,
                            (uint8_t *)out->key_pem) != 0)
   {
      result = KB_PKI_CA_LOAD_INTEGRITY;
      goto decrypt_done;
   }
   key_bytes = cn / 2;
   if (memchr(out->key_pem, 0, key_bytes))
   {
      result = KB_PKI_CA_LOAD_INTEGRITY;
      goto decrypt_done;
   }
   out->key_pem[key_bytes] = 0;
   result = KB_PKI_CA_LOAD_OK;
decrypt_done:
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(ct, sizeof(ct));
   if (result != KB_PKI_CA_LOAD_OK)
      goto failed;

validate:
   {
      X509 *cert = x509_from_pem(out->cert_pem);
      EVP_PKEY *key = key_from_pem(out->key_pem);
      char canonical_cert[KB_PKI_CERT_PEM_MAX] = {0};
      char canonical_key[KB_PKI_KEY_PEM_MAX] = {0};
      int valid = cert && key && X509_check_private_key(cert, key) == 1 &&
                  pem_from_x509(cert, canonical_cert, sizeof(canonical_cert)) == 0 &&
                  pem_from_key(key, canonical_key, sizeof(canonical_key)) == 0 &&
                  strlen(canonical_cert) == strlen(out->cert_pem) &&
                  !memcmp(canonical_cert, out->cert_pem, strlen(canonical_cert)) &&
                  strlen(canonical_key) == key_bytes &&
                  !memcmp(canonical_key, out->key_pem, key_bytes);
      OPENSSL_cleanse(canonical_key, sizeof(canonical_key));
      OPENSSL_cleanse(canonical_cert, sizeof(canonical_cert));
      EVP_PKEY_free(key);
      X509_free(cert);
      if (!valid)
      {
         result = KB_PKI_CA_LOAD_INTEGRITY;
         goto failed;
      }
   }
   OPENSSL_cleanse(buf, sizeof(buf));
   return KB_PKI_CA_LOAD_OK;

failed:
   OPENSSL_cleanse(buf, sizeof(buf));
   OPENSSL_cleanse(out, sizeof(*out));
   return result;
}

int kb_pki_ca_load_custodied(const char *dir, kb_pki_ca_t *out)
{
   return kb_pki_ca_load_custodied_ex(dir, out) == KB_PKI_CA_LOAD_OK ? 0 : -1;
}

int kb_pki_ca_load_or_create_custodied(const char *dir, kb_pki_ca_t *out, int *created)
{
   int r = kb_pki_ca_load_custodied(dir, out);
   if (r == 0)
   {
      if (created)
         *created = 0;
      return 0;
   }
   if (kb_pki_ca_generate(out) != 0 || kb_pki_ca_save_custodied(dir, out) != 0)
   {
      OPENSSL_cleanse(out, sizeof(*out));
      return -1;
   }
   if (created)
      *created = 1;
   return 0;
}
