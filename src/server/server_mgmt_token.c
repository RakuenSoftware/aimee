#include "server_mgmt_token.h"

#include "server_identity_token.h"

#include "cJSON.h"

#include <limits.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_WIRE_MAX    8192u
#define TOKEN_HEADER_MAX  1024u
#define TOKEN_PAYLOAD_MAX 4096u
#define TOKEN_SIG_MAX     1024u
#define JWKS_MAX          65536u
#define JSON_INT_MAX      INT64_C(9007199254740991)

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
      return 0;
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t used = 0;
   for (size_t i = 0; i < n; ++i)
   {
      int v = b64_value((unsigned char)in[i]);
      if (v < 0)
         return 0;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (used == cap)
            return 0;
         out[used++] = (unsigned char)(acc >> bits);
         acc &= bits ? ((UINT32_C(1) << bits) - 1) : 0;
      }
   }
   if (acc != 0)
      return 0; /* non-zero unused bits are a non-canonical spelling */
   *out_n = used;
   return 1;
}

static int b64_canonical(const char *encoded, size_t encoded_n, const unsigned char *raw,
                         size_t raw_n)
{
   static const char alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   size_t expected = (raw_n * 8 + 5) / 6;
   if (expected != encoded_n)
      return 0;
   for (size_t i = 0, o = 0; i < raw_n; i += 3)
   {
      uint32_t v = (uint32_t)raw[i] << 16;
      size_t left = raw_n - i;
      if (left > 1)
         v |= (uint32_t)raw[i + 1] << 8;
      if (left > 2)
         v |= raw[i + 2];
      unsigned chars = left == 1 ? 2 : (left == 2 ? 3 : 4);
      for (unsigned j = 0; j < chars; ++j)
         if (encoded[o++] != alphabet[(v >> (18 - j * 6)) & 63])
            return 0;
   }
   return 1;
}

static int decode_segment(const char *s, size_t n, unsigned char *out, size_t cap, size_t *out_n)
{
   return b64_decode(s, n, out, cap, out_n) && b64_canonical(s, n, out, *out_n) &&
          !memchr(out, '\0', *out_n);
}

static int decoded_nul_escape(const unsigned char *raw, size_t n)
{
   int quoted = 0;
   for (size_t i = 0; i < n; ++i)
   {
      if (!quoted)
      {
         quoted = raw[i] == '"';
         continue;
      }
      if (raw[i] == '"')
      {
         quoted = 0;
         continue;
      }
      if (raw[i] != '\\' || i + 1 >= n)
         continue;
      if (raw[i + 1] == 'u' && i + 5 < n && raw[i + 2] == '0' && raw[i + 3] == '0' &&
          raw[i + 4] == '0' && raw[i + 5] == '0')
         return 1;
      ++i;
   }
   return 0;
}

static void json_cleanse(cJSON *item)
{
   for (cJSON *p = item; p; p = p->next)
   {
      if (p->child)
         json_cleanse(p->child);
      if (p->valuestring)
         OPENSSL_cleanse(p->valuestring, strlen(p->valuestring));
      if (p->string)
         OPENSSL_cleanse(p->string, strlen(p->string));
   }
}

static void json_delete(cJSON *item)
{
   json_cleanse(item);
   cJSON_Delete(item);
}

static cJSON *parse_json(const unsigned char *raw, size_t n)
{
   if (!raw || !n || raw[0] != '{' || raw[n - 1] != '}' || decoded_nul_escape(raw, n))
      return NULL;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts((const char *)raw, n + 1, &end, 1);
   if (!cJSON_IsObject(root) || end != (const char *)raw + n)
   {
      json_delete(root);
      return NULL;
   }
   return root;
}

static int no_duplicate_members(const cJSON *item)
{
   if (!item)
      return 1;
   if (cJSON_IsObject(item))
      for (const cJSON *a = item->child; a; a = a->next)
      {
         if (!a->string)
            return 0;
         for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && strcmp(a->string, b->string) == 0)
               return 0;
      }
   for (const cJSON *p = item->child; p; p = p->next)
      if (!no_duplicate_members(p))
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
      while (i < count && (!p->string || strcmp(p->string, names[i]) != 0))
         ++i;
      if (i == count || values[i])
         return 0;
      values[i] = p;
      ++seen;
   }
   return seen == count;
}

static size_t skip_space(const unsigned char *raw, size_t n, size_t i)
{
   while (i < n && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n'))
      ++i;
   return i;
}

/* cJSON stores numbers as doubles. Locate a named top-level member in the raw
 * object and require its original spelling to be canonical unsigned decimal. */
static int raw_uint(const unsigned char *raw, size_t n, const char *name, int64_t *out)
{
   size_t want = strlen(name);
   int depth = 0;
   unsigned matches = 0;
   int64_t result = 0;
   for (size_t i = 0; i < n; ++i)
   {
      /* Consume a complete JSON string, including escapes, before considering
       * structural bytes. Braces and brackets in claim text never affect depth. */
      if (raw[i] != '"')
      {
         if (raw[i] == '{' || raw[i] == '[')
            ++depth;
         else if (raw[i] == '}' || raw[i] == ']')
            --depth;
         continue;
      }
      size_t begin = ++i;
      int escaped = 0;
      while (i < n && (escaped || raw[i] != '"'))
      {
         if (escaped)
            escaped = 0;
         else if (raw[i] == '\\')
            escaped = 1;
         ++i;
      }
      if (i >= n)
         return 0;
      size_t after = skip_space(raw, n, i + 1);
      if (depth != 1 || after >= n || raw[after] != ':' || i - begin != want ||
          memcmp(raw + begin, name, want) != 0)
         continue;
      size_t p = skip_space(raw, n, after + 1);
      if (p >= n || raw[p] < '0' || raw[p] > '9' ||
          (raw[p] == '0' && p + 1 < n && raw[p + 1] >= '0' && raw[p + 1] <= '9'))
         return 0;
      int64_t v = 0;
      do
      {
         int digit = raw[p++] - '0';
         if (v > (JSON_INT_MAX - digit) / 10)
            return 0;
         v = v * 10 + digit;
      } while (p < n && raw[p] >= '0' && raw[p] <= '9');
      p = skip_space(raw, n, p);
      if (p >= n || (raw[p] != ',' && raw[p] != '}'))
         return 0;
      ++matches;
      result = v;
   }
   if (matches != 1)
      return 0;
   *out = result;
   return 1;
}

static int ascii_token(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

static int control_free(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strlen(s);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
         return 0;
   return 1;
}

static int lower_hex(const char *s, size_t exact, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if ((exact && n != exact) || (!exact && (n == 0 || n > max)))
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int identity_component(const char *s, size_t n)
{
   if (!n)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f || c == ':')
         return 0;
      if (c != '%')
         continue;
      if (i + 2 >= n ||
          !((s[i + 1] == '2' && s[i + 2] == '5') || (s[i + 1] == '3' && s[i + 2] == 'A')))
         return 0;
      i += 2;
   }
   return 1;
}

/* A bare host-account name: the PAM login's subject form, which
 * kb_identity_token.h has always documented the `sub` as ("OIDC (iss,sub)
 * composite or PAM username"). Bounds mirror the Linux 32-character limit.
 *
 * Unprefixed because a host account has exactly one authority and the two login
 * modes are mutually exclusive per kb, so there is nothing for a prefix to
 * disambiguate. */
static int bare_username(const char *s)
{
   size_t n = strlen(s);
   if (n == 0 || n > 32)
      return 0;
   unsigned char f = (unsigned char)s[0];
   if (!((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z') || (f >= '0' && f <= '9') || f == '_'))
      return 0;
   for (size_t i = 1; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

/* The prefixed identity-key forms: `owner`, `oidc:<iss>:<sub>`,
 * `cert:<issuer>:<serial>`. Used for the MANAGEMENT token's actor, which is an
 * admin or team lead out of kb_admin_grant / kb_team_lead and is never a bare
 * host account — widening this would widen the management actor too. */
static int identity_key(const char *s)
{
   if (!s || strnlen(s, 577) > 576)
      return 0;
   if (strcmp(s, "owner") == 0)
      return 1;
   int oidc = strncmp(s, "oidc:", 5) == 0;
   int cert = strncmp(s, "cert:", 5) == 0;
   if (!oidc && !cert)
      return 0;
   const char *first = s + 5;
   const char *sep = strchr(first, ':');
   if (!sep || strchr(sep + 1, ':') || !identity_component(first, (size_t)(sep - first)) ||
       !identity_component(sep + 1, strlen(sep + 1)))
      return 0;
   return oidc || lower_hex(sep + 1, 0, 79);
}

/* The DATA-PLANE identity token's subject: the prefixed forms above, plus a bare
 * host-account name for the PAM login — which kb_identity_token.h has always
 * documented the `sub` as ("OIDC (iss,sub) composite or PAM username").
 *
 * Deliberately separate from identity_key(): that one also validates the
 * management token's actor, which comes from kb_admin_grant / kb_team_lead and is
 * never a bare account, so sharing one function would widen the management
 * surface as a side effect of a data-plane change. It did, in the first version
 * of this commit — the management token suite caught it.
 *
 * This is the THIRD place the data-plane grammar is encoded; the others are the
 * subject CHECK in db2/schema.sql and db2_intent_canonical_actor in
 * db2/management_intent_fields.h. They cannot share code — the server is
 * deliberately free of DB2_OBJS and libpq (see the $(SERVER) rule and
 * scripts/check_tier_deps.sh) — but they must agree, and whichever is stricter
 * wins SILENTLY: a subject the database admits but this rejects mints a token the
 * server then refuses as malformed. Change all three together. */
int server_identity_subject_valid(const char *s)
{
   if (!s || strnlen(s, 577) > 576)
      return 0;
   if (strncmp(s, "oidc:", 5) == 0 || strncmp(s, "cert:", 5) == 0 || strcmp(s, "owner") == 0)
      return identity_key(s);
   return bare_username(s);
}

static int copy_string(const cJSON *item, char *out, size_t cap)
{
   if (!cJSON_IsString(item) || !item->valuestring)
      return 0;
   size_t n = strnlen(item->valuestring, cap);
   if (n >= cap)
      return 0;
   memcpy(out, item->valuestring, n + 1);
   return 1;
}

static int exact_secret(const char *actual, const char *expected, size_t n)
{
   return strlen(actual) == n && strlen(expected) == n && CRYPTO_memcmp(actual, expected, n) == 0;
}

static int parse_header(cJSON *header, char kid[65])
{
   static const char *const names[] = {"alg", "typ", "kid"};
   const cJSON *v[3];
   return no_duplicate_members(header) && exact_object(header, names, 3, v) &&
          cJSON_IsString(v[0]) && strcmp(v[0]->valuestring, "RS256") == 0 && cJSON_IsString(v[1]) &&
          strcmp(v[1]->valuestring, "JWT") == 0 && cJSON_IsString(v[2]) &&
          ascii_token(v[2]->valuestring, 1, 64) && copy_string(v[2], kid, 65);
}

static EVP_PKEY *rsa_from_jwk(const cJSON *key)
{
   const cJSON *nj = cJSON_GetObjectItemCaseSensitive(key, "n");
   const cJSON *ej = cJSON_GetObjectItemCaseSensitive(key, "e");
   if (!cJSON_IsString(nj) || !cJSON_IsString(ej))
      return NULL;
   size_t ns = strlen(nj->valuestring), es = strlen(ej->valuestring), nn = 0, en = 0;
   unsigned char nb[1024], eb[8];
   if (!b64_decode(nj->valuestring, ns, nb, sizeof(nb), &nn) ||
       !b64_canonical(nj->valuestring, ns, nb, nn) ||
       !b64_decode(ej->valuestring, es, eb, sizeof(eb), &en) ||
       !b64_canonical(ej->valuestring, es, eb, en) || nn < 256 || nn > 1024 || !en || nb[0] == 0 ||
       (en > 1 && eb[0] == 0))
      return NULL;
   BIGNUM *n = BN_bin2bn(nb, (int)nn, NULL);
   BIGNUM *e = BN_bin2bn(eb, (int)en, NULL);
   EVP_PKEY *result = NULL;
   OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
   if (!n || !e || BN_num_bits(n) < 2048 || BN_num_bits(n) > 8192 || !BN_is_odd(n) ||
       BN_num_bits(e) > 31 || BN_get_word(e) < 3 || !BN_is_odd(e))
      goto done;
   if (bld && ctx && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
       OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
   {
      OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
      if (params && EVP_PKEY_fromdata_init(ctx) > 0)
         EVP_PKEY_fromdata(ctx, &result, EVP_PKEY_PUBLIC_KEY, params);
      OSSL_PARAM_free(params);
   }
done:
   EVP_PKEY_CTX_free(ctx);
   OSSL_PARAM_BLD_free(bld);
   BN_free(n);
   BN_free(e);
   OPENSSL_cleanse(nb, sizeof(nb));
   OPENSSL_cleanse(eb, sizeof(eb));
   return result;
}

typedef enum
{
   KEY_SELECT_INVALID = 0,
   KEY_SELECT_OK = 1,
   KEY_SELECT_UNKNOWN = 2,
} key_select_result_t;

static key_select_result_t select_key(const char *jwks, size_t jwks_n, const char *kid,
                                      EVP_PKEY **out)
{
   *out = NULL;
   cJSON *root = parse_json((const unsigned char *)jwks, jwks_n);
   if (!root || !no_duplicate_members(root))
   {
      json_delete(root);
      return KEY_SELECT_INVALID;
   }
   static const char *const root_names[] = {"keys"};
   const cJSON *root_v[1];
   if (!exact_object(root, root_names, 1, root_v) || !cJSON_IsArray(root_v[0]) ||
       cJSON_GetArraySize(root_v[0]) < 1)
   {
      json_delete(root);
      return KEY_SELECT_INVALID;
   }
   static const char *const key_names[] = {"kty", "kid", "use", "alg", "n", "e"};
   const cJSON *chosen = NULL;
   for (const cJSON *key = root_v[0]->child; key; key = key->next)
   {
      const cJSON *v[6];
      if (!exact_object(key, key_names, 6, v))
      {
         json_delete(root);
         return KEY_SELECT_INVALID;
      }
      for (const cJSON *other = key->next; other; other = other->next)
      {
         const cJSON *other_kid = cJSON_GetObjectItemCaseSensitive(other, "kid");
         if (cJSON_IsString(v[1]) && cJSON_IsString(other_kid) &&
             strcmp(v[1]->valuestring, other_kid->valuestring) == 0)
         {
            json_delete(root);
            return KEY_SELECT_INVALID;
         }
      }
      if (!cJSON_IsString(v[0]) || strcmp(v[0]->valuestring, "RSA") != 0 || !cJSON_IsString(v[1]) ||
          !ascii_token(v[1]->valuestring, 1, 64) || !cJSON_IsString(v[2]) ||
          strcmp(v[2]->valuestring, "sig") != 0 || !cJSON_IsString(v[3]) ||
          strcmp(v[3]->valuestring, "RS256") != 0 || !cJSON_IsString(v[4]) || !cJSON_IsString(v[5]))
      {
         json_delete(root);
         return KEY_SELECT_INVALID;
      }
      EVP_PKEY *validated = rsa_from_jwk(key);
      if (!validated)
      {
         json_delete(root);
         return KEY_SELECT_INVALID;
      }
      if (strcmp(v[1]->valuestring, kid) == 0)
      {
         if (chosen)
         {
            EVP_PKEY_free(validated);
            json_delete(root);
            return KEY_SELECT_INVALID;
         }
         chosen = key;
         *out = validated;
      }
      else
         EVP_PKEY_free(validated);
   }
   json_delete(root);
   return chosen ? KEY_SELECT_OK : KEY_SELECT_UNKNOWN;
}

static int verify_signature(EVP_PKEY *key, const char *signed_bytes, size_t signed_n,
                            const unsigned char *signature, size_t signature_n)
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   EVP_PKEY_CTX *pk = NULL;
   int ok = md && EVP_DigestVerifyInit(md, &pk, EVP_sha256(), NULL, key) == 1 && pk &&
            EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1 &&
            EVP_PKEY_CTX_set_signature_md(pk, EVP_sha256()) == 1 &&
            EVP_DigestVerifyUpdate(md, signed_bytes, signed_n) == 1 &&
            EVP_DigestVerifyFinal(md, signature, signature_n) == 1;
   EVP_MD_CTX_free(md);
   return ok;
}

static int parse_payload(cJSON *payload, const unsigned char *raw, size_t raw_n, const char *issuer,
                         const char *audience, const char *peer_issuer, const char *peer_serial,
                         const char *peer_fingerprint, const char *request_sha256, int64_t now,
                         server_mgmt_token_claims_t *out)
{
   static const char *const names[] = {"v",
                                       "iss",
                                       "aud",
                                       "sub",
                                       "team_id",
                                       "cap",
                                       "jti",
                                       "correlation_id",
                                       "request_sha256",
                                       "peer_issuer",
                                       "peer_serial",
                                       "peer_fingerprint",
                                       "iat",
                                       "exp"};
   const cJSON *v[14];
   int64_t version = 0, team = 0, issued = 0, expires = 0;
   if (!no_duplicate_members(payload) || !exact_object(payload, names, 14, v) ||
       !raw_uint(raw, raw_n, "v", &version) || !raw_uint(raw, raw_n, "team_id", &team) ||
       !raw_uint(raw, raw_n, "iat", &issued) || !raw_uint(raw, raw_n, "exp", &expires) ||
       version != 1 || team <= 0 || now < 0 || issued > now || expires <= now ||
       expires <= issued || expires - issued > SERVER_MGMT_TOKEN_MAX_LIFETIME)
      return 0;
   for (size_t i = 0; i < 14; ++i)
      if ((i == 0 || i == 4 || i == 12 || i == 13) ? !cJSON_IsNumber(v[i]) : !cJSON_IsString(v[i]))
         return 0;
   if (strcmp(v[1]->valuestring, issuer) != 0 || strcmp(v[2]->valuestring, audience) != 0 ||
       !identity_key(v[3]->valuestring) ||
       (strcmp(v[5]->valuestring, "remote_writes") != 0 &&
        strcmp(v[5]->valuestring, "remote_reads") != 0) ||
       !ascii_token(v[6]->valuestring, 16, 128) || !ascii_token(v[7]->valuestring, 1, 128) ||
       !lower_hex(v[8]->valuestring, 64, 64) ||
       (request_sha256 && !exact_secret(v[8]->valuestring, request_sha256, 64)) ||
       !control_free(v[9]->valuestring, 1, 511) || strcmp(v[9]->valuestring, peer_issuer) != 0 ||
       !lower_hex(v[10]->valuestring, 0, 79) || strcmp(v[10]->valuestring, peer_serial) != 0 ||
       !lower_hex(v[11]->valuestring, 64, 64) ||
       !exact_secret(v[11]->valuestring, peer_fingerprint, 64))
      return 0;
   out->version = 1;
   out->team_id = team;
   out->issued_at = issued;
   out->expires_at = expires;
   return copy_string(v[1], out->issuer, sizeof(out->issuer)) &&
          copy_string(v[2], out->audience, sizeof(out->audience)) &&
          copy_string(v[3], out->subject, sizeof(out->subject)) &&
          copy_string(v[5], out->capability, sizeof(out->capability)) &&
          copy_string(v[6], out->jti, sizeof(out->jti)) &&
          copy_string(v[7], out->correlation_id, sizeof(out->correlation_id)) &&
          copy_string(v[8], out->request_sha256, sizeof(out->request_sha256)) &&
          copy_string(v[9], out->peer_issuer, sizeof(out->peer_issuer)) &&
          copy_string(v[10], out->peer_serial, sizeof(out->peer_serial)) &&
          copy_string(v[11], out->peer_fingerprint, sizeof(out->peer_fingerprint));
}

static server_mgmt_token_result_t
verify_core(const char *jwt, size_t jwt_len, const char *jwks_json, const char *expected_issuer,
            const char *expected_audience, const char *peer_issuer, const char *peer_serial,
            const char *peer_fingerprint, const char *request_sha256,
            const char *required_capability, int64_t now, server_mgmt_token_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!jwt || !jwks_json || !out || !control_free(expected_issuer, 1, 255) ||
       !ascii_token(expected_audience, 1, 127) || !control_free(peer_issuer, 1, 511) ||
       !lower_hex(peer_serial, 0, 79) || !lower_hex(peer_fingerprint, 64, 64) ||
       (request_sha256 && !lower_hex(request_sha256, 64, 64)) || now < 0)
      return SERVER_MGMT_TOKEN_INVALID;
   size_t wire_n = jwt_len;
   size_t jwks_n = strnlen(jwks_json, JWKS_MAX + 1);
   if (!wire_n || wire_n > TOKEN_WIRE_MAX || memchr(jwt, '\0', wire_n) || !jwks_n ||
       jwks_n > JWKS_MAX)
      return SERVER_MGMT_TOKEN_INVALID;
   const char *dot1 = memchr(jwt, '.', wire_n);
   size_t after_dot1 = dot1 ? wire_n - (size_t)(dot1 + 1 - jwt) : 0;
   const char *dot2 = dot1 ? memchr(dot1 + 1, '.', after_dot1) : NULL;
   size_t after_dot2 = dot2 ? wire_n - (size_t)(dot2 + 1 - jwt) : 0;
   if (!dot1 || !dot2 || dot1 == jwt || dot2 == dot1 + 1 || !after_dot2 ||
       memchr(dot2 + 1, '.', after_dot2))
      return SERVER_MGMT_TOKEN_INVALID;
   size_t henc_n = (size_t)(dot1 - jwt), penc_n = (size_t)(dot2 - dot1 - 1);
   size_t senc_n = wire_n - (size_t)(dot2 + 1 - jwt);
   unsigned char header_raw[TOKEN_HEADER_MAX + 1], payload_raw[TOKEN_PAYLOAD_MAX + 1];
   unsigned char signature[TOKEN_SIG_MAX];
   size_t header_n = 0, payload_n = 0, signature_n = 0;
   cJSON *header = NULL, *payload = NULL;
   EVP_PKEY *key = NULL;
   server_mgmt_token_claims_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   server_mgmt_token_result_t result = SERVER_MGMT_TOKEN_INVALID;
   int ok = decode_segment(jwt, henc_n, header_raw, TOKEN_HEADER_MAX, &header_n) &&
            decode_segment(dot1 + 1, penc_n, payload_raw, TOKEN_PAYLOAD_MAX, &payload_n) &&
            b64_decode(dot2 + 1, senc_n, signature, TOKEN_SIG_MAX, &signature_n) &&
            b64_canonical(dot2 + 1, senc_n, signature, signature_n) && signature_n > 0;
   if (!ok)
      goto done;
   ok = 0;
   header_raw[header_n] = '\0';
   payload_raw[payload_n] = '\0';
   header = parse_json(header_raw, header_n);
   payload = parse_json(payload_raw, payload_n);
   if (!header || !payload || !parse_header(header, candidate.kid))
      goto done;
   key_select_result_t selected = select_key(jwks_json, jwks_n, candidate.kid, &key);
   if (selected == KEY_SELECT_UNKNOWN)
   {
      result = SERVER_MGMT_TOKEN_UNKNOWN_KID;
      goto done;
   }
   if (selected != KEY_SELECT_OK || !key ||
       !verify_signature(key, jwt, (size_t)(dot2 - jwt), signature, signature_n) ||
       !parse_payload(payload, payload_raw, payload_n, expected_issuer, expected_audience,
                      peer_issuer, peer_serial, peer_fingerprint, request_sha256, now,
                      &candidate) ||
       (required_capability && strcmp(candidate.capability, required_capability)))
      goto done;
   *out = candidate;
   ok = 1;
   result = SERVER_MGMT_TOKEN_OK;
done:
   if (!ok)
      memset(out, 0, sizeof(*out));
   EVP_PKEY_free(key);
   json_delete(header);
   json_delete(payload);
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(header_raw, sizeof(header_raw));
   OPENSSL_cleanse(payload_raw, sizeof(payload_raw));
   OPENSSL_cleanse(signature, sizeof(signature));
   return result;
}

server_mgmt_token_result_t
server_mgmt_token_verify_ex(const char *jwt, size_t jwt_len, const char *jwks_json,
                            const char *expected_issuer, const char *expected_audience,
                            const char *peer_issuer, const char *peer_serial,
                            const char *peer_fingerprint, const char *request_sha256, int64_t now,
                            server_mgmt_token_claims_t *out)
{
   if (!request_sha256)
   {
      if (out)
         memset(out, 0, sizeof(*out));
      return SERVER_MGMT_TOKEN_INVALID;
   }
   return verify_core(jwt, jwt_len, jwks_json, expected_issuer, expected_audience, peer_issuer,
                      peer_serial, peer_fingerprint, request_sha256, "remote_writes", now, out);
}

server_mgmt_token_result_t server_mgmt_token_verify_read_claims_ex(
    const char *jwt, size_t jwt_len, const char *jwks_json, const char *expected_issuer,
    const char *expected_audience, const char *peer_issuer, const char *peer_serial,
    const char *peer_fingerprint, int64_t now, server_mgmt_token_claims_t *out)
{
   return verify_core(jwt, jwt_len, jwks_json, expected_issuer, expected_audience, peer_issuer,
                      peer_serial, peer_fingerprint, NULL, "remote_reads", now, out);
}

int server_mgmt_token_verify(const char *jwt, size_t jwt_len, const char *jwks_json,
                             const char *expected_issuer, const char *expected_audience,
                             const char *peer_issuer, const char *peer_serial,
                             const char *peer_fingerprint, const char *request_sha256, int64_t now,
                             server_mgmt_token_claims_t *out)
{
   return server_mgmt_token_verify_ex(jwt, jwt_len, jwks_json, expected_issuer, expected_audience,
                                      peer_issuer, peer_serial, peer_fingerprint, request_sha256,
                                      now, out) == SERVER_MGMT_TOKEN_OK;
}

/* ---------------------------------------------------------------------------
 * Data-plane identity token (per-user remote_writes, proposal §4). A strictly
 * separate token type from the management JWT above: it requires the
 * `aimee-id+jwt` header `typ`, carries a three-level `tier` instead of a
 * capability, and has NO peer-cert binding or request digest. It reuses this
 * file's vetted JWS/JWKS primitives (decode_segment, b64_decode, b64_canonical,
 * parse_json, select_key, verify_signature). See server_identity_token.h.
 * ------------------------------------------------------------------------- */

/* Header schema for the identity token: alg=RS256, typ=aimee-id+jwt, kid. The
 * distinct `typ` is what makes a management token unverifiable here (and an
 * identity token unverifiable by parse_header), independent of the audience. */
static int parse_identity_header(cJSON *header, char kid[65])
{
   static const char *const names[] = {"alg", "typ", "kid"};
   const cJSON *v[3];
   return no_duplicate_members(header) && exact_object(header, names, 3, v) &&
          cJSON_IsString(v[0]) && strcmp(v[0]->valuestring, "RS256") == 0 && cJSON_IsString(v[1]) &&
          strcmp(v[1]->valuestring, "aimee-id+jwt") == 0 && cJSON_IsString(v[2]) &&
          ascii_token(v[2]->valuestring, 1, 64) && copy_string(v[2], kid, 65);
}

static int identity_tier_from_str(const char *s, kb_identity_tier_t *out)
{
   if (strcmp(s, "off") == 0)
      *out = KB_IDENTITY_TIER_OFF;
   else if (strcmp(s, "data") == 0)
      *out = KB_IDENTITY_TIER_DATA;
   else if (strcmp(s, "full") == 0)
      *out = KB_IDENTITY_TIER_FULL;
   else
      return 0;
   return 1;
}

static int parse_identity_payload(cJSON *payload, const unsigned char *raw, size_t raw_n,
                                  const char *issuer, const char *audience, int64_t now,
                                  server_identity_token_claims_t *out)
{
   static const char *const names[] = {"v",    "iss", "aud", "sub", "team_id",
                                       "tier", "jti", "iat", "exp"};
   const cJSON *v[9];
   int64_t version = 0, team = 0, issued = 0, expires = 0;
   if (!no_duplicate_members(payload) || !exact_object(payload, names, 9, v) ||
       !raw_uint(raw, raw_n, "v", &version) || !raw_uint(raw, raw_n, "team_id", &team) ||
       !raw_uint(raw, raw_n, "iat", &issued) || !raw_uint(raw, raw_n, "exp", &expires) ||
       version != 1 || team <= 0 || now < 0 || issued > now || expires <= now ||
       expires <= issued || expires - issued > SERVER_IDENTITY_TOKEN_MAX_LIFETIME)
      return 0;
   /* Types: v/team_id/iat/exp are numbers, the rest strings. */
   for (size_t i = 0; i < 9; ++i)
      if ((i == 0 || i == 4 || i == 7 || i == 8) ? !cJSON_IsNumber(v[i]) : !cJSON_IsString(v[i]))
         return 0;
   kb_identity_tier_t tier;
   if (strcmp(v[1]->valuestring, issuer) != 0 || strcmp(v[2]->valuestring, audience) != 0 ||
       !server_identity_subject_valid(v[3]->valuestring) ||
       !identity_tier_from_str(v[5]->valuestring, &tier) || !ascii_token(v[6]->valuestring, 8, 128))
      return 0;
   out->team_id = team;
   out->tier = tier;
   out->issued_at = issued;
   out->expires_at = expires;
   if (!copy_string(v[1], out->issuer, sizeof(out->issuer)) ||
       !copy_string(v[2], out->audience, sizeof(out->audience)) ||
       !copy_string(v[3], out->subject, sizeof(out->subject)) ||
       !copy_string(v[6], out->jti, sizeof(out->jti)))
      return 0;
   return 1;
}

server_identity_token_result_t
server_identity_token_verify(const char *jwt, size_t jwt_len, const char *jwks_json,
                             const char *expected_issuer, const char *expected_audience,
                             int64_t now, server_identity_token_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!jwt || !jwks_json || !out || !control_free(expected_issuer, 1, 255) ||
       !ascii_token(expected_audience, 1, 127) || now < 0)
      return SERVER_IDENTITY_TOKEN_INVALID;
   size_t wire_n = jwt_len;
   size_t jwks_n = strnlen(jwks_json, JWKS_MAX + 1);
   if (!wire_n || wire_n > TOKEN_WIRE_MAX || memchr(jwt, '\0', wire_n) || !jwks_n ||
       jwks_n > JWKS_MAX)
      return SERVER_IDENTITY_TOKEN_INVALID;
   const char *dot1 = memchr(jwt, '.', wire_n);
   size_t after_dot1 = dot1 ? wire_n - (size_t)(dot1 + 1 - jwt) : 0;
   const char *dot2 = dot1 ? memchr(dot1 + 1, '.', after_dot1) : NULL;
   size_t after_dot2 = dot2 ? wire_n - (size_t)(dot2 + 1 - jwt) : 0;
   if (!dot1 || !dot2 || dot1 == jwt || dot2 == dot1 + 1 || !after_dot2 ||
       memchr(dot2 + 1, '.', after_dot2))
      return SERVER_IDENTITY_TOKEN_INVALID;
   size_t henc_n = (size_t)(dot1 - jwt), penc_n = (size_t)(dot2 - dot1 - 1);
   size_t senc_n = wire_n - (size_t)(dot2 + 1 - jwt);
   unsigned char header_raw[TOKEN_HEADER_MAX + 1], payload_raw[TOKEN_PAYLOAD_MAX + 1];
   unsigned char signature[TOKEN_SIG_MAX];
   size_t header_n = 0, payload_n = 0, signature_n = 0;
   cJSON *header = NULL, *payload = NULL;
   EVP_PKEY *key = NULL;
   server_identity_token_claims_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   server_identity_token_result_t result = SERVER_IDENTITY_TOKEN_INVALID;
   int ok = decode_segment(jwt, henc_n, header_raw, TOKEN_HEADER_MAX, &header_n) &&
            decode_segment(dot1 + 1, penc_n, payload_raw, TOKEN_PAYLOAD_MAX, &payload_n) &&
            b64_decode(dot2 + 1, senc_n, signature, TOKEN_SIG_MAX, &signature_n) &&
            b64_canonical(dot2 + 1, senc_n, signature, signature_n) && signature_n > 0;
   if (!ok)
      goto done;
   ok = 0;
   header_raw[header_n] = '\0';
   payload_raw[payload_n] = '\0';
   header = parse_json(header_raw, header_n);
   payload = parse_json(payload_raw, payload_n);
   if (!header || !payload || !parse_identity_header(header, candidate.kid))
      goto done;
   key_select_result_t selected = select_key(jwks_json, jwks_n, candidate.kid, &key);
   if (selected == KEY_SELECT_UNKNOWN)
   {
      result = SERVER_IDENTITY_TOKEN_UNKNOWN_KID;
      goto done;
   }
   if (selected != KEY_SELECT_OK || !key ||
       !verify_signature(key, jwt, (size_t)(dot2 - jwt), signature, signature_n) ||
       !parse_identity_payload(payload, payload_raw, payload_n, expected_issuer, expected_audience,
                               now, &candidate))
      goto done;
   *out = candidate;
   ok = 1;
   result = SERVER_IDENTITY_TOKEN_OK;
done:
   if (!ok)
      memset(out, 0, sizeof(*out));
   EVP_PKEY_free(key);
   json_delete(header);
   json_delete(payload);
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(header_raw, sizeof(header_raw));
   OPENSSL_cleanse(payload_raw, sizeof(payload_raw));
   OPENSSL_cleanse(signature, sizeof(signature));
   return result;
}
