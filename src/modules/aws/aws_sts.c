#include <aimee/core/codec/base64.h>
/* modules/aws/aws_sts.c: STS request construction + parse + web-identity verify.
 * See aws_sts.h. Pure/offline — no dispatch, no network, no clock. */

#include "aws_sts.h"

#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- base64url decode (no padding), strict (mirrors kb/auth_oidc.c) --- */

static size_t b64url_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
   if (!in_len)
      return 0;
   size_t written = 0;
   return aimee_codec_base64_decode(in, in_len, out, out_cap, AIMEE_BASE64_URL_COMPAT, &written)
              ? written
              : (size_t)-1;
}

static int b64url_decode_str(const char *in, size_t in_len, char *out, size_t out_cap,
                             size_t *decoded_len)
{
   if (out_cap == 0 || !decoded_len)
      return -1;
   size_t n = b64url_decode(in, in_len, (unsigned char *)out, out_cap - 1);
   if (n == (size_t)-1)
      return -1;
   out[n] = '\0';
   *decoded_len = n;
   return 0;
}

static void cjson_cleanse(cJSON *item)
{
   for (cJSON *current = item; current; current = current->next)
   {
      if (current->child)
         cjson_cleanse(current->child);
      if (current->valuestring)
         OPENSSL_cleanse(current->valuestring, strlen(current->valuestring));
      if (current->string)
         OPENSSL_cleanse(current->string, strlen(current->string));
   }
}

static void cjson_cleanse_delete(cJSON *item)
{
   cjson_cleanse(item);
   cJSON_Delete(item);
}

static int checked_double_to_long(double value, long *out)
{
   const double exclusive_upper = (double)LONG_MAX + 1.0;
   if (!out || !isfinite(value) || value < (double)LONG_MIN || value >= exclusive_upper)
      return 0;
   *out = (long)value;
   return 1;
}

static int json_has_decoded_nul_escape(const char *raw, size_t length)
{
   int in_string = 0;
   for (size_t i = 0; i < length; ++i)
   {
      if (!in_string)
      {
         if (raw[i] == '"')
            in_string = 1;
         continue;
      }
      if (raw[i] == '"')
      {
         in_string = 0;
         continue;
      }
      if (raw[i] != '\\' || i + 1 >= length)
         continue;
      if (raw[i + 1] == 'u' && i + 5 < length && raw[i + 2] == '0' && raw[i + 3] == '0' &&
          raw[i + 4] == '0' && raw[i + 5] == '0')
         return 1;
      ++i;
   }
   return 0;
}

/* --- JWKS -> public key (RSA for RS256, EC P-256 for ES256) --- */

static EVP_PKEY *jwks_rsa_key(cJSON *k)
{
   cJSON *nj = cJSON_GetObjectItemCaseSensitive(k, "n");
   cJSON *ej = cJSON_GetObjectItemCaseSensitive(k, "e");
   if (!cJSON_IsString(nj) || !cJSON_IsString(ej))
      return NULL;
   unsigned char nbuf[1024], ebuf[16];
   size_t nlen = b64url_decode(nj->valuestring, strlen(nj->valuestring), nbuf, sizeof(nbuf));
   size_t elen = b64url_decode(ej->valuestring, strlen(ej->valuestring), ebuf, sizeof(ebuf));
   if (nlen == (size_t)-1 || elen == (size_t)-1 || nlen == 0 || elen == 0)
      return NULL;
   EVP_PKEY *pkey = NULL;
   BIGNUM *n = BN_bin2bn(nbuf, (int)nlen, NULL);
   BIGNUM *e = BN_bin2bn(ebuf, (int)elen, NULL);
   OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
   EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
   if (n && e && bld && pctx && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
       OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
   {
      OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
      if (params && EVP_PKEY_fromdata_init(pctx) > 0)
         EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
      OSSL_PARAM_free(params);
   }
   EVP_PKEY_CTX_free(pctx);
   OSSL_PARAM_BLD_free(bld);
   BN_free(n);
   BN_free(e);
   return pkey;
}

static EVP_PKEY *jwks_ec_p256_key(cJSON *k)
{
   cJSON *crv = cJSON_GetObjectItemCaseSensitive(k, "crv");
   cJSON *xj = cJSON_GetObjectItemCaseSensitive(k, "x");
   cJSON *yj = cJSON_GetObjectItemCaseSensitive(k, "y");
   if (!cJSON_IsString(crv) || strcmp(crv->valuestring, "P-256") != 0 || !cJSON_IsString(xj) ||
       !cJSON_IsString(yj))
      return NULL;
   unsigned char xb[32], yb[32];
   size_t xl = b64url_decode(xj->valuestring, strlen(xj->valuestring), xb, sizeof(xb));
   size_t yl = b64url_decode(yj->valuestring, strlen(yj->valuestring), yb, sizeof(yb));
   if (xl != 32 || yl != 32)
      return NULL;
   /* Uncompressed point: 0x04 || X || Y. */
   unsigned char point[65];
   point[0] = 0x04;
   memcpy(point + 1, xb, 32);
   memcpy(point + 33, yb, 32);
   EVP_PKEY *pkey = NULL;
   OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
   EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
   if (bld && pctx &&
       OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) &&
       OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)))
   {
      OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
      if (params && EVP_PKEY_fromdata_init(pctx) > 0)
         EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
      OSSL_PARAM_free(params);
   }
   EVP_PKEY_CTX_free(pctx);
   OSSL_PARAM_BLD_free(bld);
   return pkey;
}

/* Resolve the verifying key for `kid` + `want_ec` (0=RSA,1=EC) from the JWKS. */
static EVP_PKEY *jwks_pubkey(const char *jwks_json, const char *kid, int want_ec)
{
   cJSON *root = cJSON_Parse(jwks_json);
   if (!root)
      return NULL;
   cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
   EVP_PKEY *pkey = NULL;
   if (!cJSON_IsArray(keys))
   {
      cjson_cleanse_delete(root);
      return NULL;
   }
   const char *want_kty = want_ec ? "EC" : "RSA";
   cJSON *chosen = NULL;
   int match_count = 0;
   cJSON *k = NULL;
   cJSON_ArrayForEach(k, keys)
   {
      cJSON *kty = cJSON_GetObjectItemCaseSensitive(k, "kty");
      if (!cJSON_IsString(kty) || strcmp(kty->valuestring, want_kty) != 0)
         continue;
      match_count++;
      cJSON *kidj = cJSON_GetObjectItemCaseSensitive(k, "kid");
      if (kid && kid[0])
      {
         if (cJSON_IsString(kidj) && strcmp(kidj->valuestring, kid) == 0)
         {
            chosen = k;
            break;
         }
      }
      else if (!chosen)
      {
         chosen = k;
      }
   }
   if (!chosen || ((!kid || !kid[0]) && match_count != 1))
   {
      cjson_cleanse_delete(root);
      return NULL;
   }
   pkey = want_ec ? jwks_ec_p256_key(chosen) : jwks_rsa_key(chosen);
   cjson_cleanse_delete(root);
   return pkey;
}

/* --- signature verification --- */

static int rsa_verify(EVP_PKEY *pkey, const char *si, size_t si_len, const unsigned char *sig,
                      size_t siglen)
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   if (!md)
      return 0;
   int ok =
       (EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestVerifyUpdate(md, si, si_len) == 1 && EVP_DigestVerifyFinal(md, sig, siglen) == 1);
   EVP_MD_CTX_free(md);
   return ok;
}

/* ES256 JWS signature is raw r||s (2x32 bytes); OpenSSL wants a DER ECDSA-Sig. */
static int ecdsa_verify(EVP_PKEY *pkey, const char *si, size_t si_len, const unsigned char *raw,
                        size_t raw_len)
{
   if (raw_len != 64)
      return 0;
   ECDSA_SIG *s = ECDSA_SIG_new();
   BIGNUM *r = BN_bin2bn(raw, 32, NULL);
   BIGNUM *sn = BN_bin2bn(raw + 32, 32, NULL);
   int ok = 0;
   unsigned char *der = NULL;
   if (s && r && sn && ECDSA_SIG_set0(s, r, sn))
   {
      r = NULL; /* owned by s now */
      sn = NULL;
      int derlen = i2d_ECDSA_SIG(s, &der);
      if (derlen > 0)
      {
         EVP_MD_CTX *md = EVP_MD_CTX_new();
         if (md)
         {
            ok = (EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pkey) == 1 &&
                  EVP_DigestVerifyUpdate(md, si, si_len) == 1 &&
                  EVP_DigestVerifyFinal(md, der, (size_t)derlen) == 1);
            EVP_MD_CTX_free(md);
         }
      }
   }
   OPENSSL_free(der);
   BN_free(r);
   BN_free(sn);
   ECDSA_SIG_free(s);
   return ok;
}

static int aud_contains(const cJSON *payload, const char *aud_want)
{
   const cJSON *aud = cJSON_GetObjectItemCaseSensitive(payload, "aud");
   if (cJSON_IsString(aud))
      return strcmp(aud->valuestring, aud_want) == 0;
   if (cJSON_IsArray(aud))
   {
      const cJSON *e = NULL;
      cJSON_ArrayForEach(e, aud)
      {
         if (cJSON_IsString(e) && strcmp(e->valuestring, aud_want) == 0)
            return 1;
      }
   }
   return 0;
}

aws_webid_status_t aws_webidentity_validate(const char *token, const char *jwks_json,
                                            const char *expected_iss, const char *expected_aud,
                                            long now, aws_webid_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));

   aws_webid_status_t st = AWS_WEBID_ERR_MALFORMED;
   char hdr_json[1024] = {0};
   char kid[256] = {0};
   unsigned char sig[512] = {0};
   char *pl_json = NULL;
   cJSON *hdr = NULL, *pl = NULL;
   EVP_PKEY *pkey = NULL;
   if (!token || !jwks_json || !jwks_json[0])
      goto done;

   /* Fail CLOSED on a missing/empty expectation: an unconfigured issuer or
    * audience must never silently accept ANY iss/aud (a misconfiguration would
    * otherwise trust arbitrarily-issued tokens). Both are mandatory. */
   if (!expected_iss || !expected_iss[0] || !expected_aud || !expected_aud[0])
   {
      st = AWS_WEBID_ERR_CLAIMS;
      goto done;
   }

   /* Split the compact JWS into three segments. */
   const char *dot1 = strchr(token, '.');
   if (!dot1)
      goto done;
   const char *dot2 = strchr(dot1 + 1, '.');
   if (!dot2)
      goto done;
   const char *hdr_b64 = token;
   size_t hdr_len = (size_t)(dot1 - token);
   const char *pl_b64 = dot1 + 1;
   size_t pl_len = (size_t)(dot2 - pl_b64);
   const char *sig_b64 = dot2 + 1;
   size_t sig_len = strlen(sig_b64);
   if (hdr_len == 0 || pl_len == 0 || sig_len == 0 || strchr(sig_b64, '.'))
      goto done;

   /* Header: alg must be RS256 or ES256; capture kid. */
   size_t hdr_json_len = 0;
   if (b64url_decode_str(hdr_b64, hdr_len, hdr_json, sizeof(hdr_json), &hdr_json_len) != 0 ||
       memchr(hdr_json, '\0', hdr_json_len) || json_has_decoded_nul_escape(hdr_json, hdr_json_len))
      goto done;
   hdr = cJSON_Parse(hdr_json);
   if (!hdr)
      goto done;
   cJSON *alg = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
   int want_ec = -1;
   if (cJSON_IsString(alg))
   {
      if (strcmp(alg->valuestring, "RS256") == 0)
         want_ec = 0;
      else if (strcmp(alg->valuestring, "ES256") == 0)
         want_ec = 1;
   }
   cJSON *kidj = cJSON_GetObjectItemCaseSensitive(hdr, "kid");
   if (cJSON_IsString(kidj))
      snprintf(kid, sizeof(kid), "%s", kidj->valuestring);
   cjson_cleanse_delete(hdr);
   hdr = NULL;
   if (want_ec < 0)
   {
      st = AWS_WEBID_ERR_UNSUPPORTED_ALG;
      goto done;
   }

   /* Resolve key + verify signature over the raw "header.payload" bytes. */
   pkey = jwks_pubkey(jwks_json, kid, want_ec);
   if (!pkey)
   {
      st = AWS_WEBID_ERR_NO_KEY;
      goto done;
   }
   size_t siglen = b64url_decode(sig_b64, sig_len, sig, sizeof(sig));
   int verified = 0;
   if (siglen != (size_t)-1 && siglen > 0)
   {
      size_t si_len = (size_t)(dot2 - hdr_b64);
      verified = want_ec ? ecdsa_verify(pkey, hdr_b64, si_len, sig, siglen)
                         : rsa_verify(pkey, hdr_b64, si_len, sig, siglen);
   }
   EVP_PKEY_free(pkey);
   pkey = NULL;
   if (!verified)
   {
      st = AWS_WEBID_ERR_BAD_SIGNATURE;
      goto done;
   }

   /* Signature good — decode + validate claims (verify-then-trust). */
   pl_json = malloc(pl_len + 1);
   if (!pl_json)
      goto done;
   size_t pl_json_len = 0;
   if (b64url_decode_str(pl_b64, pl_len, pl_json, pl_len + 1, &pl_json_len) != 0 ||
       memchr(pl_json, '\0', pl_json_len) || json_has_decoded_nul_escape(pl_json, pl_json_len))
      goto done;
   pl = cJSON_Parse(pl_json);
   OPENSSL_cleanse(pl_json, pl_len + 1);
   free(pl_json);
   pl_json = NULL;
   if (!pl)
      goto done;

   const long skew = 60;
   st = AWS_WEBID_OK;
   do
   {
      cJSON *iss = cJSON_GetObjectItemCaseSensitive(pl, "iss");
      if (expected_iss && expected_iss[0])
      {
         if (!cJSON_IsString(iss) || strcmp(iss->valuestring, expected_iss) != 0)
         {
            st = AWS_WEBID_ERR_ISS;
            break;
         }
      }
      if (expected_aud && expected_aud[0] && !aud_contains(pl, expected_aud))
      {
         st = AWS_WEBID_ERR_AUD;
         break;
      }
      cJSON *exp = cJSON_GetObjectItemCaseSensitive(pl, "exp");
      if (!cJSON_IsNumber(exp))
      {
         st = AWS_WEBID_ERR_CLAIMS;
         break;
      }
      long expiry = 0;
      if (!checked_double_to_long(exp->valuedouble, &expiry))
      {
         st = AWS_WEBID_ERR_CLAIMS;
         break;
      }
      if ((long double)expiry + (long double)skew < (long double)now)
      {
         st = AWS_WEBID_ERR_EXPIRED;
         break;
      }
      cJSON *iat = cJSON_GetObjectItemCaseSensitive(pl, "iat");
      if (!cJSON_IsNumber(iat))
      {
         st = AWS_WEBID_ERR_IAT;
         break;
      }
      long iat_v = 0;
      if (!checked_double_to_long(iat->valuedouble, &iat_v) || iat_v < 0 ||
          (long double)iat_v > (long double)now + (long double)skew)
      {
         st = AWS_WEBID_ERR_IAT;
         break;
      }
      if (out)
      {
         out->expiry = expiry;
         out->issued_at = iat_v;
         if (cJSON_IsString(iss))
            snprintf(out->issuer, sizeof(out->issuer), "%s", iss->valuestring);
         cJSON *sub = cJSON_GetObjectItemCaseSensitive(pl, "sub");
         if (cJSON_IsString(sub))
            snprintf(out->subject, sizeof(out->subject), "%s", sub->valuestring);
         if (expected_aud && expected_aud[0])
            snprintf(out->audience, sizeof(out->audience), "%s", expected_aud);
      }
   } while (0);

done:
   if (st != AWS_WEBID_OK && out)
      memset(out, 0, sizeof(*out));
   cjson_cleanse_delete(pl);
   cjson_cleanse_delete(hdr);
   if (pl_json)
   {
      OPENSSL_cleanse(pl_json, pl_len + 1);
      free(pl_json);
   }
   EVP_PKEY_free(pkey);
   OPENSSL_cleanse(sig, sizeof(sig));
   OPENSSL_cleanse(kid, sizeof(kid));
   OPENSSL_cleanse(hdr_json, sizeof(hdr_json));
   return st;
}

/* --- form-body construction --- */

/* --- STS XML response parse (hostile-input-safe) --- */
