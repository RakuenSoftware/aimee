/* modules/aws/aws_sts.c: STS request construction + parse + web-identity verify.
 * See aws_sts.h. Pure/offline — no dispatch, no network, no clock. */

#include "aws_sts.h"

#include "cJSON.h"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- base64url decode (no padding), strict (mirrors kb/auth_oidc.c) --- */
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

static size_t b64url_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
   size_t o = 0;
   unsigned int buf = 0;
   int bits = 0;
   for (size_t i = 0; i < in_len; i++)
   {
      int v = b64url_val(in[i]);
      if (v < 0)
         return (size_t)-1;
      buf = (buf << 6) | (unsigned int)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= out_cap)
            return (size_t)-1;
         out[o++] = (unsigned char)((buf >> bits) & 0xFF);
      }
   }
   return o;
}

static int b64url_decode_str(const char *in, size_t in_len, char *out, size_t out_cap)
{
   if (out_cap == 0)
      return -1;
   size_t n = b64url_decode(in, in_len, (unsigned char *)out, out_cap - 1);
   if (n == (size_t)-1)
      return -1;
   out[n] = '\0';
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
      cJSON_Delete(root);
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
      cJSON_Delete(root);
      return NULL;
   }
   pkey = want_ec ? jwks_ec_p256_key(chosen) : jwks_rsa_key(chosen);
   cJSON_Delete(root);
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
   if (!token || !jwks_json || !jwks_json[0])
      return AWS_WEBID_ERR_MALFORMED;

   /* Split the compact JWS into three segments. */
   const char *dot1 = strchr(token, '.');
   if (!dot1)
      return AWS_WEBID_ERR_MALFORMED;
   const char *dot2 = strchr(dot1 + 1, '.');
   if (!dot2)
      return AWS_WEBID_ERR_MALFORMED;
   const char *hdr_b64 = token;
   size_t hdr_len = (size_t)(dot1 - token);
   const char *pl_b64 = dot1 + 1;
   size_t pl_len = (size_t)(dot2 - pl_b64);
   const char *sig_b64 = dot2 + 1;
   size_t sig_len = strlen(sig_b64);
   if (hdr_len == 0 || pl_len == 0 || sig_len == 0 || strchr(sig_b64, '.'))
      return AWS_WEBID_ERR_MALFORMED;

   /* Header: alg must be RS256 or ES256; capture kid. */
   char hdr_json[1024];
   if (b64url_decode_str(hdr_b64, hdr_len, hdr_json, sizeof(hdr_json)) != 0)
      return AWS_WEBID_ERR_MALFORMED;
   cJSON *hdr = cJSON_Parse(hdr_json);
   if (!hdr)
      return AWS_WEBID_ERR_MALFORMED;
   cJSON *alg = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
   int want_ec = -1;
   if (cJSON_IsString(alg))
   {
      if (strcmp(alg->valuestring, "RS256") == 0)
         want_ec = 0;
      else if (strcmp(alg->valuestring, "ES256") == 0)
         want_ec = 1;
   }
   char kid[256] = "";
   cJSON *kidj = cJSON_GetObjectItemCaseSensitive(hdr, "kid");
   if (cJSON_IsString(kidj))
      snprintf(kid, sizeof(kid), "%s", kidj->valuestring);
   cJSON_Delete(hdr);
   if (want_ec < 0)
      return AWS_WEBID_ERR_UNSUPPORTED_ALG;

   /* Resolve key + verify signature over the raw "header.payload" bytes. */
   EVP_PKEY *pkey = jwks_pubkey(jwks_json, kid, want_ec);
   if (!pkey)
      return AWS_WEBID_ERR_NO_KEY;
   unsigned char sig[512];
   size_t siglen = b64url_decode(sig_b64, sig_len, sig, sizeof(sig));
   int verified = 0;
   if (siglen != (size_t)-1 && siglen > 0)
   {
      size_t si_len = (size_t)(dot2 - hdr_b64);
      verified = want_ec ? ecdsa_verify(pkey, hdr_b64, si_len, sig, siglen)
                         : rsa_verify(pkey, hdr_b64, si_len, sig, siglen);
   }
   EVP_PKEY_free(pkey);
   if (!verified)
      return AWS_WEBID_ERR_BAD_SIGNATURE;

   /* Signature good — decode + validate claims (verify-then-trust). */
   char *pl_json = malloc(pl_len + 1);
   if (!pl_json)
      return AWS_WEBID_ERR_MALFORMED;
   if (b64url_decode_str(pl_b64, pl_len, pl_json, pl_len + 1) != 0)
   {
      free(pl_json);
      return AWS_WEBID_ERR_MALFORMED;
   }
   cJSON *pl = cJSON_Parse(pl_json);
   free(pl_json);
   if (!pl)
      return AWS_WEBID_ERR_MALFORMED;

   aws_webid_status_t st = AWS_WEBID_OK;
   const long skew = 60;
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
      long expiry = (long)exp->valuedouble;
      if (expiry + skew < now)
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
      long iat_v = (long)iat->valuedouble;
      if (iat_v < 0 || iat_v > now + skew)
      {
         st = AWS_WEBID_ERR_IAT;
         break;
      }
      if (out)
      {
         memset(out, 0, sizeof(*out));
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

   cJSON_Delete(pl);
   return st;
}

/* --- form-body construction --- */

/* Append "&key=<uri-encoded value>" (or "key=" for the first pair when *o==0). */
static int form_append(char *out, size_t cap, size_t *o, const char *key, const char *value)
{
   char enc[6144];
   if (aws_uri_encode(value ? value : "", 1, enc, sizeof(enc)) != 0)
      return -1;
   int n = snprintf(out + *o, cap - *o, "%s%s=%s", *o ? "&" : "", key, enc);
   if (n < 0 || (size_t)n >= cap - *o)
      return -1;
   *o += (size_t)n;
   return 0;
}

int aws_sts_assume_role_body(char *out, size_t cap, const char *role_arn,
                             const char *role_session_name, const char *external_id,
                             const char *session_policy_json)
{
   if (!out || cap == 0 || !role_arn || !role_session_name || !external_id)
      return -1;
   out[0] = '\0';
   size_t o = 0;
   char dur[8];
   snprintf(dur, sizeof(dur), "%d", AWS_STS_DURATION_SECONDS);
   if (form_append(out, cap, &o, "Action", "AssumeRole") != 0 ||
       form_append(out, cap, &o, "DurationSeconds", dur) != 0 ||
       form_append(out, cap, &o, "ExternalId", external_id) != 0)
      return -1;
   if (session_policy_json && session_policy_json[0] &&
       form_append(out, cap, &o, "Policy", session_policy_json) != 0)
      return -1;
   if (form_append(out, cap, &o, "RoleArn", role_arn) != 0 ||
       form_append(out, cap, &o, "RoleSessionName", role_session_name) != 0)
      return -1;
   return 0;
}

int aws_sts_assume_role_web_identity_body(char *out, size_t cap, const char *role_arn,
                                          const char *role_session_name,
                                          const char *web_identity_token,
                                          const char *session_policy_json)
{
   if (!out || cap == 0 || !role_arn || !role_session_name || !web_identity_token)
      return -1;
   out[0] = '\0';
   size_t o = 0;
   char dur[8];
   snprintf(dur, sizeof(dur), "%d", AWS_STS_DURATION_SECONDS);
   if (form_append(out, cap, &o, "Action", "AssumeRoleWithWebIdentity") != 0 ||
       form_append(out, cap, &o, "DurationSeconds", dur) != 0)
      return -1;
   if (session_policy_json && session_policy_json[0] &&
       form_append(out, cap, &o, "Policy", session_policy_json) != 0)
      return -1;
   if (form_append(out, cap, &o, "RoleArn", role_arn) != 0 ||
       form_append(out, cap, &o, "RoleSessionName", role_session_name) != 0 ||
       form_append(out, cap, &o, "WebIdentityToken", web_identity_token) != 0)
      return -1;
   return 0;
}

int aws_sts_build_signed_assume_role(aws_sts_signed_request_t *out, const char *region,
                                     const char *host, const char *access_key_id,
                                     const char *secret_access_key, const char *session_token,
                                     const char *role_arn, const char *role_session_name,
                                     const char *external_id, const char *session_policy_json,
                                     const char *amz_date, const char *date)
{
   if (!out || !region || !host || !access_key_id || !secret_access_key || !amz_date || !date)
      return -1;
   memset(out, 0, sizeof(*out));
   if (aws_sts_assume_role_body(out->body, sizeof(out->body), role_arn, role_session_name,
                                external_id, session_policy_json) != 0)
      return -1;

   char payload_hash[65];
   aws_sha256_hex((const unsigned char *)out->body, strlen(out->body), payload_hash);

   aws_sigv4_kv_t headers[] = {
       {"host", host},
       {"content-type", "application/x-www-form-urlencoded; charset=utf-8"},
       {"x-amz-date", amz_date},
   };
   aws_sigv4_request_t req;
   memset(&req, 0, sizeof(req));
   req.method = "POST";
   req.raw_path = "/";
   req.query = NULL;
   req.n_query = 0;
   req.headers = headers;
   req.n_headers = sizeof(headers) / sizeof(headers[0]);
   req.payload_hash = payload_hash;
   req.amz_date = amz_date;
   req.date = date;
   req.region = region;
   req.service = "sts";
   req.access_key_id = access_key_id;
   req.secret_access_key = secret_access_key;
   req.session_token = session_token;
   return aws_sigv4_sign(&req, &out->sig);
}

/* --- STS XML response parse (hostile-input-safe) --- */

/* Count occurrences of `needle` in [hay,end). */
static int count_occ(const char *hay, const char *end, const char *needle)
{
   size_t nl = strlen(needle);
   int c = 0;
   for (const char *p = hay; p + nl <= end; p++)
      if (memcmp(p, needle, nl) == 0)
         c++;
   return c;
}

/* Extract the text of <tag>...</tag> within [block,end) into out[cap]. Requires
 * EXACTLY one occurrence of the open tag (duplicate -> error). Returns 0/-1. */
static int extract_field(const char *block, const char *end, const char *tag, char *out, size_t cap)
{
   char open[64], close[64];
   if ((size_t)snprintf(open, sizeof(open), "<%s>", tag) >= sizeof(open) ||
       (size_t)snprintf(close, sizeof(close), "</%s>", tag) >= sizeof(close))
      return -1;
   if (count_occ(block, end, open) != 1)
      return -1; /* missing or duplicate */
   const char *o = NULL;
   size_t ol = strlen(open);
   for (const char *p = block; p + ol <= end; p++)
      if (memcmp(p, open, ol) == 0)
      {
         o = p;
         break;
      }
   if (!o)
      return -1;
   const char *vstart = o + ol;
   const char *c = NULL;
   size_t cl = strlen(close);
   for (const char *p = vstart; p + cl <= end; p++)
      if (memcmp(p, close, cl) == 0)
      {
         c = p;
         break;
      }
   if (!c || c < vstart)
      return -1;
   size_t vlen = (size_t)(c - vstart);
   if (vlen >= cap)
      return -1;
   memcpy(out, vstart, vlen);
   out[vlen] = '\0';
   return 0;
}

/* Case-insensitive substring search (for the XXE keyword guard). */
static int contains_ci(const char *hay, const char *needle)
{
   size_t nl = strlen(needle);
   for (const char *p = hay; *p; p++)
   {
      size_t i = 0;
      for (; i < nl; i++)
      {
         char a = p[i];
         char b = needle[i];
         if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
         if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
         if (a != b || p[i] == '\0')
            break;
      }
      if (i == nl)
         return 1;
   }
   return 0;
}

int aws_sts_parse_assume_response(const char *xml, aws_sts_credentials_t *out)
{
   if (!xml || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   /* XXE / entity defense: never resolve entities — reject a DTD or entity decl. */
   if (contains_ci(xml, "<!DOCTYPE") || contains_ci(xml, "<!ENTITY"))
      return -1;

   const char *end = xml + strlen(xml);

   /* Take the FIRST <Credentials> block; reject a trailing alternate. */
   const char *cred_open = strstr(xml, "<Credentials>");
   if (!cred_open)
      return -1;
   const char *cred_end = strstr(cred_open, "</Credentials>");
   if (!cred_end)
      return -1;
   /* No second <Credentials> anywhere (trailing alternate result object). */
   if (strstr(cred_end + strlen("</Credentials>"), "<Credentials>"))
      return -1;
   (void)end;

   if (extract_field(cred_open, cred_end, "AccessKeyId", out->access_key_id,
                     sizeof(out->access_key_id)) != 0 ||
       extract_field(cred_open, cred_end, "SecretAccessKey", out->secret_access_key,
                     sizeof(out->secret_access_key)) != 0 ||
       extract_field(cred_open, cred_end, "SessionToken", out->session_token,
                     sizeof(out->session_token)) != 0 ||
       extract_field(cred_open, cred_end, "Expiration", out->expiration, sizeof(out->expiration)) !=
           0)
      return -1;
   return 0;
}
