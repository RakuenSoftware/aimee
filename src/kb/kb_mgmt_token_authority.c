#include "kb_mgmt_token_authority.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
   EVP_PKEY *key;
} authority_signer_t;

static int fixed_text(const char *s, size_t cap, size_t min, size_t max, int token)
{
   if (!s || !cap || max >= cap)
      return 0;
   size_t n = strnlen(s, cap);
   if (n == cap || n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f ||
          (token && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')))
         return 0;
   }
   for (size_t i = n + 1; i < cap; ++i)
      if (s[i] != 0)
         return 0;
   return 1;
}

static int exact_hex(const char *s, size_t cap, size_t n)
{
   if (!fixed_text(s, cap, n, n, 0))
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int digest(const void *p, size_t n, uint8_t out[32])
{
   unsigned int written = 0;
   return p && n && EVP_Digest(p, n, out, &written, EVP_sha256(), NULL) == 1 && written == 32 ? 0
                                                                                              : -1;
}

static int canonical_actor(const char *s)
{
   if (!fixed_text(s, 577, 1, 576, 0))
      return 0;
   if (!strcmp(s, "owner"))
      return 1;
   int is_cert = !strncmp(s, "cert:", 5);
   if (!is_cert && strncmp(s, "oidc:", 5))
      return 0;
   const char *middle = strchr(s + 5, ':');
   if (!middle || middle == s + 5 || !middle[1] || strchr(middle + 1, ':'))
      return 0;
   const char *parts[2] = {s + 5, middle + 1};
   size_t lengths[2] = {(size_t)(middle - (s + 5)), strlen(middle + 1)};
   for (size_t part = 0; part < 2; ++part)
      for (size_t i = 0; i < lengths[part]; ++i)
      {
         unsigned char c = (unsigned char)parts[part][i];
         if (c < 0x20 || c == 0x7f || c == ':')
            return 0;
         if (c == '%')
         {
            if (i + 2 >= lengths[part] ||
                !((parts[part][i + 1] == '2' && parts[part][i + 2] == '5') ||
                  (parts[part][i + 1] == '3' && parts[part][i + 2] == 'A')))
               return 0;
            i += 2;
         }
      }
   if (is_cert)
      for (const char *p = middle + 1; *p; ++p)
         if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return 0;
   return 1;
}

/* Recompute every published binding from the record's own bytes: the modulus
 * digest, the JWK digest, the kid derived from the modulus, and the HWM
 * attestation digest. A record that disagrees with itself never reaches a key. */
static int key_bindings_valid(const uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN],
                              const uint8_t expected_public_digest[32],
                              const uint8_t expected_jwk_digest[32], const char *expected_kid,
                              const uint8_t *attestation, size_t attestation_len,
                              const uint8_t expected_attestation_digest[32])
{
   uint8_t public_digest[32] = {0}, jwk_digest[32] = {0}, attestation_digest[32] = {0};
   char kid[KB_MGMT_TOKEN_KID_MAX + 1] = {0};
   char jwk[KB_MGMT_TOKEN_JWK_MAX] = {0};
   size_t jwk_len = 0;
   int ok = !digest(modulus, KB_MGMT_TOKEN_MODULUS_LEN, public_digest) &&
            !kb_mgmt_token_kid(modulus, KB_MGMT_TOKEN_MODULUS_LEN, kid, sizeof(kid)) &&
            !kb_mgmt_token_jwk(modulus, KB_MGMT_TOKEN_MODULUS_LEN, jwk, sizeof(jwk), &jwk_len) &&
            !digest(jwk, jwk_len, jwk_digest) &&
            !digest(attestation, attestation_len, attestation_digest) &&
            !CRYPTO_memcmp(public_digest, expected_public_digest, 32) &&
            !CRYPTO_memcmp(jwk_digest, expected_jwk_digest, 32) &&
            !CRYPTO_memcmp(attestation_digest, expected_attestation_digest, 32) &&
            strlen(kid) == strlen(expected_kid) && !CRYPTO_memcmp(kid, expected_kid, strlen(kid));
   OPENSSL_cleanse(public_digest, sizeof(public_digest));
   OPENSSL_cleanse(jwk_digest, sizeof(jwk_digest));
   OPENSSL_cleanse(attestation_digest, sizeof(attestation_digest));
   OPENSSL_cleanse(kid, sizeof(kid));
   OPENSSL_cleanse(jwk, sizeof(jwk));
   return ok;
}

int kb_mgmt_token_authority_record_valid(const kb_mgmt_token_authority_record_t *r)
{
   if (!r || (r->newly_admitted != 0 && r->newly_admitted != 1) ||
       !exact_hex(r->correlation_id, sizeof(r->correlation_id), 64) ||
       !exact_hex(r->jti, sizeof(r->jti), 64) || r->team_id < 1 ||
       !canonical_actor(r->actor_identity) ||
       (r->capability != KB_MGMT_TOKEN_CAP_REMOTE_WRITES &&
        r->capability != KB_MGMT_TOKEN_CAP_REMOTE_READS) ||
       !fixed_text(r->target_server_id, sizeof(r->target_server_id), 1, 127, 1) ||
       !exact_hex(r->request_sha256, sizeof(r->request_sha256), 64) ||
       !fixed_text(r->token_issuer, sizeof(r->token_issuer), 1, 255, 0) ||
       !fixed_text(r->audience, sizeof(r->audience), 1, 127, 1) ||
       !fixed_text(r->kid, sizeof(r->kid), 1, 64, 1) || r->issued_at < 0 ||
       r->expires_at <= r->issued_at || r->expires_at - r->issued_at > 90 ||
       !exact_hex(r->installation_id, sizeof(r->installation_id), 32) ||
       r->installation_generation < 1 || r->installation_enrollment_id < 1 ||
       !fixed_text(r->local_cert_issuer, sizeof(r->local_cert_issuer), 1, 511, 0) ||
       !exact_hex(r->local_cert_serial_norm, sizeof(r->local_cert_serial_norm),
                  strnlen(r->local_cert_serial_norm, sizeof(r->local_cert_serial_norm))) ||
       !exact_hex(r->local_cert_fingerprint, sizeof(r->local_cert_fingerprint), 64) ||
       r->target_enrollment_id < 1 ||
       !fixed_text(r->target_mgmt_issuer, sizeof(r->target_mgmt_issuer), 1, 511, 0) ||
       !exact_hex(r->target_mgmt_serial_norm, sizeof(r->target_mgmt_serial_norm),
                  strnlen(r->target_mgmt_serial_norm, sizeof(r->target_mgmt_serial_norm))) ||
       !exact_hex(r->target_mgmt_fingerprint, sizeof(r->target_mgmt_fingerprint), 64) ||
       r->revocation_generation < 1 || r->publication_generation != 1 ||
       !fixed_text(r->publication_candidate_id, sizeof(r->publication_candidate_id), 1,
                   KB_MGMT_TOKEN_AUTHORITY_CANDIDATE_MAX, 1) ||
       !fixed_text(r->token_custody_key_id, sizeof(r->token_custody_key_id), 1,
                   KB_MGMT_ROOT_CUSTODY_ID_MAX, 0) ||
       r->token_version != 2 || r->token_public_exponent[0] != 1 ||
       r->token_public_exponent[1] != 0 || r->token_public_exponent[2] != 1 ||
       r->vault_seal_epoch < 1 || !r->hwm_attestation_len ||
       r->hwm_attestation_len > sizeof(r->hwm_attestation) ||
       r->envelope.seal_epoch != r->vault_seal_epoch || r->envelope.version != 2 ||
       !r->envelope.ciphertext_len || r->envelope.ciphertext_len > sizeof(r->envelope.ciphertext) ||
       r->key_use_created_at_epoch < 0)
      return 0;

   size_t local_serial = strnlen(r->local_cert_serial_norm, sizeof(r->local_cert_serial_norm));
   size_t target_serial = strnlen(r->target_mgmt_serial_norm, sizeof(r->target_mgmt_serial_norm));
   if (!local_serial || local_serial > 79 || !target_serial || target_serial > 79)
      return 0;

   return key_bindings_valid(r->token_public_key, r->token_public_digest, r->token_jwk_digest,
                             r->kid, r->hwm_attestation, r->hwm_attestation_len,
                             r->hwm_attestation_digest);
}

static int sign_rs256(void *opaque, const unsigned char *input, size_t input_len,
                      unsigned char *signature, size_t signature_cap, size_t *signature_len)
{
   authority_signer_t *s = opaque;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   EVP_PKEY_CTX *pk = NULL;
   size_t n = signature_cap;
   int ok = s && s->key && input && input_len && signature && signature_len && md &&
            EVP_DigestSignInit(md, &pk, EVP_sha256(), NULL, s->key) == 1 && pk &&
            EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1 &&
            EVP_DigestSign(md, signature, &n, input, input_len) == 1 && n == 384;
   EVP_MD_CTX_free(md);
   if (!ok)
   {
      if (signature && signature_cap)
         OPENSSL_cleanse(signature, signature_cap);
      if (signature_len)
         *signature_len = 0;
      return 0;
   }
   *signature_len = n;
   return 1;
}

static int b64_value(unsigned char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   return c == '-' ? 62 : (c == '_' ? 63 : -1);
}

static int decode_signature(const char *s, size_t n, unsigned char out[384])
{
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t used = 0;
   if (!s || n != 512)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      int v = b64_value((unsigned char)s[i]);
      if (v < 0)
         return 0;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (used >= 384)
            return 0;
         out[used++] = (unsigned char)(acc >> bits);
         acc &= bits ? (UINT32_C(1) << bits) - 1 : 0;
      }
   }
   return used == 384 && bits == 0 && acc == 0;
}

static int verify_exact(EVP_PKEY *key, const char *jwt, size_t jwt_len)
{
   const char *dot = NULL;
   if (jwt)
      for (size_t i = jwt_len; i > 0; --i)
         if (jwt[i - 1] == '.')
         {
            dot = jwt + i - 1;
            break;
         }
   unsigned char signature[384] = {0};
   EVP_MD_CTX *md = NULL;
   EVP_PKEY_CTX *pk = NULL;
   int ok = dot && (size_t)(dot - jwt) > 0 &&
            decode_signature(dot + 1, jwt_len - (size_t)(dot + 1 - jwt), signature) &&
            (md = EVP_MD_CTX_new()) &&
            EVP_DigestVerifyInit(md, &pk, EVP_sha256(), NULL, key) == 1 && pk &&
            EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1 &&
            EVP_DigestVerify(md, signature, sizeof(signature), (const unsigned char *)jwt,
                             (size_t)(dot - jwt)) == 1;
   EVP_MD_CTX_free(md);
   OPENSSL_cleanse(signature, sizeof(signature));
   return ok;
}

/* Decode the custody-released PKCS#8 blob and bind it to the modulus the record
 * publishes: exactly one RSA-3072/e=65537 private key whose modulus is the one
 * the JWKS already advertises. Returns the key (caller frees) or NULL — a NULL
 * return is always a key/record mismatch, never a claim problem. */
static EVP_PKEY *authority_signing_key(const unsigned char *der, size_t der_len,
                                       const uint8_t expected_modulus[KB_MGMT_TOKEN_MODULUS_LEN])
{
   const unsigned char *p = der;
   PKCS8_PRIV_KEY_INFO *p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, (long)der_len);
   EVP_PKEY *key = p8 && p == der + der_len ? EVP_PKCS82PKEY(p8) : NULL;
   EVP_PKEY_CTX *check = key ? EVP_PKEY_CTX_new(key, NULL) : NULL;
   BIGNUM *n = NULL, *e = NULL, *f1 = NULL, *f2 = NULL, *f3 = NULL;
   unsigned char modulus[KB_MGMT_TOKEN_MODULUS_LEN] = {0};
   int key_ok = key && EVP_PKEY_base_id(key) == EVP_PKEY_RSA && EVP_PKEY_bits(key) == 3072 &&
                check && EVP_PKEY_private_check(check) == 1 &&
                EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1 && n &&
                EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1 && e &&
                BN_is_word(e, 65537) &&
                EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR1, &f1) == 1 && f1 &&
                EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR2, &f2) == 1 && f2 &&
                EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR3, &f3) != 1 &&
                BN_num_bytes(n) == (int)sizeof(modulus) &&
                BN_bn2binpad(n, modulus, sizeof(modulus)) == (int)sizeof(modulus) && modulus[0] &&
                !CRYPTO_memcmp(modulus, expected_modulus, sizeof(modulus));

   OPENSSL_cleanse(modulus, sizeof(modulus));
   BN_free(n);
   BN_free(e);
   BN_clear_free(f1);
   BN_clear_free(f2);
   BN_clear_free(f3);
   EVP_PKEY_CTX_free(check);
   PKCS8_PRIV_KEY_INFO_free(p8);
   if (!key_ok)
   {
      EVP_PKEY_free(key);
      return NULL;
   }
   return key;
}

kb_mgmt_token_authority_result_t
kb_mgmt_token_authority_sign_pkcs8(const kb_mgmt_token_authority_record_t *r,
                                   const unsigned char *der, size_t der_len, char *jwt_out,
                                   size_t jwt_cap, size_t *jwt_len)
{
   if (jwt_len)
      *jwt_len = 0;
   if (jwt_out && jwt_cap)
      jwt_out[0] = 0;
   if (!jwt_out || !jwt_len || !der || !der_len || der_len > KB_MGMT_ROOT_SECRET_MAX ||
       !kb_mgmt_token_authority_record_valid(r))
      return KB_MGMT_TOKEN_AUTHORITY_INVALID;

   EVP_PKEY *key = authority_signing_key(der, der_len, r->token_public_key);

   kb_mgmt_token_authority_result_t result = KB_MGMT_TOKEN_AUTHORITY_KEY_MISMATCH;
   kb_mgmt_token_claims_t claims;
   memset(&claims, 0, sizeof(claims));
   if (key)
   {
      memcpy(claims.issuer, r->token_issuer, sizeof(claims.issuer));
      memcpy(claims.audience, r->audience, sizeof(claims.audience));
      memcpy(claims.subject, r->actor_identity, sizeof(claims.subject));
      claims.team_id = r->team_id;
      claims.capability = r->capability;
      memcpy(claims.jti, r->jti, sizeof(r->jti));
      memcpy(claims.correlation_id, r->correlation_id, sizeof(r->correlation_id));
      memcpy(claims.request_sha256, r->request_sha256, sizeof(claims.request_sha256));
      memcpy(claims.peer_issuer, r->target_mgmt_issuer, sizeof(claims.peer_issuer));
      memcpy(claims.peer_serial, r->target_mgmt_serial_norm, sizeof(claims.peer_serial));
      memcpy(claims.peer_fingerprint, r->target_mgmt_fingerprint, sizeof(claims.peer_fingerprint));
      memcpy(claims.kid, r->kid, sizeof(claims.kid));
      claims.issued_at = r->issued_at;
      claims.expires_at = r->expires_at;

      authority_signer_t signer = {key};
      kb_mgmt_token_result_t minted =
          kb_mgmt_token_build(&claims, sign_rs256, &signer, jwt_out, jwt_cap, jwt_len);
      if (minted == KB_MGMT_TOKEN_OUTPUT_TOO_SMALL)
         result = KB_MGMT_TOKEN_AUTHORITY_OUTPUT_TOO_SMALL;
      else if (minted != KB_MGMT_TOKEN_OK)
         result = KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE;
      else if (!verify_exact(key, jwt_out, *jwt_len))
      {
         OPENSSL_cleanse(jwt_out, jwt_cap);
         *jwt_len = 0;
         result = KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE;
      }
      else
         result = KB_MGMT_TOKEN_AUTHORITY_OK;
   }

   OPENSSL_cleanse(&claims, sizeof(claims));
   EVP_PKEY_free(key);
   if (result != KB_MGMT_TOKEN_AUTHORITY_OK)
   {
      OPENSSL_cleanse(jwt_out, jwt_cap);
      *jwt_len = 0;
   }
   return result;
}
