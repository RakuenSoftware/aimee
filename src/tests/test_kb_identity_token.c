/* test_kb_identity_token.c — unit tests for the kb-signed data-plane identity
 * token builder (proposal per-user-remote-writes-authz.md §4).
 *
 * Covers: the pinned claim set round-trips; the tier enum maps to the wire
 * string; a `sub` carrying JSON metacharacters is escaped and cannot inject or
 * override a claim (the load-bearing security property); and the fail-closed
 * validation + output-preflight + signer-failure paths. */
#include <assert.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "kb_identity_token.h"

/* --- RS256 test signer (mirrors test_kb_mgmt_token.c) --- */
typedef struct
{
   EVP_PKEY *key;
   int force_fail;   /* return 0 from the signer */
   size_t force_len; /* if >0, emit this many bytes instead of a real signature */
} signer_t;

static int sign_rs256(void *opaque, const unsigned char *input, size_t input_n, unsigned char *sig,
                      size_t cap, size_t *sig_n)
{
   signer_t *s = opaque;
   if (s->force_fail)
      return 0;
   if (s->force_len)
   {
      if (s->force_len > cap)
         return 0;
      memset(sig, 0xa5, s->force_len);
      *sig_n = s->force_len;
      return 1;
   }
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   size_t n = cap;
   int ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, s->key) == 1 &&
            EVP_DigestSign(md, sig, &n, input, input_n) == 1;
   EVP_MD_CTX_free(md);
   if (!ok)
      return 0;
   *sig_n = n;
   return 1;
}

static EVP_PKEY *new_rsa(unsigned bits)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
          EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)bits) == 1 && EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

/* --- minimal base64url decoder for verifying the payload segment --- */
static int b64url_val(char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '-')
      return 62;
   if (c == '_')
      return 63;
   return -1;
}
static size_t b64url_decode(const char *in, size_t n, unsigned char *out, size_t cap)
{
   size_t o = 0;
   unsigned acc = 0;
   int bits = 0;
   for (size_t i = 0; i < n; i++)
   {
      int v = b64url_val(in[i]);
      assert(v >= 0);
      acc = (acc << 6) | (unsigned)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         assert(o < cap);
         out[o++] = (unsigned char)((acc >> bits) & 0xFF);
      }
   }
   return o;
}

/* Parse the payload (2nd segment) of a compact JWS into a cJSON object. */
static cJSON *decode_payload(const char *jwt)
{
   const char *d1 = strchr(jwt, '.');
   assert(d1);
   const char *d2 = strchr(d1 + 1, '.');
   assert(d2);
   unsigned char buf[2048];
   size_t n = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), buf, sizeof(buf) - 1);
   buf[n] = '\0';
   cJSON *o = cJSON_Parse((const char *)buf);
   assert(o);
   return o;
}
static cJSON *decode_header(const char *jwt)
{
   const char *d1 = strchr(jwt, '.');
   assert(d1);
   unsigned char buf[512];
   size_t n = b64url_decode(jwt, (size_t)(d1 - jwt), buf, sizeof(buf) - 1);
   buf[n] = '\0';
   cJSON *o = cJSON_Parse((const char *)buf);
   assert(o);
   return o;
}

static kb_identity_token_claims_t base_claims(void)
{
   kb_identity_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "kb");
   snprintf(c.audience, sizeof(c.audience), "server-abc");
   snprintf(c.subject, sizeof(c.subject), "https://idp.example.com|user-42");
   c.team_id = 7;
   c.tier = KB_IDENTITY_TIER_DATA;
   snprintf(c.jti, sizeof(c.jti), "jti-0001");
   snprintf(c.kid, sizeof(c.kid), "kid-2026-a");
   c.issued_at = 1000;
   c.expires_at = 1300;
   return c;
}

static const char *sstr(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   assert(cJSON_IsString(v));
   return v->valuestring;
}
static double snum(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   assert(cJSON_IsNumber(v));
   return v->valuedouble;
}

int main(void)
{
   EVP_PKEY *key = new_rsa(2048);
   signer_t s = {key, 0, 0};
   char jwt[KB_IDENTITY_TOKEN_WIRE_MAX];
   size_t jl = 0;

   /* 1) Happy path: the pinned claim set round-trips. */
   kb_identity_token_claims_t c = base_claims();
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_OK);
   assert(jl == strlen(jwt) && jl > 0);
   cJSON *h = decode_header(jwt);
   assert(strcmp(sstr(h, "typ"), "aimee-id+jwt") == 0); /* distinct type */
   assert(strcmp(sstr(h, "alg"), "RS256") == 0);
   assert(strcmp(sstr(h, "kid"), "kid-2026-a") == 0);
   cJSON_Delete(h);
   cJSON *p = decode_payload(jwt);
   assert(strcmp(sstr(p, "iss"), "kb") == 0);
   assert(strcmp(sstr(p, "aud"), "server-abc") == 0);
   assert(strcmp(sstr(p, "sub"), "https://idp.example.com|user-42") == 0);
   assert((int)snum(p, "team_id") == 7);
   assert(strcmp(sstr(p, "tier"), "data") == 0);
   assert(strcmp(sstr(p, "jti"), "jti-0001") == 0);
   assert((long)snum(p, "iat") == 1000 && (long)snum(p, "exp") == 1300);
   cJSON_Delete(p);

   /* 2) Tier enum -> wire string for every level. */
   struct
   {
      kb_identity_tier_t t;
      const char *w;
   } tiers[] = {{KB_IDENTITY_TIER_OFF, "off"},
                {KB_IDENTITY_TIER_DATA, "data"},
                {KB_IDENTITY_TIER_FULL, "full"}};
   for (unsigned i = 0; i < 3; i++)
   {
      c = base_claims();
      c.tier = tiers[i].t;
      assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
             KB_IDENTITY_TOKEN_OK);
      cJSON *pp = decode_payload(jwt);
      assert(strcmp(sstr(pp, "tier"), tiers[i].w) == 0);
      cJSON_Delete(pp);
      assert(strcmp(kb_identity_tier_str(tiers[i].t), tiers[i].w) == 0);
   }

   /* 3) SECURITY: a crafted sub cannot inject or override a claim. The subject
    *    carries JSON metacharacters that, unescaped, would set tier=full. */
   c = base_claims();
   c.tier = KB_IDENTITY_TIER_OFF;
   snprintf(c.subject, sizeof(c.subject), "evil\",\"tier\":\"full\",\"x\":\"");
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_OK);
   cJSON *pi = decode_payload(jwt);
   assert(strcmp(sstr(pi, "sub"), "evil\",\"tier\":\"full\",\"x\":\"") == 0); /* literal, escaped */
   assert(strcmp(sstr(pi, "tier"), "off") == 0); /* NOT overridden to full */
   cJSON_Delete(pi);

   /* 4) Fail-closed validation. */
   c = base_claims();
   c.subject[0] = '\0';
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_INVALID);
   c = base_claims();
   c.team_id = 0;
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_INVALID);
   c = base_claims();
   c.expires_at = c.issued_at; /* non-increasing window */
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_INVALID);
   c = base_claims();
   c.tier = (kb_identity_tier_t)9; /* out of range */
   assert(kb_identity_token_build(&c, sign_rs256, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_INVALID);
   c = base_claims();
   assert(kb_identity_token_build(&c, NULL, &s, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_INVALID);
   assert(jl == 0 && jwt[0] == '\0'); /* outputs cleared on failure */

   /* 5) Output preflight: a too-small buffer is rejected, not overrun. */
   c = base_claims();
   char tiny[64];
   assert(kb_identity_token_build(&c, sign_rs256, &s, tiny, sizeof(tiny), &jl) ==
          KB_IDENTITY_TOKEN_OUTPUT_TOO_SMALL);

   /* 6) Signer failure and a too-short signature are rejected. */
   c = base_claims();
   signer_t fail = {key, 1, 0};
   assert(kb_identity_token_build(&c, sign_rs256, &fail, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_SIGN_UNAVAILABLE);
   signer_t shortsig = {key, 0, 8}; /* 8 bytes < 256 minimum */
   assert(kb_identity_token_build(&c, sign_rs256, &shortsig, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_SIGN_UNAVAILABLE);

   EVP_PKEY_free(key);
   printf("  PASS: kb_identity_token builds the pinned claim set, escapes sub, fails closed\n");
   return 0;
}
