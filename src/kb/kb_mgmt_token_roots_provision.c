#include "kb_mgmt_token_roots_provision.h"

#include "vault_server_key.h"

#include <limits.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#define TOKEN_PRINCIPAL    "org:p5-token"
#define TOKEN_AGENT        "management"
#define TOKEN_CRED         "rs256"
#define MANIFEST_PRINCIPAL "org:p5-jwks-manifest"
#define MANIFEST_AGENT     "management"
#define MANIFEST_CRED      "ed25519"

typedef struct
{
   uint8_t secret[KB_MGMT_ROOT_SECRET_MAX];
   uint8_t plaintext[KB_MGMT_ROOT_SECRET_MAX];
   uint8_t dek[VAULT_DEK_LEN];
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
} root_secret_arena_t;

typedef struct
{
   kb_mgmt_root_kind_t kind;
   int build;
   kb_mgmt_root_record_t *record;
   kb_mgmt_roots_result_t result;
} secret_call_t;

static int sha256(const void *value, size_t len, uint8_t out[32])
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            (!len || EVP_DigestUpdate(md, value, len) == 1) &&
            EVP_DigestFinal_ex(md, out, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   if (!ok)
      OPENSSL_cleanse(out, 32);
   return ok ? 0 : -1;
}

static void put_u32be(uint8_t out[4], uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

static void put_u64be(uint8_t out[8], uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static void hex_encode(const uint8_t *value, size_t len, char *out)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < len; ++i)
   {
      out[i * 2] = hex[value[i] >> 4];
      out[i * 2 + 1] = hex[value[i] & 15];
   }
   out[len * 2] = 0;
}

static int fixed_text(const char *s, size_t max)
{
   return s && s[0] && strnlen(s, max + 1) <= max;
}

static int b64url_encode(const uint8_t *value, size_t len, char *out, size_t cap, size_t *out_len)
{
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!value || !out || !out_len || len > (INT_MAX - 2) / 4 * 3)
      return -1;
   size_t padded = 4 * ((len + 2) / 3);
   size_t actual = padded;
   while (actual &&
          ((len % 3 == 1 && padded - actual < 2) || (len % 3 == 2 && padded - actual < 1)))
      --actual;
   if (actual + 1 > cap)
      return -1;
   unsigned char tmp[KB_MGMT_TOKEN_JWK_MAX];
   if (padded >= sizeof(tmp) || EVP_EncodeBlock(tmp, value, (int)len) != (int)padded)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   for (size_t i = 0; i < actual; ++i)
      out[i] = tmp[i] == '+' ? '-' : (tmp[i] == '/' ? '_' : (char)tmp[i]);
   out[actual] = 0;
   *out_len = actual;
   OPENSSL_cleanse(tmp, sizeof(tmp));
   return 0;
}

static int b64url_decode(const char *value, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!value || !len || !out || !out_len || len % 4 == 1 || len > KB_MGMT_TOKEN_JWK_MAX - 4)
      return -1;
   size_t padded = (len + 3) & ~(size_t)3;
   unsigned char tmp[KB_MGMT_TOKEN_JWK_MAX] = {0};
   uint8_t decoded_bytes[KB_MGMT_TOKEN_JWK_MAX] = {0};
   for (size_t i = 0; i < len; ++i)
   {
      unsigned char c = (unsigned char)value[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_'))
         return -1;
      tmp[i] = c == '-' ? '+' : (c == '_' ? '/' : c);
   }
   for (size_t i = len; i < padded; ++i)
      tmp[i] = '=';
   size_t decoded = padded / 4 * 3 - (padded - len);
   int raw_len = EVP_DecodeBlock(decoded_bytes, tmp, (int)padded);
   if (decoded > cap || raw_len < 0 || (size_t)raw_len != padded / 4 * 3)
   {
      OPENSSL_cleanse(out, cap);
      OPENSSL_cleanse(decoded_bytes, sizeof(decoded_bytes));
      return -1;
   }
   memcpy(out, decoded_bytes, decoded);
   /* Re-encoding is the simplest strict trailing-bit/canonicality check. */
   char canonical[KB_MGMT_TOKEN_JWK_MAX];
   size_t canonical_len = 0;
   int ok = !b64url_encode(out, decoded, canonical, sizeof(canonical), &canonical_len) &&
            canonical_len == len && !CRYPTO_memcmp(canonical, value, len);
   OPENSSL_cleanse(tmp, sizeof(tmp));
   OPENSSL_cleanse(decoded_bytes, sizeof(decoded_bytes));
   OPENSSL_cleanse(canonical, sizeof(canonical));
   if (!ok)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   *out_len = decoded;
   return 0;
}

int kb_mgmt_manifest_wire_id(const uint8_t public_key[32], char *out, size_t cap)
{
   uint8_t digest[32];
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (!public_key || !out || cap < 49 || sha256(public_key, 32, digest))
      return -1;
   memcpy(out, "p5-jwks-root-v1-", 16);
   hex_encode(digest, 16, out + 16);
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static int bundle_payload(const uint8_t *modulus, size_t modulus_len, const uint8_t manifest[32],
                          const uint8_t publication[32], char *out, size_t cap, size_t *out_len)
{
   char kid[65], jwk[KB_MGMT_TOKEN_JWK_MAX], manifest_id[65], manifest_b64[64], publication_hex[65];
   size_t jwk_len = 0, manifest_len = 0;
   if (kb_mgmt_token_kid(modulus, modulus_len, kid, sizeof(kid)) ||
       kb_mgmt_token_jwk(modulus, modulus_len, jwk, sizeof(jwk), &jwk_len) ||
       kb_mgmt_manifest_wire_id(manifest, manifest_id, sizeof(manifest_id)) ||
       b64url_encode(manifest, 32, manifest_b64, sizeof(manifest_b64), &manifest_len))
      return -1;
   hex_encode(publication, 32, publication_hex);
   int n = snprintf(out, cap,
                    "{\"format_version\":1,\"token_kid\":\"%s\",\"token_jwk\":%s,"
                    "\"manifest_wire_id\":\"%s\",\"manifest_public_key\":\"%s\","
                    "\"publication_hwm_identity_digest\":\"%s\"}",
                    kid, jwk, manifest_id, manifest_b64, publication_hex);
   OPENSSL_cleanse(kid, sizeof(kid));
   OPENSSL_cleanse(jwk, sizeof(jwk));
   OPENSSL_cleanse(manifest_id, sizeof(manifest_id));
   OPENSSL_cleanse(manifest_b64, sizeof(manifest_b64));
   OPENSSL_cleanse(publication_hex, sizeof(publication_hex));
   if (n < 0 || (size_t)n >= cap)
      return -1;
   *out_len = (size_t)n;
   return 0;
}

int kb_mgmt_public_bundle(const uint8_t *token_modulus, size_t token_modulus_len,
                          const uint8_t manifest_public[32],
                          const uint8_t publication_identity_digest[32], char *out, size_t cap,
                          size_t *out_len)
{
   char payload[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t payload_len = 0;
   uint8_t digest[32];
   char digest_hex[65];
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!token_modulus || !manifest_public || !publication_identity_digest || !out || !out_len ||
       bundle_payload(token_modulus, token_modulus_len, manifest_public,
                      publication_identity_digest, payload, sizeof(payload), &payload_len) ||
       sha256(payload, payload_len, digest))
      return -1;
   hex_encode(digest, sizeof(digest), digest_hex);
   /* Replace the payload's final brace; bundle_sha256 is outside its own preimage. */
   int n = snprintf(out, cap, "%.*s,\"bundle_sha256\":\"%s\"}", (int)payload_len - 1, payload,
                    digest_hex);
   OPENSSL_cleanse(payload, sizeof(payload));
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(digest_hex, sizeof(digest_hex));
   if (n < 0 || (size_t)n >= cap)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   *out_len = (size_t)n;
   return 0;
}

static int hex_decode_32(const char *s, uint8_t out[32])
{
   for (size_t i = 0; i < 32; ++i)
   {
      unsigned char a = (unsigned char)s[i * 2], b = (unsigned char)s[i * 2 + 1];
      if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) ||
          !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f')))
         return -1;
      out[i] = (uint8_t)(((a <= '9' ? a - '0' : a - 'a' + 10) << 4) |
                         (b <= '9' ? b - '0' : b - 'a' + 10));
   }
   return 0;
}

int kb_mgmt_public_bundle_validate(const char *bundle, size_t bundle_len)
{
   static const char token_mark[] = "\"token_jwk\":";
   static const char manifest_mark[] = ",\"manifest_wire_id\":\"";
   static const char pub_mark[] = "\",\"manifest_public_key\":\"";
   static const char hwm_mark[] = "\",\"publication_hwm_identity_digest\":\"";
   static const char digest_mark[] = ",\"bundle_sha256\":\"";
   if (!bundle || !bundle_len || bundle_len >= KB_MGMT_PUBLIC_BUNDLE_MAX)
      return -1;
   char bounded[KB_MGMT_PUBLIC_BUNDLE_MAX];
   memcpy(bounded, bundle, bundle_len);
   bounded[bundle_len] = 0;
   const char *j = strstr(bounded, token_mark);
   const char *mi = j ? strstr(j, manifest_mark) : NULL;
   const char *mp = mi ? strstr(mi, pub_mark) : NULL;
   const char *hi = mp ? strstr(mp, hwm_mark) : NULL;
   const char *di = hi ? strstr(hi, digest_mark) : NULL;
   if (!j || !mi || !mp || !hi || !di ||
       di + sizeof(digest_mark) - 1 + 64 + 2 != bounded + bundle_len ||
       di[sizeof(digest_mark) - 1 + 64] != '"' || di[sizeof(digest_mark) + 64] != '}')
      return -1;
   j += sizeof(token_mark) - 1;
   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN], manifest[32], publication[32];
   size_t modulus_len = 0, manifest_len = 0;
   if (kb_mgmt_token_jwk_validate(j, (size_t)(mi - j), modulus, sizeof(modulus), &modulus_len))
      return -1;
   mp += sizeof(pub_mark) - 1;
   if ((size_t)(hi - mp) != 43 ||
       b64url_decode(mp, 43, manifest, sizeof(manifest), &manifest_len) || manifest_len != 32)
      goto fail;
   hi += sizeof(hwm_mark) - 1;
   if ((size_t)(di - hi) != 65 || hi[64] != '"' || hex_decode_32(hi, publication))
      goto fail;
   char expected[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t expected_len = 0;
   int ok = !kb_mgmt_public_bundle(modulus, modulus_len, manifest, publication, expected,
                                   sizeof(expected), &expected_len) &&
            expected_len == bundle_len && !CRYPTO_memcmp(expected, bounded, bundle_len);
   OPENSSL_cleanse(expected, sizeof(expected));
   OPENSSL_cleanse(modulus, sizeof(modulus));
   OPENSSL_cleanse(manifest, sizeof(manifest));
   OPENSSL_cleanse(publication, sizeof(publication));
   OPENSSL_cleanse(bounded, sizeof(bounded));
   return ok ? 0 : -1;
fail:
   OPENSSL_cleanse(modulus, sizeof(modulus));
   OPENSSL_cleanse(manifest, sizeof(manifest));
   OPENSSL_cleanse(publication, sizeof(publication));
   OPENSSL_cleanse(bounded, sizeof(bounded));
   return -1;
}

static const char *root_principal(kb_mgmt_root_kind_t kind)
{
   return kind == KB_MGMT_ROOT_TOKEN      ? TOKEN_PRINCIPAL
          : kind == KB_MGMT_ROOT_MANIFEST ? MANIFEST_PRINCIPAL
                                          : NULL;
}

static const char *root_cred(kb_mgmt_root_kind_t kind)
{
   return kind == KB_MGMT_ROOT_TOKEN      ? TOKEN_CRED
          : kind == KB_MGMT_ROOT_MANIFEST ? MANIFEST_CRED
                                          : NULL;
}

static const char *root_agent(kb_mgmt_root_kind_t kind)
{
   return kind == KB_MGMT_ROOT_TOKEN      ? TOKEN_AGENT
          : kind == KB_MGMT_ROOT_MANIFEST ? MANIFEST_AGENT
                                          : NULL;
}

static int aad_field(uint8_t *out, size_t cap, size_t *off, const void *value, size_t len)
{
   if (!out || !off || (!value && len) || len > UINT32_MAX || *off > cap || cap - *off < 4 + len)
      return -1;
   put_u32be(out + *off, (uint32_t)len);
   *off += 4;
   if (len)
   {
      memcpy(out + *off, value, len);
      *off += len;
   }
   return 0;
}

int kb_mgmt_root_aad(kb_mgmt_root_kind_t kind, int64_t version, uint8_t *out, size_t cap,
                     size_t *out_len)
{
   static const char manifest_domain[] = "aimee.p5.manifest-root.envelope.aad.v1";
   if (kind == KB_MGMT_ROOT_TOKEN)
      return kb_mgmt_token_root_aad(version, out, cap, out_len);
   const char *domain = kind == KB_MGMT_ROOT_MANIFEST ? manifest_domain : NULL;
   const char *principal = root_principal(kind), *agent = root_agent(kind), *cred = root_cred(kind);
   uint8_t encoded_version[8];
   size_t off = 0;
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!domain || !principal || !agent || !cred || version < 1 || !out || !out_len)
      return -1;
   put_u64be(encoded_version, (uint64_t)version);
   if (aad_field(out, cap, &off, domain, strlen(domain)) ||
       aad_field(out, cap, &off, principal, strlen(principal)) ||
       aad_field(out, cap, &off, agent, strlen(agent)) ||
       aad_field(out, cap, &off, cred, strlen(cred)) ||
       aad_field(out, cap, &off, encoded_version, sizeof(encoded_version)))
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   *out_len = off;
   return 0;
}

int kb_mgmt_root_bootstrap_id(kb_mgmt_root_kind_t kind, const char *custody_key_id,
                              char out[KB_MGMT_ROOT_BOOTSTRAP_LEN + 1])
{
   static const char token_domain[] = "aimee-p5-token-root-bootstrap-v1|";
   static const char manifest_domain[] = "aimee-p5-jwks-manifest-root-bootstrap-v1|";
   const char *domain = kind == KB_MGMT_ROOT_TOKEN ? token_domain : manifest_domain;
   size_t domain_len =
       kind == KB_MGMT_ROOT_TOKEN ? sizeof(token_domain) - 1 : sizeof(manifest_domain) - 1;
   uint8_t digest[32];
   if (out)
      OPENSSL_cleanse(out, KB_MGMT_ROOT_BOOTSTRAP_LEN + 1);
   if (!root_principal(kind) || !fixed_text(custody_key_id, KB_MGMT_ROOT_CUSTODY_ID_MAX) || !out)
      return -1;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, domain_len) == 1 &&
            EVP_DigestUpdate(md, custody_key_id, strlen(custody_key_id)) == 1 &&
            EVP_DigestFinal_ex(md, digest, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   if (!ok)
      return -1;
   hex_encode(digest, 32, out);
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static int digest_field(EVP_MD_CTX *md, const void *value, size_t len)
{
   uint8_t encoded[4];
   if (len > UINT32_MAX)
      return -1;
   put_u32be(encoded, (uint32_t)len);
   return EVP_DigestUpdate(md, encoded, 4) == 1 && (!len || EVP_DigestUpdate(md, value, len) == 1)
              ? 0
              : -1;
}

int kb_mgmt_root_envelope_digest(kb_mgmt_root_kind_t kind, const kb_mgmt_root_envelope_t *e,
                                 uint8_t out[32])
{
   static const char domain[] = "aimee.management.root.envelope.v1\n";
   uint8_t version[8], kind_byte = (uint8_t)kind, aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   if (out)
      OPENSSL_cleanse(out, 32);
   if (!root_principal(kind) || !e || !out || (e->version != 1 && e->version != 2) ||
       !e->ciphertext_len || e->ciphertext_len > KB_MGMT_ROOT_SECRET_MAX ||
       kb_mgmt_root_aad(kind, e->version, aad, sizeof(aad), &aad_len))
      return -1;
   put_u64be(version, (uint64_t)e->version);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(md, &kind_byte, 1) == 1 && EVP_DigestUpdate(md, version, 8) == 1 &&
            !digest_field(md, aad, aad_len) &&
            !digest_field(md, e->wrapped_dek, sizeof(e->wrapped_dek)) &&
            !digest_field(md, e->nonce, sizeof(e->nonce)) &&
            !digest_field(md, e->ciphertext, e->ciphertext_len) &&
            !digest_field(md, e->tag, sizeof(e->tag)) && EVP_DigestFinal_ex(md, out, &n) == 1 &&
            n == 32;
   EVP_MD_CTX_free(md);
   OPENSSL_cleanse(aad, sizeof(aad));
   if (!ok)
      OPENSSL_cleanse(out, 32);
   return ok ? 0 : -1;
}

static root_secret_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   long page = mapped ? sysconf(_SC_PAGESIZE) : -1;
   if (page <= 0)
      return NULL;
   size_t n = (sizeof(root_secret_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return NULL;
   if (mlock(p, n) || madvise(p, n, MADV_DONTDUMP) || madvise(p, n, MADV_WIPEONFORK))
   {
      OPENSSL_cleanse(p, n);
      (void)munlock(p, n);
      (void)munmap(p, n);
      return NULL;
   }
   *mapped = n;
   return p;
#else
   (void)mapped;
   return NULL;
#endif
}

static void arena_free(root_secret_arena_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#endif
}

static int token_key_generate(root_secret_arena_t *arena, size_t *secret_len)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   PKCS8_PRIV_KEY_INFO *p8 = NULL;
   unsigned char *p = arena->secret;
   int n = 0;
   int ok = ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 3072) == 1 && EVP_PKEY_keygen(ctx, &key) == 1 &&
            (p8 = EVP_PKEY2PKCS8(key)) && (n = i2d_PKCS8_PRIV_KEY_INFO(p8, NULL)) > 0 &&
            n <= KB_MGMT_ROOT_SECRET_MAX && i2d_PKCS8_PRIV_KEY_INFO(p8, &p) == n;
   PKCS8_PRIV_KEY_INFO_free(p8);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(ctx);
   if (!ok)
   {
      OPENSSL_cleanse(arena->secret, sizeof(arena->secret));
      return -1;
   }
   *secret_len = (size_t)n;
   return 0;
}

static int token_binding(const uint8_t *der, size_t der_len, kb_mgmt_root_record_t *r, int compare)
{
   const unsigned char *p = der;
   PKCS8_PRIV_KEY_INFO *p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, (long)der_len);
   EVP_PKEY *key = p8 && p == der + der_len ? EVP_PKCS82PKEY(p8) : NULL;
   EVP_PKEY_CTX *check = key ? EVP_PKEY_CTX_new(key, NULL) : NULL;
   BIGNUM *n = NULL, *e = NULL, *f1 = NULL, *f2 = NULL, *f3 = NULL;
   int ok = key && EVP_PKEY_base_id(key) == EVP_PKEY_RSA && EVP_PKEY_bits(key) == 3072 && check &&
            EVP_PKEY_private_check(check) == 1 &&
            EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1 && n &&
            EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1 && e &&
            BN_is_word(e, 65537) &&
            EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR1, &f1) == 1 && f1 &&
            EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR2, &f2) == 1 && f2 &&
            EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_FACTOR3, &f3) != 1 &&
            BN_num_bytes(n) == KB_MGMT_TOKEN_MODULUS_LEN;
   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN], digest[32], jwk_digest[32];
   char wire[65], jwk[KB_MGMT_TOKEN_JWK_MAX];
   size_t jwk_len = 0;
   if (ok)
      ok = BN_bn2binpad(n, modulus, sizeof(modulus)) == (int)sizeof(modulus) && modulus[0] &&
           !sha256(modulus, sizeof(modulus), digest) &&
           !kb_mgmt_token_kid(modulus, sizeof(modulus), wire, sizeof(wire)) &&
           !kb_mgmt_token_jwk(modulus, sizeof(modulus), jwk, sizeof(jwk), &jwk_len) &&
           !sha256(jwk, jwk_len, jwk_digest);
   if (ok && compare)
      ok = r->public_key_len == sizeof(modulus) &&
           !CRYPTO_memcmp(r->public_key, modulus, sizeof(modulus)) &&
           !CRYPTO_memcmp(r->public_digest, digest, 32) &&
           !CRYPTO_memcmp(r->jwk_digest, jwk_digest, 32) && !strcmp(r->wire_id, wire);
   else if (ok)
   {
      memcpy(r->public_key, modulus, sizeof(modulus));
      r->public_key_len = sizeof(modulus);
      memcpy(r->public_digest, digest, 32);
      memcpy(r->jwk_digest, jwk_digest, 32);
      snprintf(r->wire_id, sizeof(r->wire_id), "%s", wire);
   }
   BN_free(n);
   BN_free(e);
   BN_clear_free(f1);
   BN_clear_free(f2);
   BN_clear_free(f3);
   EVP_PKEY_CTX_free(check);
   EVP_PKEY_free(key);
   PKCS8_PRIV_KEY_INFO_free(p8);
   OPENSSL_cleanse(modulus, sizeof(modulus));
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(jwk_digest, sizeof(jwk_digest));
   OPENSSL_cleanse(wire, sizeof(wire));
   OPENSSL_cleanse(jwk, sizeof(jwk));
   return ok ? 0 : -1;
}

static int manifest_binding(const uint8_t seed[32], kb_mgmt_root_record_t *r, int compare)
{
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
   uint8_t public_key[32], digest[32];
   size_t n = sizeof(public_key);
   char wire[65];
   int ok = key && EVP_PKEY_get_raw_public_key(key, public_key, &n) == 1 && n == 32 &&
            !sha256(public_key, 32, digest) &&
            !kb_mgmt_manifest_wire_id(public_key, wire, sizeof(wire));
   if (ok && compare)
      ok = r->public_key_len == 32 && !CRYPTO_memcmp(r->public_key, public_key, 32) &&
           !CRYPTO_memcmp(r->public_digest, digest, 32) && !strcmp(r->wire_id, wire);
   else if (ok)
   {
      memcpy(r->public_key, public_key, 32);
      r->public_key_len = 32;
      memcpy(r->public_digest, digest, 32);
      OPENSSL_cleanse(r->jwk_digest, 32);
      snprintf(r->wire_id, sizeof(r->wire_id), "%s", wire);
   }
   EVP_PKEY_free(key);
   OPENSSL_cleanse(public_key, sizeof(public_key));
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(wire, sizeof(wire));
   return ok ? 0 : -1;
}

static int envelope_encrypt(kb_mgmt_root_kind_t kind, const uint8_t kek[VAULT_KEK_LEN],
                            root_secret_arena_t *a, size_t secret_len, int64_t version,
                            kb_mgmt_root_envelope_t *e)
{
   size_t aad_len = 0;
   memset(e, 0, sizeof(*e));
   e->version = version;
   e->ciphertext_len = secret_len;
   if (kb_mgmt_root_aad(kind, version, a->aad, sizeof(a->aad), &aad_len) ||
       vault_crypto_random(a->dek, sizeof(a->dek)) ||
       vault_secret_encrypt(a->dek, a->aad, aad_len, a->secret, secret_len, e->nonce, e->ciphertext,
                            e->tag) ||
       vault_dek_wrap(kek, a->dek, e->wrapped_dek))
   {
      OPENSSL_cleanse(e, sizeof(*e));
      return -1;
   }
   OPENSSL_cleanse(a->dek, sizeof(a->dek));
   OPENSSL_cleanse(a->aad, sizeof(a->aad));
   return 0;
}

static int secret_callback(const uint8_t kek[VAULT_KEK_LEN], void *opaque)
{
   secret_call_t *call = opaque;
   root_secret_arena_t *a = NULL;
   size_t mapped = 0, secret_len = 0, aad_len = 0;
   a = arena_new(&mapped);
   if (!a)
      return -1;
   int rc = -1;
   if (call->build)
   {
      int generated = call->kind == KB_MGMT_ROOT_TOKEN
                          ? token_key_generate(a, &secret_len)
                          : (vault_crypto_random(a->secret, 32) ? -1 : (secret_len = 32, 0));
      if (!generated &&
          !(call->kind == KB_MGMT_ROOT_TOKEN ? token_binding(a->secret, secret_len, call->record, 0)
                                             : manifest_binding(a->secret, call->record, 0)) &&
          !envelope_encrypt(call->kind, kek, a, secret_len, 1, &call->record->v1) &&
          !envelope_encrypt(call->kind, kek, a, secret_len, 2, &call->record->v2) &&
          !kb_mgmt_root_envelope_digest(call->kind, &call->record->v1, call->record->v1_digest) &&
          !kb_mgmt_root_envelope_digest(call->kind, &call->record->v2, call->record->v2_digest))
         rc = 0;
   }
   else
   {
      secret_len = call->record->v2.ciphertext_len;
      if (secret_len && secret_len <= sizeof(a->plaintext) &&
          !kb_mgmt_root_aad(call->kind, 2, a->aad, sizeof(a->aad), &aad_len) &&
          !vault_dek_unwrap(kek, call->record->v2.wrapped_dek, a->dek) &&
          !vault_secret_decrypt(a->dek, a->aad, aad_len, call->record->v2.nonce,
                                call->record->v2.ciphertext, secret_len, call->record->v2.tag,
                                a->plaintext) &&
          !(call->kind == KB_MGMT_ROOT_TOKEN
                ? token_binding(a->plaintext, secret_len, call->record, 1)
                : (secret_len == 32 ? manifest_binding(a->plaintext, call->record, 1) : -1)))
         rc = 0;
   }
   arena_free(a, mapped);
   call->result =
       rc ? (call->build ? KB_MGMT_ROOTS_RETRY : KB_MGMT_ROOTS_INTEGRITY) : KB_MGMT_ROOTS_FRESH;
   return rc;
}

static kb_mgmt_roots_result_t protected_secret(kb_mgmt_root_record_t *record, int build)
{
   vault_maintenance_guard_t *guard = NULL;
   secret_call_t call = {.kind = record->kind,
                         .build = build,
                         .record = record,
                         .result = build ? KB_MGMT_ROOTS_RETRY : KB_MGMT_ROOTS_INTEGRITY};
   if (vault_maintenance_guard_begin(&guard) != VAULT_MAINTENANCE_OK)
      return KB_MGMT_ROOTS_RETRY;
   if (vault_maintenance_guard_sync_primary_epoch(guard, record->seal_epoch) !=
           VAULT_MAINTENANCE_OK ||
       vault_maintenance_guard_unseal(guard, NULL, 0) != VAULT_MAINTENANCE_OK)
      goto done;
   int rc = vault_maintenance_guard_with_active_kek(guard, secret_callback, &call);
   if (rc == VAULT_MAINTENANCE_SEALED)
      call.result = KB_MGMT_ROOTS_SEALED;
   else if (rc != VAULT_MAINTENANCE_OK && build)
      call.result = KB_MGMT_ROOTS_RETRY;
done:
   if (vault_maintenance_guard_end(&guard) != VAULT_MAINTENANCE_OK)
      return KB_MGMT_ROOTS_RETRY;
   return call.result;
}

static kb_mgmt_roots_result_t db_result(kb_mgmt_root_db_result_t rc)
{
   switch (rc)
   {
   case KB_MGMT_ROOT_DB_SEALED:
      return KB_MGMT_ROOTS_SEALED;
   case KB_MGMT_ROOT_DB_CONFLICT:
      return KB_MGMT_ROOTS_CONFLICT;
   case KB_MGMT_ROOT_DB_INTEGRITY:
      return KB_MGMT_ROOTS_INTEGRITY;
   default:
      return KB_MGMT_ROOTS_RETRY;
   }
}

static kb_mgmt_roots_result_t hwm_read(const char *id, uint64_t *version, uint8_t att[512],
                                       size_t *att_len)
{
   OPENSSL_cleanse(att, 512);
   *version = 0;
   *att_len = 0;
   if (vault_hwm_read(id, version, att, 512, att_len) || !*version || !*att_len || *att_len > 512 ||
       vault_hwm_verify(id, *version, att, *att_len))
      return KB_MGMT_ROOTS_RETRY;
   return KB_MGMT_ROOTS_FRESH;
}

static int fixed_root(const kb_mgmt_root_record_t *r, kb_mgmt_root_kind_t kind, const char *id)
{
   char bootstrap[65];
   uint8_t digest[32];
   size_t expected_secret = kind == KB_MGMT_ROOT_MANIFEST ? 32 : 0;
   int ok = r && r->kind == kind && r->phase >= KB_MGMT_ROOT_STAGED &&
            r->phase <= KB_MGMT_ROOT_FINAL && r->seal_epoch &&
            fixed_text(r->custody_key_id, KB_MGMT_ROOT_CUSTODY_ID_MAX) &&
            !strcmp(r->custody_key_id, id) && !kb_mgmt_root_bootstrap_id(kind, id, bootstrap) &&
            !strcmp(r->bootstrap_id, bootstrap) && fixed_text(r->wire_id, KB_MGMT_TOKEN_KID_MAX) &&
            r->hwm1_attestation_len && r->hwm1_attestation_len <= 512 &&
            !vault_hwm_verify(id, 1, r->hwm1_attestation, r->hwm1_attestation_len) &&
            (r->phase < KB_MGMT_ROOT_CAS_DONE ||
             (r->hwm2_attestation_len && r->hwm2_attestation_len <= 512 &&
              !vault_hwm_verify(id, 2, r->hwm2_attestation, r->hwm2_attestation_len))) &&
            r->v1.version == 1 && r->v2.version == 2 && r->v1.ciphertext_len &&
            r->v1.ciphertext_len == r->v2.ciphertext_len &&
            (!expected_secret || r->v1.ciphertext_len == expected_secret) &&
            (kind != KB_MGMT_ROOT_TOKEN || r->v1.ciphertext_len <= KB_MGMT_ROOT_SECRET_MAX) &&
            !kb_mgmt_root_envelope_digest(kind, &r->v1, digest) &&
            !CRYPTO_memcmp(digest, r->v1_digest, 32) &&
            !kb_mgmt_root_envelope_digest(kind, &r->v2, digest) &&
            !CRYPTO_memcmp(digest, r->v2_digest, 32);
   OPENSSL_cleanse(bootstrap, sizeof(bootstrap));
   OPENSSL_cleanse(digest, sizeof(digest));
   return ok;
}

static kb_mgmt_roots_result_t advance_hwm(const char *id, uint64_t live, uint8_t att[512],
                                          size_t *att_len)
{
   if (live == 2)
      return KB_MGMT_ROOTS_FRESH;
   if (live != 1)
      return KB_MGMT_ROOTS_INTEGRITY;
   OPENSSL_cleanse(att, 512);
   *att_len = 0;
   if (!vault_hwm_cas(id, 1, 2, att, 512, att_len) && *att_len && *att_len <= 512 &&
       !vault_hwm_verify(id, 2, att, *att_len))
      return KB_MGMT_ROOTS_FRESH;
   uint64_t reread = 0;
   kb_mgmt_roots_result_t rc = hwm_read(id, &reread, att, att_len);
   if (rc != KB_MGMT_ROOTS_FRESH)
      return rc;
   return reread == 2   ? KB_MGMT_ROOTS_FRESH
          : reread == 1 ? KB_MGMT_ROOTS_RETRY
                        : KB_MGMT_ROOTS_INTEGRITY;
}

static kb_mgmt_roots_result_t root_step(const kb_mgmt_roots_db_t *db, kb_mgmt_root_kind_t kind,
                                        const char *id, kb_mgmt_root_record_t *r, int *mutated,
                                        int *all_fresh)
{
   kb_mgmt_root_db_result_t dr = db->inspect_root(db->ctx, kind, id, r);
   if (dr != KB_MGMT_ROOT_DB_OK)
      return db_result(dr);
   int fresh = r->phase == KB_MGMT_ROOT_EMPTY;
   if (!fresh)
      *all_fresh = 0;
   if ((fresh && (!r->seal_epoch || r->kind != kind)) || (!fresh && !fixed_root(r, kind, id)))
      return KB_MGMT_ROOTS_INTEGRITY;
   uint8_t att[512];
   size_t att_len = 0;
   uint64_t live = 0;
   kb_mgmt_roots_result_t rc = hwm_read(id, &live, att, &att_len);
   if (rc != KB_MGMT_ROOTS_FRESH)
      goto done;
   if (live < 1 || live > 2 || (fresh && live != 1) ||
       (r->phase >= KB_MGMT_ROOT_CAS_DONE && live != 2))
   {
      rc = KB_MGMT_ROOTS_INTEGRITY;
      goto done;
   }
   if (fresh)
   {
      r->kind = kind;
      r->phase = KB_MGMT_ROOT_STAGED;
      snprintf(r->custody_key_id, sizeof(r->custody_key_id), "%s", id);
      if (kb_mgmt_root_bootstrap_id(kind, id, r->bootstrap_id) ||
          att_len > sizeof(r->hwm1_attestation))
      {
         rc = KB_MGMT_ROOTS_INTEGRITY;
         goto done;
      }
      memcpy(r->hwm1_attestation, att, att_len);
      r->hwm1_attestation_len = att_len;
      rc = protected_secret(r, 1);
      if (rc != KB_MGMT_ROOTS_FRESH)
         goto done;
      dr = db->stage_root(db->ctx, r);
      if (dr != KB_MGMT_ROOT_DB_OK)
      {
         rc = db_result(dr);
         goto done;
      }
      *mutated = 1;
   }
   else
   {
      rc = protected_secret(r, 0);
      if (rc != KB_MGMT_ROOTS_FRESH)
         goto done;
   }
   if (r->phase == KB_MGMT_ROOT_STAGED)
   {
      rc = advance_hwm(id, live, att, &att_len);
      if (rc != KB_MGMT_ROOTS_FRESH)
         goto done;
      dr = db->record_cas(db->ctx, r, att, att_len);
      if (dr != KB_MGMT_ROOT_DB_OK)
      {
         rc = db_result(dr);
         goto done;
      }
      r->phase = KB_MGMT_ROOT_CAS_DONE;
      memcpy(r->hwm2_attestation, att, att_len);
      r->hwm2_attestation_len = att_len;
      *mutated = 1;
   }
   if (r->phase == KB_MGMT_ROOT_CAS_DONE)
   {
      dr = db->finalize_root(db->ctx, r);
      if (dr != KB_MGMT_ROOT_DB_OK)
      {
         rc = db_result(dr);
         goto done;
      }
      r->phase = KB_MGMT_ROOT_FINAL;
      *mutated = 1;
   }
   rc = r->phase == KB_MGMT_ROOT_FINAL ? KB_MGMT_ROOTS_FRESH : KB_MGMT_ROOTS_INTEGRITY;
done:
   OPENSSL_cleanse(att, sizeof(att));
   return rc;
}

static int config_valid(const kb_mgmt_roots_config_t *c, const kb_mgmt_roots_db_t *db)
{
   return c && db && fixed_text(c->token_custody_key_id, KB_MGMT_ROOT_CUSTODY_ID_MAX) &&
          fixed_text(c->manifest_custody_key_id, KB_MGMT_ROOT_CUSTODY_ID_MAX) &&
          fixed_text(c->publication_custody_key_id, KB_MGMT_ROOT_CUSTODY_ID_MAX) &&
          strcmp(c->token_custody_key_id, c->manifest_custody_key_id) &&
          strcmp(c->token_custody_key_id, c->publication_custody_key_id) &&
          strcmp(c->manifest_custody_key_id, c->publication_custody_key_id) &&
          fixed_text(c->publication_helper, 128) &&
          fixed_text(c->publication_verifier_domain, 128) && db->inspect_root && db->stage_root &&
          db->record_cas && db->finalize_root && db->inspect_publication && db->bind_publication;
}

static kb_mgmt_roots_result_t publication_step(const kb_mgmt_roots_config_t *c,
                                               const kb_mgmt_roots_db_t *db, int *mutated,
                                               int *all_fresh, int require_bound)
{
   kb_mgmt_publication_root_t p;
   memset(&p, 0, sizeof(p));
   kb_mgmt_root_db_result_t dr = db->inspect_publication(db->ctx, &p);
   if (dr != KB_MGMT_ROOT_DB_OK)
      return db_result(dr);
   uint8_t att[512];
   size_t att_len = 0;
   uint64_t live = 0;
   kb_mgmt_roots_result_t rc = hwm_read(c->publication_custody_key_id, &live, att, &att_len);
   if (rc != KB_MGMT_ROOTS_FRESH || live < 1 || live > 2)
   {
      if (rc == KB_MGMT_ROOTS_FRESH)
         rc = KB_MGMT_ROOTS_INTEGRITY;
      goto done;
   }
   if (p.bound)
   {
      *all_fresh = 0;
      if (strcmp(p.custody_key_id, c->publication_custody_key_id) ||
          strcmp(p.helper, c->publication_helper) ||
          strcmp(p.verifier_domain, c->publication_verifier_domain) ||
          CRYPTO_memcmp(p.identity_digest, c->publication_identity_digest, 32) ||
          !p.hwm1_attestation_len || p.hwm1_attestation_len > sizeof(p.hwm1_attestation) ||
          vault_hwm_verify(p.custody_key_id, 1, p.hwm1_attestation, p.hwm1_attestation_len) ||
          (live == 1 && (p.hwm1_attestation_len != att_len ||
                         CRYPTO_memcmp(p.hwm1_attestation, att, att_len))))
         rc = KB_MGMT_ROOTS_INTEGRITY;
   }
   else
   {
      /* A bound root may legitimately be at HWM 2 after the publisher's
       * irreversible CAS. An unbound root must still begin at 1: accepting 2
       * there would attach a database record after an unaccounted private use. */
      if (require_bound || live != 1)
      {
         rc = KB_MGMT_ROOTS_INTEGRITY;
         goto done;
      }
      p.bound = 1;
      snprintf(p.custody_key_id, sizeof(p.custody_key_id), "%s", c->publication_custody_key_id);
      snprintf(p.helper, sizeof(p.helper), "%s", c->publication_helper);
      snprintf(p.verifier_domain, sizeof(p.verifier_domain), "%s", c->publication_verifier_domain);
      memcpy(p.identity_digest, c->publication_identity_digest, 32);
      memcpy(p.hwm1_attestation, att, att_len);
      p.hwm1_attestation_len = att_len;
      dr = db->bind_publication(db->ctx, &p);
      if (dr != KB_MGMT_ROOT_DB_OK)
         rc = db_result(dr);
      else
         *mutated = 1;
   }
done:
   OPENSSL_cleanse(att, sizeof(att));
   OPENSSL_cleanse(&p, sizeof(p));
   return rc;
}

static kb_mgmt_roots_result_t roots_impl(const kb_mgmt_roots_config_t *c,
                                         const kb_mgmt_roots_db_t *db, int export_only,
                                         char *bundle, size_t cap, size_t *bundle_len)
{
   if (bundle && cap)
      OPENSSL_cleanse(bundle, cap);
   if (bundle_len)
      *bundle_len = 0;
   if (!bundle || !cap || !bundle_len || !config_valid(c, db))
      return KB_MGMT_ROOTS_INTEGRITY;
   kb_mgmt_root_record_t token, manifest;
   memset(&token, 0, sizeof(token));
   memset(&manifest, 0, sizeof(manifest));
   int mutated = 0, all_fresh = 1;
   kb_mgmt_roots_result_t rc;
   if (export_only)
   {
      if (db->inspect_root(db->ctx, KB_MGMT_ROOT_TOKEN, c->token_custody_key_id, &token) !=
              KB_MGMT_ROOT_DB_OK ||
          db->inspect_root(db->ctx, KB_MGMT_ROOT_MANIFEST, c->manifest_custody_key_id, &manifest) !=
              KB_MGMT_ROOT_DB_OK ||
          token.phase != KB_MGMT_ROOT_FINAL || manifest.phase != KB_MGMT_ROOT_FINAL ||
          !fixed_root(&token, KB_MGMT_ROOT_TOKEN, c->token_custody_key_id) ||
          !fixed_root(&manifest, KB_MGMT_ROOT_MANIFEST, c->manifest_custody_key_id))
      {
         rc = KB_MGMT_ROOTS_INTEGRITY;
         goto done;
      }
      uint64_t tv = 0, mv = 0;
      uint8_t att[512];
      size_t an = 0;
      rc = hwm_read(c->token_custody_key_id, &tv, att, &an);
      if (rc == KB_MGMT_ROOTS_FRESH)
         rc = hwm_read(c->manifest_custody_key_id, &mv, att, &an);
      OPENSSL_cleanse(att, sizeof(att));
      if (rc != KB_MGMT_ROOTS_FRESH || tv != 2 || mv != 2 ||
          protected_secret(&token, 0) != KB_MGMT_ROOTS_FRESH ||
          protected_secret(&manifest, 0) != KB_MGMT_ROOTS_FRESH)
      {
         rc = rc == KB_MGMT_ROOTS_FRESH ? KB_MGMT_ROOTS_INTEGRITY : rc;
         goto done;
      }
   }
   else
   {
      rc = root_step(db, KB_MGMT_ROOT_TOKEN, c->token_custody_key_id, &token, &mutated, &all_fresh);
      if (rc != KB_MGMT_ROOTS_FRESH)
         goto done;
      rc = root_step(db, KB_MGMT_ROOT_MANIFEST, c->manifest_custody_key_id, &manifest, &mutated,
                     &all_fresh);
      if (rc != KB_MGMT_ROOTS_FRESH)
         goto done;
   }
   rc = publication_step(c, db, &mutated, &all_fresh, export_only);
   if (rc != KB_MGMT_ROOTS_FRESH)
      goto done;
   if (kb_mgmt_public_bundle(token.public_key, token.public_key_len, manifest.public_key,
                             c->publication_identity_digest, bundle, cap, bundle_len))
   {
      rc = KB_MGMT_ROOTS_INTEGRITY;
      goto done;
   }
   rc = export_only ? KB_MGMT_ROOTS_FINAL
        : all_fresh ? KB_MGMT_ROOTS_FRESH
        : mutated   ? KB_MGMT_ROOTS_RECOVERED
                    : KB_MGMT_ROOTS_FINAL;
done:
   if ((!export_only && rc != KB_MGMT_ROOTS_FRESH) || (export_only && rc != KB_MGMT_ROOTS_FINAL))
   {
      OPENSSL_cleanse(bundle, cap);
      *bundle_len = 0;
   }
   OPENSSL_cleanse(&token, sizeof(token));
   OPENSSL_cleanse(&manifest, sizeof(manifest));
   return rc;
}

static kb_mgmt_roots_result_t roots_entry(const kb_mgmt_roots_config_t *c,
                                          const kb_mgmt_roots_db_t *db, int export_only,
                                          char *bundle, size_t cap, size_t *bundle_len)
{
   if (bundle && cap)
      OPENSSL_cleanse(bundle, cap);
   if (bundle_len)
      *bundle_len = 0;
   int prior = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior))
      return KB_MGMT_ROOTS_RETRY;
   kb_mgmt_roots_result_t rc = roots_impl(c, db, export_only, bundle, cap, bundle_len);
   (void)pthread_setcancelstate(prior, NULL);
   if (prior == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return rc;
}

kb_mgmt_roots_result_t kb_mgmt_token_roots_provision(const kb_mgmt_roots_config_t *c,
                                                     const kb_mgmt_roots_db_t *db, char *bundle,
                                                     size_t cap, size_t *bundle_len)
{
   return roots_entry(c, db, 0, bundle, cap, bundle_len);
}

kb_mgmt_roots_result_t kb_mgmt_token_roots_export(const kb_mgmt_roots_config_t *c,
                                                  const kb_mgmt_roots_db_t *db, char *bundle,
                                                  size_t cap, size_t *bundle_len)
{
   return roots_entry(c, db, 1, bundle, cap, bundle_len);
}
