#define _GNU_SOURCE
#include "kb_management_cert_crypto.h"

#include <openssl/asn1.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string.h>
#include <time.h>

static const char management_cn[] = "p5-kb-management";
static const char management_oid[] = "1.3.6.1.4.1.55555.5.1";
static const unsigned char management_marker[] = "aimee-p5-kb-management-v1";

static int exact_csr_cn(X509_REQ *);

int kb_management_cert_sha256(const void *p, size_t n, uint8_t out[32])
{
   if (!out)
      return -1;
   memset(out, 0, 32);
   unsigned int len = 0;
   return p && n && EVP_Digest(p, n, out, &len, EVP_sha256(), NULL) == 1 && len == 32 ? 0 : -1;
}

static int der_digest_pubkey(EVP_PKEY *key, uint8_t out[32])
{
   int n = i2d_PUBKEY(key, NULL);
   if (n <= 0)
      return -1;
   unsigned char *der = OPENSSL_malloc((size_t)n), *p = der;
   int rc = der && i2d_PUBKEY(key, &p) == n ? kb_management_cert_sha256(der, (size_t)n, out) : -1;
   if (der)
      OPENSSL_clear_free(der, (size_t)n);
   return rc;
}

static int bio_copy(BIO *bio, char *out, size_t cap, size_t *len)
{
   BUF_MEM *mem = NULL;
   BIO_get_mem_ptr(bio, &mem);
   if (!mem || !mem->length || mem->length >= cap)
      return -1;
   memcpy(out, mem->data, mem->length);
   out[mem->length] = 0;
   *len = mem->length;
   return 0;
}

static int key_material(EVP_PKEY *key, X509_REQ *csr, kb_management_cert_key_material_t *out)
{
   unsigned char *p;
   PKCS8_PRIV_KEY_INFO *p8 = EVP_PKEY2PKCS8(key);
   int n = p8 ? i2d_PKCS8_PRIV_KEY_INFO(p8, NULL) : -1;
   if (n <= 0 || (size_t)n > sizeof(out->key_der))
   {
      PKCS8_PRIV_KEY_INFO_free(p8);
      return -1;
   }
   p = out->key_der;
   if (i2d_PKCS8_PRIV_KEY_INFO(p8, &p) != n)
   {
      PKCS8_PRIV_KEY_INFO_free(p8);
      return -1;
   }
   PKCS8_PRIV_KEY_INFO_free(p8);
   out->key_der_len = (size_t)n;
   n = i2d_X509_REQ(csr, NULL);
   if (n <= 0 || (size_t)n > sizeof(out->csr_der))
      return -1;
   p = out->csr_der;
   if (i2d_X509_REQ(csr, &p) != n)
      return -1;
   out->csr_der_len = (size_t)n;
   BIO *bio = BIO_new(BIO_s_mem());
   int ok = bio && PEM_write_bio_X509_REQ(bio, csr) == 1 &&
            bio_copy(bio, out->csr_pem, sizeof(out->csr_pem), &out->csr_pem_len) == 0 &&
            kb_management_cert_sha256(out->csr_der, out->csr_der_len, out->csr_digest) == 0 &&
            der_digest_pubkey(key, out->csr_spki_digest) == 0;
   BIO_free(bio);
   return ok ? 0 : -1;
}

int kb_management_cert_key_generate(kb_management_cert_key_material_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   EVP_PKEY *key = EVP_RSA_gen(2048);
   X509_REQ *csr = X509_REQ_new();
   X509_NAME *name = csr ? X509_NAME_new() : NULL;
   int ok = key && csr && name && X509_REQ_set_version(csr, 0L) == 1 &&
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       (const unsigned char *)management_cn, -1, -1, 0) == 1 &&
            X509_REQ_set_subject_name(csr, name) == 1 && X509_REQ_set_pubkey(csr, key) == 1 &&
            X509_REQ_sign(csr, key, EVP_sha256()) > 0 && X509_REQ_verify(csr, key) == 1 &&
            key_material(key, csr, out) == 0;
   X509_NAME_free(name);
   X509_REQ_free(csr);
   EVP_PKEY_free(key);
   if (!ok)
      kb_management_cert_key_material_clear(out);
   return ok ? 0 : -1;
}

int kb_management_cert_key_intent_verify(const uint8_t *key_der, size_t key_len,
                                         const uint8_t *csr_der, size_t csr_len,
                                         kb_management_cert_key_material_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!key_der || !key_len || key_len > sizeof(out->key_der) || !csr_der || !csr_len ||
       csr_len > sizeof(out->csr_der))
      return -1;
   const unsigned char *kp = key_der, *cp = csr_der;
   PKCS8_PRIV_KEY_INFO *p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &kp, (long)key_len);
   EVP_PKEY *key = p8 ? EVP_PKCS82PKEY(p8) : NULL;
   X509_REQ *csr = d2i_X509_REQ(NULL, &cp, (long)csr_len);
   EVP_PKEY *pub = csr ? X509_REQ_get_pubkey(csr) : NULL;
   int bits = key ? EVP_PKEY_get_bits(key) : 0;
   int ok = key && csr && pub && kp == key_der + key_len && cp == csr_der + csr_len &&
            EVP_PKEY_is_a(key, "RSA") == 1 && bits == 2048 && X509_REQ_verify(csr, pub) == 1 &&
            EVP_PKEY_eq(key, pub) == 1 && exact_csr_cn(csr) && key_material(key, csr, out) == 0 &&
            out->key_der_len == key_len && out->csr_der_len == csr_len &&
            CRYPTO_memcmp(out->key_der, key_der, key_len) == 0 &&
            CRYPTO_memcmp(out->csr_der, csr_der, csr_len) == 0;
   EVP_PKEY_free(pub);
   X509_REQ_free(csr);
   EVP_PKEY_free(key);
   PKCS8_PRIV_KEY_INFO_free(p8);
   if (!ok)
      kb_management_cert_key_material_clear(out);
   return ok ? 0 : -1;
}

static int exact_cn(X509 *cert)
{
   X509_NAME *name = X509_get_subject_name(cert);
   int count = X509_NAME_entry_count(name), cn_count = 0;
   for (int i = 0; i < count; ++i)
      if (OBJ_obj2nid(X509_NAME_ENTRY_get_object(X509_NAME_get_entry(name, i))) ==
          NID_commonName)
         cn_count++;
   char value[64];
   int n = X509_NAME_get_text_by_NID(name, NID_commonName, value, sizeof(value));
   return cn_count == 1 && n == (int)strlen(management_cn) && !memcmp(value, management_cn, n) &&
          count == 1;
}

static int exact_csr_cn(X509_REQ *csr)
{
   X509_NAME *name = X509_REQ_get_subject_name(csr);
   int count = X509_NAME_entry_count(name), cn_count = 0;
   for (int i = 0; i < count; ++i)
      if (OBJ_obj2nid(X509_NAME_ENTRY_get_object(X509_NAME_get_entry(name, i))) ==
          NID_commonName)
         cn_count++;
   char value[64];
   int n = X509_NAME_get_text_by_NID(name, NID_commonName, value, sizeof(value));
   return count == 1 && cn_count == 1 && n == (int)strlen(management_cn) &&
          !memcmp(value, management_cn, (size_t)n);
}

static int exact_extensions(X509 *cert)
{
   BASIC_CONSTRAINTS *bc = X509_get_ext_d2i(cert, NID_basic_constraints, NULL, NULL);
   ASN1_BIT_STRING *ku = X509_get_ext_d2i(cert, NID_key_usage, NULL, NULL);
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
   int bc_index = X509_get_ext_by_NID(cert, NID_basic_constraints, -1);
   int ku_index = X509_get_ext_by_NID(cert, NID_key_usage, -1);
   int eku_index = X509_get_ext_by_NID(cert, NID_ext_key_usage, -1);
   int ski_index = X509_get_ext_by_NID(cert, NID_subject_key_identifier, -1);
   int aki_index = X509_get_ext_by_NID(cert, NID_authority_key_identifier, -1);
   X509_EXTENSION *bc_ext = bc_index >= 0 ? X509_get_ext(cert, bc_index) : NULL;
   X509_EXTENSION *ku_ext = ku_index >= 0 ? X509_get_ext(cert, ku_index) : NULL;
   int ok = X509_get_ext_count(cert) == 6 && bc && !bc->ca && !bc->pathlen && bc_ext &&
            X509_EXTENSION_get_critical(bc_ext) == 1 && ku && ku_ext &&
            X509_EXTENSION_get_critical(ku_ext) == 1 && ASN1_BIT_STRING_get_bit(ku, 0) == 1;
   int ku_bits = ku ? ASN1_STRING_length(ku) * 8 : 0;
   for (int i = 1; ok && i < ku_bits; ++i)
      if (ASN1_BIT_STRING_get_bit(ku, i))
         ok = 0;
   ok = ok && eku && sk_ASN1_OBJECT_num(eku) == 1 &&
        OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == NID_client_auth && bc_index >= 0 &&
        X509_get_ext_by_NID(cert, NID_basic_constraints, bc_index) < 0 && ku_index >= 0 &&
        X509_get_ext_by_NID(cert, NID_key_usage, ku_index) < 0 && eku_index >= 0 &&
        X509_get_ext_by_NID(cert, NID_ext_key_usage, eku_index) < 0 && ski_index >= 0 &&
        X509_get_ext_by_NID(cert, NID_subject_key_identifier, ski_index) < 0 && aki_index >= 0 &&
        X509_get_ext_by_NID(cert, NID_authority_key_identifier, aki_index) < 0;

   ASN1_OBJECT *marker_oid = OBJ_txt2obj(management_oid, 1);
   int marker_index = marker_oid ? X509_get_ext_by_OBJ(cert, marker_oid, -1) : -1;
   X509_EXTENSION *marker = marker_index >= 0 ? X509_get_ext(cert, marker_index) : NULL;
   ASN1_OCTET_STRING *data = marker ? X509_EXTENSION_get_data(marker) : NULL;
   ok = ok && marker && X509_EXTENSION_get_critical(marker) == 0 && data &&
        (size_t)ASN1_STRING_length(data) == sizeof(management_marker) - 1 &&
        !memcmp(ASN1_STRING_get0_data(data), management_marker, sizeof(management_marker) - 1) &&
        X509_get_ext_by_OBJ(cert, marker_oid, marker_index) < 0;
   ASN1_OBJECT_free(marker_oid);
   BASIC_CONSTRAINTS_free(bc);
   ASN1_BIT_STRING_free(ku);
   EXTENDED_KEY_USAGE_free(eku);
   return ok;
}

static int name_text(X509_NAME *name, char *out, size_t cap)
{
   if (!name || !out || cap > INT_MAX)
      return -1;
   char *allocated = X509_NAME_oneline(name, NULL, 0);
   size_t n = allocated ? strlen(allocated) : 0;
   if (!n || n >= cap)
   {
      OPENSSL_free(allocated);
      return -1;
   }
   memcpy(out, allocated, n + 1);
   OPENSSL_clear_free(allocated, n + 1);
   return 0;
}

static int serial_text(X509 *cert, char *out, size_t cap)
{
   const ASN1_INTEGER *serial = X509_get0_serialNumber(cert);
   const unsigned char *raw = serial ? ASN1_STRING_get0_data(serial) : NULL;
   int raw_len = serial ? ASN1_STRING_length(serial) : 0;
   unsigned char any = 0;
   for (int i = 0; i < raw_len; ++i)
      any |= raw[i];
   if (!serial || ASN1_STRING_type(serial) == V_ASN1_NEG_INTEGER || raw_len <= 0 || !any)
      return -1;
   BIGNUM *bn = ASN1_INTEGER_to_BN(serial, NULL);
   char *hex = bn ? BN_bn2hex(bn) : NULL;
   size_t n = hex ? strlen(hex) : 0;
   if (!n || n >= cap)
   {
      OPENSSL_free(hex);
      BN_free(bn);
      return -1;
   }
   for (size_t i = 0; i < n; ++i)
      out[i] = (char)(hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] + ('a' - 'A') : hex[i]);
   out[n] = 0;
   OPENSSL_free(hex);
   BN_free(bn);
   return 0;
}

static int x509_digest(X509 *cert, uint8_t out[32])
{
   unsigned int n = 0;
   return X509_digest(cert, EVP_sha256(), out, &n) == 1 && n == 32 ? 0 : -1;
}

static X509 *strict_x509_from_pem(const char *pem)
{
   size_t input_len = pem ? strnlen(pem, KB_PKI_CERT_PEM_MAX) : 0;
   if (!input_len || input_len == KB_PKI_CERT_PEM_MAX || input_len > INT_MAX)
      return NULL;
   BIO *input = BIO_new_mem_buf(pem, (int)input_len);
   X509 *cert = input ? PEM_read_bio_X509(input, NULL, NULL, NULL) : NULL;
   int consumed = cert && BIO_ctrl_pending(input) == 0;
   BIO *canonical = consumed ? BIO_new(BIO_s_mem()) : NULL;
   BUF_MEM *memory = NULL;
   int exact = canonical && PEM_write_bio_X509(canonical, cert) == 1;
   if (exact)
   {
      BIO_get_mem_ptr(canonical, &memory);
      exact = memory && memory->length == input_len &&
              CRYPTO_memcmp(memory->data, pem, input_len) == 0;
   }
   BIO_free(canonical);
   BIO_free(input);
   if (!exact)
   {
      X509_free(cert);
      cert = NULL;
   }
   return cert;
}

int kb_management_cert_leaf_verify(const kb_management_cert_key_material_t *material,
                                   const char *leaf_pem, const char *ca_pem,
                                   kb_management_cert_verified_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!material || !leaf_pem || !ca_pem)
      return -1;
   X509 *leaf = strict_x509_from_pem(leaf_pem);
   X509 *ca = strict_x509_from_pem(ca_pem);
   const unsigned char *kp = material->key_der;
   PKCS8_PRIV_KEY_INFO *p8 =
       d2i_PKCS8_PRIV_KEY_INFO(NULL, &kp, (long)material->key_der_len);
   EVP_PKEY *key = p8 ? EVP_PKCS82PKEY(p8) : NULL;
   EVP_PKEY *leaf_key = leaf ? X509_get_pubkey(leaf) : NULL;
   EVP_PKEY *ca_key = ca ? X509_get_pubkey(ca) : NULL;
   X509_STORE *store = X509_STORE_new();
   X509_STORE_CTX *ctx = X509_STORE_CTX_new();
   int verified = store && ctx && ca && leaf && X509_STORE_add_cert(store, ca) == 1 &&
                  X509_STORE_CTX_init(ctx, store, leaf, NULL) == 1 &&
                  X509_verify_cert(ctx) == 1;
   kb_management_cert_verified_t v = {0};
   int leaf_n = leaf ? i2d_X509(leaf, NULL) : -1, ca_n = ca ? i2d_X509(ca, NULL) : -1;
   int64_t not_before = 0, not_after = 0;
   unsigned char *p;
   int ok = verified && key && kp == material->key_der + material->key_der_len && leaf_key &&
            ca_key && EVP_PKEY_eq(key, leaf_key) == 1 &&
            X509_verify(leaf, ca_key) == 1 && exact_cn(leaf) && exact_extensions(leaf) &&
            leaf_n > 0 && (size_t)leaf_n <= sizeof(v.leaf_der) && ca_n > 0 &&
            (size_t)ca_n <= sizeof(v.ca_der);
   if (verified)
   {
      struct tm before_tm = {0}, after_tm = {0};
      if (ASN1_TIME_to_tm(X509_get0_notBefore(leaf), &before_tm) != 1 ||
          ASN1_TIME_to_tm(X509_get0_notAfter(leaf), &after_tm) != 1)
         ok = 0;
#ifdef _GNU_SOURCE
      not_before = (int64_t)timegm(&before_tm);
      not_after = (int64_t)timegm(&after_tm);
#else
      not_before = (int64_t)mktime(&before_tm);
      not_after = (int64_t)mktime(&after_tm);
#endif
   }
   ok = ok && not_before > 0 && not_after > not_before && not_after - not_before >= 3540 &&
        not_after - not_before <= 3660;
   if (ok)
   {
      p = v.leaf_der;
      ok = i2d_X509(leaf, &p) == leaf_n;
      v.leaf_der_len = (size_t)leaf_n;
      p = v.ca_der;
      ok = ok && i2d_X509(ca, &p) == ca_n;
      v.ca_der_len = (size_t)ca_n;
      ok = ok && name_text(X509_get_subject_name(ca), v.ca_issuer, sizeof(v.ca_issuer)) == 0 &&
           name_text(X509_get_issuer_name(leaf), v.leaf_issuer, sizeof(v.leaf_issuer)) == 0 &&
           !strcmp(v.ca_issuer, v.leaf_issuer) &&
           serial_text(leaf, v.leaf_serial_norm, sizeof(v.leaf_serial_norm)) == 0 &&
           x509_digest(ca, v.ca_fingerprint) == 0 &&
           x509_digest(leaf, v.leaf_fingerprint) == 0 &&
           der_digest_pubkey(leaf_key, v.leaf_spki_digest) == 0 &&
           CRYPTO_memcmp(v.leaf_spki_digest, material->csr_spki_digest, 32) == 0;
      v.not_before_epoch = not_before;
      v.not_after_epoch = not_after;
   }
   X509_STORE_CTX_free(ctx);
   X509_STORE_free(store);
   EVP_PKEY_free(ca_key);
   EVP_PKEY_free(leaf_key);
   EVP_PKEY_free(key);
   PKCS8_PRIV_KEY_INFO_free(p8);
   X509_free(ca);
   X509_free(leaf);
   if (!ok)
      memset(&v, 0, sizeof(v));
   else
      *out = v;
   return ok ? 0 : -1;
}

static int der_to_pem(const uint8_t *der, size_t len, int key, char *out, size_t cap, size_t *used)
{
   const unsigned char *p = der;
   PKCS8_PRIV_KEY_INFO *p8 = key ? d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, (long)len) : NULL;
   EVP_PKEY *pkey = p8 ? EVP_PKCS82PKEY(p8) : NULL;
   X509 *cert = key ? NULL : d2i_X509(NULL, &p, (long)len);
   BIO *bio = BIO_new(BIO_s_mem());
   int ok = p == der + len && bio &&
            (key ? PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL)
                 : PEM_write_bio_X509(bio, cert)) == 1 &&
            bio_copy(bio, out, cap, used) == 0;
   BIO_free(bio);
   X509_free(cert);
   EVP_PKEY_free(pkey);
   PKCS8_PRIV_KEY_INFO_free(p8);
   return ok ? 0 : -1;
}

int kb_management_cert_bundle_to_pem(const uint8_t *key, size_t key_len, const uint8_t *leaf,
                                     size_t leaf_len, const uint8_t *ca, size_t ca_len,
                                     kb_management_cert_bundle_t *out)
{
   if (!out)
      return -1;
   kb_management_cert_bundle_clear(out);
   kb_management_cert_bundle_t v = {0};
   if (der_to_pem(key, key_len, 1, v.key_pem, sizeof(v.key_pem), &v.key_pem_len) ||
       der_to_pem(leaf, leaf_len, 0, v.leaf_pem, sizeof(v.leaf_pem), &v.leaf_pem_len) ||
       der_to_pem(ca, ca_len, 0, v.ca_pem, sizeof(v.ca_pem), &v.ca_pem_len))
   {
      kb_management_cert_bundle_clear(&v);
      return -1;
   }
   *out = v;
   OPENSSL_cleanse(&v, sizeof(v));
   return 0;
}

void kb_management_cert_key_material_clear(kb_management_cert_key_material_t *value)
{
   if (value)
      OPENSSL_cleanse(value, sizeof(*value));
}
