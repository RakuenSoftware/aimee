#include "server_mgmt_jwks_cache.h"
#include "mgmt_jwks_cache.h"

#include "cJSON.h"
#include "modules/db1/db1_internal.h"

#include <math.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAFE_JSON_INT INT64_C(9007199254740991)

typedef struct
{
   char token_kid[65];
   char token_jwk[768];
   unsigned char manifest_public[32];
   char manifest_id[65];
   unsigned char bundle_digest[32];
} trust_t;

static int trust_parse(const char *bundle, size_t bundle_n, trust_t *out);

int server_mgmt_jwks_trust_bundle_load(const char *absolute_path, char *out, size_t cap,
                                       size_t *out_len)
{
   if (out && cap)
      memset(out, 0, cap);
   if (out_len)
      *out_len = 0;
   if (!absolute_path || absolute_path[0] != '/' || !out || !out_len ||
       cap < SERVER_MGMT_JWKS_BUNDLE_MAX)
      return -1;
   int fd = open(absolute_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 ||
       (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size < 1 ||
       st.st_size >= SERVER_MGMT_JWKS_BUNDLE_MAX)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   size_t used = 0;
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, out + used, (size_t)st.st_size - used);
      if (n <= 0)
      {
         close(fd);
         memset(out, 0, cap);
         return -1;
      }
      used += (size_t)n;
   }
   char extra;
   ssize_t trailing = read(fd, &extra, 1);
   close(fd);
   if (trailing != 0 || memchr(out, '\0', used))
   {
      memset(out, 0, cap);
      return -1;
   }
   /* Both offline export binaries use a terminal newline as stdout framing.
    * A direct, documented `... --export-public > bundle.json` must therefore
    * load as the canonical JSON bytes rather than fail because the framing byte
    * was mistaken for signed content. Accept exactly one LF (or CRLF), while an
    * extra newline/internal whitespace still fails the byte-exact parser. */
   if (used && out[used - 1] == '\n')
   {
      used--;
      if (used && out[used - 1] == '\r')
         used--;
   }
   out[used] = '\0';
   trust_t trust;
   if (trust_parse(out, used, &trust))
   {
      memset(out, 0, cap);
      return -1;
   }
   OPENSSL_cleanse(&trust, sizeof(trust));
   *out_len = used;
   return 0;
}

static int sha256(const void *value, size_t len, unsigned char out[32])
{
   unsigned int n = 0;
   if ((!value && len) || EVP_Digest(value, len, out, &n, EVP_sha256(), NULL) != 1 || n != 32)
   {
      OPENSSL_cleanse(out, 32);
      return -1;
   }
   return 0;
}

static void hex_encode(const unsigned char *in, size_t n, char *out)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < n; ++i)
   {
      out[i * 2] = hex[in[i] >> 4];
      out[i * 2 + 1] = hex[in[i] & 15];
   }
   out[n * 2] = '\0';
}

static int lower_hex(const char *s, size_t n)
{
   if (!s || strlen(s) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
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

static int b64_decode(const char *in, size_t n, unsigned char *out, size_t cap, size_t *out_n)
{
   if (!in || !n || !out || !out_n || n % 4 == 1)
      return -1;
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t used = 0;
   for (size_t i = 0; i < n; ++i)
   {
      int v = b64_value((unsigned char)in[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (used == cap)
            return -1;
         out[used++] = (unsigned char)(acc >> bits);
         acc &= bits ? ((UINT32_C(1) << bits) - 1) : 0;
      }
   }
   if (acc)
      return -1;
   *out_n = used;
   return 0;
}

static int no_duplicates(const cJSON *item)
{
   if (!item)
      return 1;
   if (cJSON_IsObject(item))
      for (const cJSON *a = item->child; a; a = a->next)
         for (const cJSON *b = a->next; b; b = b->next)
            if (!a->string || !b->string || strcmp(a->string, b->string) == 0)
               return 0;
   for (const cJSON *p = item->child; p; p = p->next)
      if (!no_duplicates(p))
         return 0;
   return 1;
}

static int exact_object(const cJSON *object, const char *const *names, size_t count,
                        const cJSON **values)
{
   if (!cJSON_IsObject(object))
      return 0;
   memset(values, 0, count * sizeof(*values));
   size_t seen = 0;
   for (const cJSON *p = object->child; p; p = p->next)
   {
      size_t i = 0;
      while (i < count && (!p->string || strcmp(p->string, names[i])))
         ++i;
      if (i == count || values[i])
         return 0;
      values[i] = p;
      ++seen;
   }
   return seen == count;
}

static cJSON *parse_exact(const char *raw, size_t n)
{
   if (!raw || !n || raw[0] != '{' || raw[n - 1] != '}' || memchr(raw, '\0', n))
      return NULL;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(raw, n + 1, &end, 1);
   if (!cJSON_IsObject(root) || end != raw + n || !no_duplicates(root))
   {
      cJSON_Delete(root);
      return NULL;
   }
   return root;
}

static void put_u32be(unsigned char out[4], uint32_t v)
{
   out[0] = (unsigned char)(v >> 24);
   out[1] = (unsigned char)(v >> 16);
   out[2] = (unsigned char)(v >> 8);
   out[3] = (unsigned char)v;
}

static int derive_kid(const unsigned char modulus[384], char out[65])
{
   static const char domain[] = "aimee.p5.token.public.v1\n";
   static const unsigned char exponent[] = {1, 0, 1};
   unsigned char ml[4], el[4], digest[32];
   put_u32be(ml, 384);
   put_u32be(el, 3);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(md, ml, sizeof(ml)) == 1 && EVP_DigestUpdate(md, modulus, 384) == 1 &&
            EVP_DigestUpdate(md, el, sizeof(el)) == 1 &&
            EVP_DigestUpdate(md, exponent, sizeof(exponent)) == 1 &&
            EVP_DigestFinal_ex(md, digest, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   if (!ok)
      return -1;
   memcpy(out, "p5-token-v1-", 12);
   hex_encode(digest, 16, out + 12);
   return 0;
}

static int validate_jwk(const cJSON *jwk, char *canonical, size_t cap, char kid[65])
{
   static const char *const names[] = {"kty", "kid", "use", "alg", "n", "e"};
   const cJSON *v[6];
   if (!exact_object(jwk, names, 6, v))
      return -1;
   for (size_t i = 0; i < 6; ++i)
      if (!cJSON_IsString(v[i]) || !v[i]->valuestring)
         return -1;
   if (strcmp(v[0]->valuestring, "RSA") || strcmp(v[2]->valuestring, "sig") ||
       strcmp(v[3]->valuestring, "RS256") || strcmp(v[5]->valuestring, "AQAB") ||
       strlen(v[1]->valuestring) > 64)
      return -1;
   unsigned char modulus[384];
   size_t modulus_n = 0;
   if (b64_decode(v[4]->valuestring, strlen(v[4]->valuestring), modulus, sizeof(modulus),
                  &modulus_n) ||
       modulus_n != sizeof(modulus) || !modulus[0] || derive_kid(modulus, kid) ||
       strcmp(kid, v[1]->valuestring))
      return -1;
   int n = snprintf(canonical, cap,
                    "{\"kty\":\"RSA\",\"kid\":\"%s\",\"use\":\"sig\","
                    "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"AQAB\"}",
                    kid, v[4]->valuestring);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int trust_parse(const char *bundle, size_t bundle_n, trust_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!bundle || !bundle_n || bundle_n >= SERVER_MGMT_JWKS_BUNDLE_MAX)
      return -1;
   cJSON *root = parse_exact(bundle, bundle_n);
   static const char *const names[] = {"format_version",      "token_kid",
                                       "token_jwk",           "manifest_wire_id",
                                       "manifest_public_key", "publication_hwm_identity_digest",
                                       "bundle_sha256"};
   const cJSON *v[7];
   char jwk[768], kid[65], manifest_id[65], manifest_b64[64], publication[65];
   unsigned char digest[32];
   char digest_hex[65], preimage[SERVER_MGMT_JWKS_BUNDLE_MAX],
       expected[SERVER_MGMT_JWKS_BUNDLE_MAX];
   size_t manifest_n = 0;
   int ok = root && exact_object(root, names, 7, v) && cJSON_IsNumber(v[0]) &&
            v[0]->valuedouble == 1 && cJSON_IsString(v[1]) && cJSON_IsObject(v[2]) &&
            cJSON_IsString(v[3]) && cJSON_IsString(v[4]) && cJSON_IsString(v[5]) &&
            cJSON_IsString(v[6]) && validate_jwk(v[2], jwk, sizeof(jwk), kid) == 0 &&
            strcmp(kid, v[1]->valuestring) == 0 && strlen(v[4]->valuestring) == 43 &&
            b64_decode(v[4]->valuestring, 43, out->manifest_public, sizeof(out->manifest_public),
                       &manifest_n) == 0 &&
            manifest_n == 32 && lower_hex(v[5]->valuestring, 64) &&
            lower_hex(v[6]->valuestring, 64);
   if (!ok)
      goto done;
   unsigned char manifest_digest[32];
   if (sha256(out->manifest_public, 32, manifest_digest))
      goto done;
   memcpy(manifest_id, "p5-jwks-root-v1-", 16);
   hex_encode(manifest_digest, 16, manifest_id + 16);
   if (strcmp(manifest_id, v[3]->valuestring))
      goto done;
   snprintf(manifest_b64, sizeof(manifest_b64), "%s", v[4]->valuestring);
   snprintf(publication, sizeof(publication), "%s", v[5]->valuestring);
   int pn = snprintf(preimage, sizeof(preimage),
                     "{\"format_version\":1,\"token_kid\":\"%s\",\"token_jwk\":%s,"
                     "\"manifest_wire_id\":\"%s\",\"manifest_public_key\":\"%s\","
                     "\"publication_hwm_identity_digest\":\"%s\"}",
                     kid, jwk, manifest_id, manifest_b64, publication);
   if (pn < 1 || (size_t)pn >= sizeof(preimage) || sha256(preimage, (size_t)pn, digest))
      goto done;
   hex_encode(digest, 32, digest_hex);
   int en = snprintf(expected, sizeof(expected), "%.*s,\"bundle_sha256\":\"%s\"}", pn - 1, preimage,
                     digest_hex);
   if (en < 1 || (size_t)en != bundle_n || CRYPTO_memcmp(expected, bundle, bundle_n) ||
       sha256(bundle, bundle_n, out->bundle_digest))
      goto done;
   snprintf(out->token_kid, sizeof(out->token_kid), "%s", kid);
   snprintf(out->token_jwk, sizeof(out->token_jwk), "%s", jwk);
   snprintf(out->manifest_id, sizeof(out->manifest_id), "%s", manifest_id);
   ok = 1;
done:
   cJSON_Delete(root);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok ? 0 : -1;
}

static int safe_integer(const cJSON *v, int64_t *out)
{
   if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
       v->valuedouble > (double)SAFE_JSON_INT || floor(v->valuedouble) != v->valuedouble)
      return -1;
   *out = (int64_t)v->valuedouble;
   return 0;
}

static int digest_part(EVP_MD_CTX *md, const void *value, size_t n)
{
   unsigned char size[4];
   if (n > UINT32_MAX)
      return -1;
   put_u32be(size, (uint32_t)n);
   return EVP_DigestUpdate(md, size, 4) == 1 && (!n || EVP_DigestUpdate(md, value, n) == 1) ? 0
                                                                                            : -1;
}

static int manifest_digest(const char *payload, size_t payload_n, const char *manifest_id,
                           const unsigned char signature[64], unsigned char out[32])
{
   static const char domain[] = "aimee.p5.jwks.manifest.v1";
   static const char alg[] = "EdDSA";
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            !digest_part(md, domain, sizeof(domain) - 1) && !digest_part(md, payload, payload_n) &&
            !digest_part(md, manifest_id, strlen(manifest_id)) &&
            !digest_part(md, alg, sizeof(alg) - 1) && !digest_part(md, signature, 64) &&
            EVP_DigestFinal_ex(md, out, &n) == 1 && n == 32;
   EVP_MD_CTX_free(md);
   return ok ? 0 : -1;
}

server_mgmt_jwks_cache_result_t
server_mgmt_jwks_envelope_validate(const char *trust_bundle, size_t trust_bundle_len,
                                   const char *envelope, size_t envelope_len, int64_t now,
                                   server_mgmt_jwks_cache_record_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || now < 0 || !envelope || !envelope_len ||
       envelope_len >= SERVER_MGMT_JWKS_ENVELOPE_MAX)
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   trust_t trust;
   if (trust_parse(trust_bundle, trust_bundle_len, &trust))
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   cJSON *root = parse_exact(envelope, envelope_len);
   static const char *const outer_names[] = {"payload", "manifest_kid", "signature_alg",
                                             "signature", "manifest_sha256"};
   static const char *const payload_names[] = {
       "format_version",           "generation", "valid_from", "valid_until",
       "previous_manifest_sha256", "keys",       "jwks_sha256"};
   const cJSON *ov[5], *pv[7];
   int64_t format = 0, generation = 0, valid_from = 0, valid_until = 0;
   char *jwk = NULL;
   unsigned char signature[64], computed_manifest[32], computed_jwks[32];
   size_t signature_n = 0;
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX], payload[2048], expected[SERVER_MGMT_JWKS_ENVELOPE_MAX];
   char jwks_hex[65], manifest_hex[65];
   int ok = root && exact_object(root, outer_names, 5, ov) && cJSON_IsObject(ov[0]) &&
            cJSON_IsString(ov[1]) && cJSON_IsString(ov[2]) && cJSON_IsString(ov[3]) &&
            cJSON_IsString(ov[4]) && exact_object(ov[0], payload_names, 7, pv) &&
            !safe_integer(pv[0], &format) && !safe_integer(pv[1], &generation) &&
            !safe_integer(pv[2], &valid_from) && !safe_integer(pv[3], &valid_until) &&
            cJSON_IsString(pv[4]) && cJSON_IsArray(pv[5]) && cJSON_GetArraySize(pv[5]) == 1 &&
            cJSON_IsString(pv[6]) && format == 1 && generation == 1 && valid_until > valid_from &&
            strcmp(pv[4]->valuestring,
                   "0000000000000000000000000000000000000000000000000000000000000000") == 0 &&
            strcmp(ov[1]->valuestring, trust.manifest_id) == 0 &&
            strcmp(ov[2]->valuestring, "EdDSA") == 0 && strlen(ov[3]->valuestring) == 86 &&
            b64_decode(ov[3]->valuestring, 86, signature, sizeof(signature), &signature_n) == 0 &&
            signature_n == 64 && lower_hex(ov[4]->valuestring, 64) &&
            lower_hex(pv[6]->valuestring, 64);
   if (!ok)
      goto done;
   jwk = cJSON_PrintUnformatted(pv[5]->child);
   if (!jwk || strcmp(jwk, trust.token_jwk))
      goto done;
   int jn = snprintf(jwks, sizeof(jwks), "{\"keys\":[%s]}", jwk);
   if (jn < 1 || (size_t)jn >= sizeof(jwks) || sha256(jwks, (size_t)jn, computed_jwks))
      goto done;
   hex_encode(computed_jwks, 32, jwks_hex);
   if (strcmp(jwks_hex, pv[6]->valuestring))
      goto done;
   int pn =
       snprintf(payload, sizeof(payload),
                "{\"format_version\":1,\"generation\":1,\"valid_from\":%lld,"
                "\"valid_until\":%lld,\"previous_manifest_sha256\":\"%s\","
                "\"keys\":[%s],\"jwks_sha256\":\"%s\"}",
                (long long)valid_from, (long long)valid_until, pv[4]->valuestring, jwk, jwks_hex);
   if (pn < 1 || (size_t)pn >= sizeof(payload))
      goto done;
   EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, trust.manifest_public, 32);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   int verified =
       key && md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) == 1 &&
       EVP_DigestVerify(md, signature, 64, (const unsigned char *)payload, (size_t)pn) == 1;
   EVP_MD_CTX_free(md);
   EVP_PKEY_free(key);
   if (!verified ||
       manifest_digest(payload, (size_t)pn, trust.manifest_id, signature, computed_manifest))
      goto done;
   hex_encode(computed_manifest, 32, manifest_hex);
   if (strcmp(manifest_hex, ov[4]->valuestring))
      goto done;
   int en = snprintf(expected, sizeof(expected),
                     "{\"payload\":%s,\"manifest_kid\":\"%s\","
                     "\"signature_alg\":\"EdDSA\",\"signature\":\"%s\","
                     "\"manifest_sha256\":\"%s\"}",
                     payload, trust.manifest_id, ov[3]->valuestring, manifest_hex);
   if (en < 1 || (size_t)en != envelope_len || CRYPTO_memcmp(expected, envelope, envelope_len) ||
       sha256(envelope, envelope_len, out->envelope_sha256))
      goto done;
   out->generation = 1;
   out->valid_from = valid_from;
   out->valid_until = valid_until;
   memcpy(out->jwks, jwks, (size_t)jn + 1);
   out->jwks_len = (size_t)jn;
   memcpy(out->manifest_sha256, computed_manifest, 32);
   memcpy(out->trust_bundle_sha256, trust.bundle_digest, 32);
   ok = 1;
done:
   free(jwk);
   cJSON_Delete(root);
   OPENSSL_cleanse(&trust, sizeof(trust));
   OPENSSL_cleanse(signature, sizeof(signature));
   if (!ok)
   {
      memset(out, 0, sizeof(*out));
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   }
   if (now < valid_from || now >= valid_until)
   {
      memset(out, 0, sizeof(*out));
      return SERVER_MGMT_JWKS_CACHE_STALE;
   }
   return SERVER_MGMT_JWKS_CACHE_OK;
}

/* Bytes to lowercase hex, for the three digests and the JWKS the store keeps
   as blobs. The store speaks hex because the wire has no bytes; see
   db1/mgmt_jwks_cache.h. */
/* Exactly 64 lowercase hex characters back to 32 bytes, or refuse. A digest
   that does not round-trip is not one to compare against. */
static int jwks_unhex(const char *hex, unsigned char *out)
{
   if (!hex || strlen(hex) != 64)
      return -1;
   for (int i = 0; i < 32; i++)
   {
      int hi = hex[i * 2], lo = hex[i * 2 + 1];
      hi = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
      lo = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
      if (hi < 0 || lo < 0)
         return -1;
      out[i] = (unsigned char)((hi << 4) | lo);
   }
   return 0;
}

static void jwks_hex(const unsigned char *bytes, size_t len, char *out)
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < len; i++)
   {
      out[i * 2] = digits[bytes[i] >> 4];
      out[i * 2 + 1] = digits[bytes[i] & 0x0f];
   }
   out[len * 2] = '\0';
}

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_install(const char *trust_bundle,
                                                               size_t trust_bundle_len,
                                                               const char *envelope,
                                                               size_t envelope_len, int64_t now)
{
   server_mgmt_jwks_cache_record_t record;
   server_mgmt_jwks_cache_result_t valid = server_mgmt_jwks_envelope_validate(
       trust_bundle, trust_bundle_len, envelope, envelope_len, now, &record);
   if (valid != SERVER_MGMT_JWKS_CACHE_OK)
      return valid;
   if (envelope_len >= DB1_MGMT_JWKS_ENVELOPE_MAX || record.jwks_len > DB1_MGMT_JWKS_BYTES_MAX)
      return SERVER_MGMT_JWKS_CACHE_INVALID;

   db1_mgmt_jwks_install_t in;
   memset(&in, 0, sizeof in);
   in.valid_from = record.valid_from;
   in.valid_until = record.valid_until;
   in.fetched_at = now;
   jwks_hex((const unsigned char *)record.jwks, record.jwks_len, in.jwks);
   memcpy(in.envelope, envelope, envelope_len);
   in.envelope[envelope_len] = '\0';
   jwks_hex(record.envelope_sha256, 32, in.envelope_sha256);
   jwks_hex(record.manifest_sha256, 32, in.manifest_sha256);
   jwks_hex(record.trust_bundle_sha256, 32, in.trust_bundle_sha256);

   switch (db1_mgmt_jwks_install(&in))
   {
   case 0:
      return SERVER_MGMT_JWKS_CACHE_OK;
   case 1:
      return SERVER_MGMT_JWKS_CACHE_CONFLICT;
   default:
      return SERVER_MGMT_JWKS_CACHE_STORAGE;
   }
}

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_load(const char *trust_bundle,
                                                            size_t trust_bundle_len, int64_t now,
                                                            char *jwks_out, size_t jwks_cap,
                                                            size_t *jwks_len)
{
   if (jwks_out && jwks_cap)
      memset(jwks_out, 0, jwks_cap);
   if (jwks_len)
      *jwks_len = 0;
   if (!jwks_out || !jwks_cap || !jwks_len)
      return SERVER_MGMT_JWKS_CACHE_INVALID;

   db1_mgmt_jwks_row_t row;
   int found = db1_mgmt_jwks_read(&row);
   if (found < 0)
      return SERVER_MGMT_JWKS_CACHE_STORAGE;
   if (found == 0)
      return SERVER_MGMT_JWKS_CACHE_MISSING;

   /* The digests come back as hex and go straight back to bytes, so the
      comparison below is the same CRYPTO_memcmp over the same 32 bytes it
      always was. Only the spelling in transit changed. */
   unsigned char digests[3][32];
   if (jwks_unhex(row.envelope_sha256, digests[0]) != 0 ||
       jwks_unhex(row.manifest_sha256, digests[1]) != 0 ||
       jwks_unhex(row.trust_bundle_sha256, digests[2]) != 0)
      return SERVER_MGMT_JWKS_CACHE_INVALID;

   size_t envelope_len = strlen(row.envelope);
   if (envelope_len < 1)
      return SERVER_MGMT_JWKS_CACHE_INVALID;

   server_mgmt_jwks_cache_record_t record;
   server_mgmt_jwks_cache_result_t result = server_mgmt_jwks_envelope_validate(
       trust_bundle, trust_bundle_len, row.envelope, envelope_len, now, &record);
   if (result != SERVER_MGMT_JWKS_CACHE_OK || record.valid_from != row.valid_from ||
       record.valid_until != row.valid_until ||
       CRYPTO_memcmp(record.envelope_sha256, digests[0], 32) ||
       CRYPTO_memcmp(record.manifest_sha256, digests[1], 32) ||
       CRYPTO_memcmp(record.trust_bundle_sha256, digests[2], 32) || record.jwks_len + 1 > jwks_cap)
      return result == SERVER_MGMT_JWKS_CACHE_STALE ? result : SERVER_MGMT_JWKS_CACHE_INVALID;
   memcpy(jwks_out, record.jwks, record.jwks_len + 1);
   *jwks_len = record.jwks_len;
   return SERVER_MGMT_JWKS_CACHE_OK;
}

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_current_generation(const char *trust_bundle,
                                                                          size_t trust_bundle_len,
                                                                          int64_t now,
                                                                          int64_t *generation)
{
   if (generation)
      *generation = 0;
   if (!generation)
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   /* The load first, and its result respected: the generation of a row whose
      envelope no longer verifies is not a generation anybody should act on. */
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_len = 0;
   server_mgmt_jwks_cache_result_t result = server_mgmt_jwks_cache_load(
       trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks), &jwks_len);
   OPENSSL_cleanse(jwks, sizeof(jwks));
   if (result != SERVER_MGMT_JWKS_CACHE_OK)
      return result;

   int64_t found = 0;
   int present = db1_mgmt_jwks_generation(&found);
   if (present < 0)
      return SERVER_MGMT_JWKS_CACHE_STORAGE;
   if (present == 0)
      return SERVER_MGMT_JWKS_CACHE_MISSING;
   /* A generation below one is a row that exists but says nothing, which the
      caller must not read as "generation zero is current". */
   if (found < 1)
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   *generation = found;
   return SERVER_MGMT_JWKS_CACHE_OK;
}

static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t refresh_condition = PTHREAD_COND_INITIALIZER;
static int refresh_active;

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_refresh(const char *trust_bundle,
                                                               size_t trust_bundle_len, int64_t now,
                                                               server_mgmt_jwks_fetch_fn fetch,
                                                               void *fetch_ctx)
{
   if (!fetch)
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   pthread_mutex_lock(&refresh_lock);
   if (refresh_active)
   {
      while (refresh_active)
         pthread_cond_wait(&refresh_condition, &refresh_lock);
      pthread_mutex_unlock(&refresh_lock);
      char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
      size_t jwks_n = 0;
      return server_mgmt_jwks_cache_load(trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks),
                                         &jwks_n);
   }
   refresh_active = 1;
   pthread_mutex_unlock(&refresh_lock);

   char envelope[SERVER_MGMT_JWKS_ENVELOPE_MAX] = {0};
   size_t envelope_n = 0;
   server_mgmt_jwks_cache_result_t result = SERVER_MGMT_JWKS_CACHE_STORAGE;
   if (fetch(fetch_ctx, envelope, sizeof(envelope), &envelope_n) == 0 && envelope_n > 0 &&
       envelope_n < sizeof(envelope) && envelope[envelope_n] == '\0')
      result =
          server_mgmt_jwks_cache_install(trust_bundle, trust_bundle_len, envelope, envelope_n, now);
   OPENSSL_cleanse(envelope, sizeof(envelope));

   pthread_mutex_lock(&refresh_lock);
   refresh_active = 0;
   pthread_cond_broadcast(&refresh_condition);
   pthread_mutex_unlock(&refresh_lock);
   return result;
}

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_startup(const char *trust_bundle_path,
                                                               int64_t now,
                                                               server_mgmt_jwks_fetch_fn fetch,
                                                               void *fetch_ctx)
{
   char trust[SERVER_MGMT_JWKS_BUNDLE_MAX] = {0};
   size_t trust_n = 0;
   if (server_mgmt_jwks_trust_bundle_load(trust_bundle_path, trust, sizeof(trust), &trust_n) != 0)
      return SERVER_MGMT_JWKS_CACHE_INVALID;

   server_mgmt_jwks_cache_result_t result =
       server_mgmt_jwks_cache_refresh(trust, trust_n, now, fetch, fetch_ctx);
   if (result != SERVER_MGMT_JWKS_CACHE_OK)
   {
      char jwks[SERVER_MGMT_JWKS_BYTES_MAX] = {0};
      size_t jwks_n = 0;
      result = server_mgmt_jwks_cache_load(trust, trust_n, now, jwks, sizeof(jwks), &jwks_n);
      OPENSSL_cleanse(jwks, sizeof(jwks));
   }
   OPENSSL_cleanse(trust, sizeof(trust));
   return result;
}

server_mgmt_token_result_t server_mgmt_token_verify_cached(
    const char *jwt, size_t jwt_len, const char *trust_bundle, size_t trust_bundle_len,
    const char *expected_issuer, const char *expected_audience, const char *peer_issuer,
    const char *peer_serial, const char *peer_fingerprint, const char *request_sha256, int64_t now,
    server_mgmt_jwks_fetch_fn fetch, void *fetch_ctx, server_mgmt_token_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out)
      return SERVER_MGMT_TOKEN_INVALID;
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_n = 0;
   int refreshed = 0;
   server_mgmt_jwks_cache_result_t loaded = server_mgmt_jwks_cache_load(
       trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks), &jwks_n);
   if (loaded != SERVER_MGMT_JWKS_CACHE_OK)
   {
      refreshed = 1;
      if (server_mgmt_jwks_cache_refresh(trust_bundle, trust_bundle_len, now, fetch, fetch_ctx) !=
              SERVER_MGMT_JWKS_CACHE_OK ||
          server_mgmt_jwks_cache_load(trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks),
                                      &jwks_n) != SERVER_MGMT_JWKS_CACHE_OK)
         return SERVER_MGMT_TOKEN_INVALID;
   }
   server_mgmt_token_result_t result = server_mgmt_token_verify_ex(
       jwt, jwt_len, jwks, expected_issuer, expected_audience, peer_issuer, peer_serial,
       peer_fingerprint, request_sha256, now, out);
   if (result != SERVER_MGMT_TOKEN_UNKNOWN_KID || refreshed)
      return result;
   if (server_mgmt_jwks_cache_refresh(trust_bundle, trust_bundle_len, now, fetch, fetch_ctx) !=
           SERVER_MGMT_JWKS_CACHE_OK ||
       server_mgmt_jwks_cache_load(trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks),
                                   &jwks_n) != SERVER_MGMT_JWKS_CACHE_OK)
      return SERVER_MGMT_TOKEN_INVALID;
   return server_mgmt_token_verify_ex(jwt, jwt_len, jwks, expected_issuer, expected_audience,
                                      peer_issuer, peer_serial, peer_fingerprint, request_sha256,
                                      now, out);
}

server_mgmt_token_result_t server_mgmt_token_verify_read_claims_cached(
    const char *jwt, size_t jwt_len, const char *trust_bundle, size_t trust_bundle_len,
    const char *expected_issuer, const char *expected_audience, const char *peer_issuer,
    const char *peer_serial, const char *peer_fingerprint, int64_t now,
    server_mgmt_jwks_fetch_fn fetch, void *fetch_ctx, server_mgmt_token_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out)
      return SERVER_MGMT_TOKEN_INVALID;
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_n = 0;
   int refreshed = 0;
   server_mgmt_jwks_cache_result_t loaded = server_mgmt_jwks_cache_load(
       trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks), &jwks_n);
   if (loaded != SERVER_MGMT_JWKS_CACHE_OK)
   {
      refreshed = 1;
      if (server_mgmt_jwks_cache_refresh(trust_bundle, trust_bundle_len, now, fetch, fetch_ctx) !=
              SERVER_MGMT_JWKS_CACHE_OK ||
          server_mgmt_jwks_cache_load(trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks),
                                      &jwks_n) != SERVER_MGMT_JWKS_CACHE_OK)
         return SERVER_MGMT_TOKEN_INVALID;
   }
   server_mgmt_token_result_t result = server_mgmt_token_verify_read_claims_ex(
       jwt, jwt_len, jwks, expected_issuer, expected_audience, peer_issuer, peer_serial,
       peer_fingerprint, now, out);
   if (result != SERVER_MGMT_TOKEN_UNKNOWN_KID || refreshed)
      return result;
   if (server_mgmt_jwks_cache_refresh(trust_bundle, trust_bundle_len, now, fetch, fetch_ctx) !=
           SERVER_MGMT_JWKS_CACHE_OK ||
       server_mgmt_jwks_cache_load(trust_bundle, trust_bundle_len, now, jwks, sizeof(jwks),
                                   &jwks_n) != SERVER_MGMT_JWKS_CACHE_OK)
      return SERVER_MGMT_TOKEN_INVALID;
   return server_mgmt_token_verify_read_claims_ex(jwt, jwt_len, jwks, expected_issuer,
                                                  expected_audience, peer_issuer, peer_serial,
                                                  peer_fingerprint, now, out);
}
