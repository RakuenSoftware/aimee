/* kb_mgmt_status_peer.c: fail-closed P5 management-client peer profile. */
#include "kb_mgmt_status_peer.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/x509v3.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MANAGEMENT_PROFILE_OID "1.3.6.1.4.1.55555.5.1"

static int exact_common_name(X509 *cert)
{
   static const unsigned char expected[] = "p5-kb-management";
   X509_NAME *subject = X509_get_subject_name(cert);
   if (!subject)
      return 0;

   int pos = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
   if (pos < 0 || X509_NAME_get_index_by_NID(subject, NID_commonName, pos) >= 0)
      return 0;
   X509_NAME_ENTRY *entry = X509_NAME_get_entry(subject, pos);
   ASN1_STRING *value = entry ? X509_NAME_ENTRY_get_data(entry) : NULL;
   return value && ASN1_STRING_length(value) == (int)sizeof(expected) - 1 &&
          CRYPTO_memcmp(ASN1_STRING_get0_data(value), expected, sizeof(expected) - 1) == 0;
}

static int client_auth_only(X509 *cert)
{
   int pos = X509_get_ext_by_NID(cert, NID_ext_key_usage, -1);
   if (pos < 0 || X509_get_ext_by_NID(cert, NID_ext_key_usage, pos) >= 0)
      return 0;

   int critical = -1;
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, &critical, NULL);
   int ok = eku && sk_ASN1_OBJECT_num(eku) == 1 &&
            OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == NID_client_auth &&
            X509_check_purpose(cert, X509_PURPOSE_SSL_CLIENT, 0) == 1;
   EXTENDED_KEY_USAGE_free(eku);
   return ok;
}

static int exact_management_marker(X509 *cert)
{
   static const unsigned char marker[] = "aimee-p5-kb-management-v1";
   ASN1_OBJECT *oid = OBJ_txt2obj(MANAGEMENT_PROFILE_OID, 1);
   if (!oid)
      return 0;

   int pos = X509_get_ext_by_OBJ(cert, oid, -1);
   X509_EXTENSION *ext = pos >= 0 ? X509_get_ext(cert, pos) : NULL;
   ASN1_OCTET_STRING *value = ext ? X509_EXTENSION_get_data(ext) : NULL;
   int ok = ext && !X509_EXTENSION_get_critical(ext) &&
            X509_get_ext_by_OBJ(cert, oid, pos) < 0 && value &&
            ASN1_STRING_length(value) == (int)sizeof(marker) - 1 &&
            CRYPTO_memcmp(ASN1_STRING_get0_data(value), marker, sizeof(marker) - 1) == 0;
   ASN1_OBJECT_free(oid);
   return ok;
}

int kb_mgmt_status_peer_verify(SSL *ssl, kb_mgmt_status_peer_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!ssl || !SSL_is_init_finished(ssl) || !(SSL_get_verify_mode(ssl) & SSL_VERIFY_PEER) ||
       SSL_get_verify_result(ssl) != X509_V_OK)
      return 0;

   X509 *cert = SSL_get1_peer_certificate(ssl);
   if (!cert)
      return 0;

   int ok = 0;
   char *issuer = NULL;
   char *serial = NULL;
   BIGNUM *serial_bn = NULL;
   unsigned char digest[32];
   unsigned int digest_len = 0;

   ASN1_INTEGER *asn_serial = X509_get_serialNumber(cert);
   issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
   serial_bn = asn_serial ? ASN1_INTEGER_to_BN(asn_serial, NULL) : NULL;
   serial = serial_bn && !BN_is_negative(serial_bn) ? BN_bn2hex(serial_bn) : NULL;
   if (!issuer || !issuer[0] || strlen(issuer) >= sizeof(out->issuer) || !serial || !serial[0] ||
       strlen(serial) >= sizeof(out->serial_norm) || !exact_common_name(cert) ||
       X509_check_ca(cert) != 0 || !client_auth_only(cert) || !exact_management_marker(cert) ||
       X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1 || digest_len != sizeof(digest))
      goto done;

   memcpy(out->issuer, issuer, strlen(issuer) + 1);
   for (size_t i = 0; serial[i]; i++)
      out->serial_norm[i] = (char)tolower((unsigned char)serial[i]);
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(out->fingerprint + i * 2, 3, "%02x", digest[i]);
   out->fingerprint[64] = '\0';
   ok = 1;

done:
   OPENSSL_free(serial);
   OPENSSL_free(issuer);
   BN_free(serial_bn);
   X509_free(cert);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok;
}
