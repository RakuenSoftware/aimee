/* auth_oidc.c: BYO OIDC / JWT verifier for aimee-kb. See kb_auth_oidc.h.
 * (distributed-mode-auth proposal, Phase 3.)
 *
 * Verifies a compact JWS bearer (RS256) against an operator-configured JWKS:
 *   1. split header.payload.signature, base64url-decode header + payload
 *   2. require alg == "RS256" (reject "none"/HMAC — alg-confusion defense)
 *   3. select the JWKS RSA key by header "kid"; build a public EVP_PKEY from
 *      its (n, e) via EVP_PKEY_fromdata (OpenSSL 3)
 *   4. verify the RSASSA-PKCS1-v1_5 + SHA-256 signature over "header.payload"
 *   5. check exp > now and (when configured) iss / aud
 *   6. map claims -> scope (verify-then-trust: only after all checks pass)
 *
 * Signature verification uses public keys only, so a kb compromise cannot forge
 * another tenant's identity. */
#include "kb_auth_oidc.h"

#include "cJSON.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- base64url decode (no padding), tolerant of stray chars --- */
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

/* Decode `in_len` base64url chars into out[out_cap]; returns byte count, or
 * (size_t)-1 if it does not fit or an invalid char is encountered. */
static size_t b64url_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
   size_t o = 0;
   unsigned int buf = 0;
   int bits = 0;
   for (size_t i = 0; i < in_len; i++)
   {
      int v = b64url_val(in[i]);
      if (v < 0)
         return (size_t)-1; /* strict: no whitespace / invalid chars in a JWS */
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

/* Decode a base64url segment into a NUL-terminated string buffer. Returns 0 on
 * success, -1 on overflow/invalid. */
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

/* --- JWKS key lookup: build a public RSA EVP_PKEY for `kid` --- */

/* Returns a newly-allocated EVP_PKEY (caller frees) for the RSA key in
 * jwks_json matching `kid` (or the sole RSA key when kid is NULL/empty and
 * exactly one key is present), or NULL if not found / malformed. */
static EVP_PKEY *jwks_pubkey_for_kid(const char *jwks_json, const char *kid)
{
   cJSON *root = cJSON_Parse(jwks_json);
   if (!root)
      return NULL;
   EVP_PKEY *pkey = NULL;
   cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
   if (!cJSON_IsArray(keys))
   {
      cJSON_Delete(root);
      return NULL;
   }

   cJSON *chosen = NULL;
   int rsa_count = 0;
   cJSON *k = NULL;
   cJSON_ArrayForEach(k, keys)
   {
      cJSON *kty = cJSON_GetObjectItemCaseSensitive(k, "kty");
      if (!cJSON_IsString(kty) || strcmp(kty->valuestring, "RSA") != 0)
         continue;
      rsa_count++;
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
         chosen = k; /* remember the first; only valid if it is the only one */
      }
   }
   /* With no kid we only accept an unambiguous single-key JWKS. */
   if (!chosen || ((!kid || !kid[0]) && rsa_count != 1))
   {
      cJSON_Delete(root);
      return NULL;
   }

   cJSON *nj = cJSON_GetObjectItemCaseSensitive(chosen, "n");
   cJSON *ej = cJSON_GetObjectItemCaseSensitive(chosen, "e");
   if (!cJSON_IsString(nj) || !cJSON_IsString(ej))
   {
      cJSON_Delete(root);
      return NULL;
   }

   unsigned char nbuf[1024], ebuf[16];
   size_t nlen = b64url_decode(nj->valuestring, strlen(nj->valuestring), nbuf, sizeof(nbuf));
   size_t elen = b64url_decode(ej->valuestring, strlen(ej->valuestring), ebuf, sizeof(ebuf));
   if (nlen == (size_t)-1 || elen == (size_t)-1 || nlen == 0 || elen == 0)
   {
      cJSON_Delete(root);
      return NULL;
   }

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
   cJSON_Delete(root);
   return pkey;
}

/* --- RS256 signature verification over "header.payload" --- */
static int rs256_verify(EVP_PKEY *pkey, const char *signing_input, size_t signing_len,
                        const unsigned char *sig, size_t siglen)
{
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   if (!md)
      return 0;
   int ok = 0;
   if (EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pkey) == 1 &&
       EVP_DigestVerifyUpdate(md, signing_input, signing_len) == 1 &&
       EVP_DigestVerifyFinal(md, sig, siglen) == 1)
      ok = 1;
   EVP_MD_CTX_free(md);
   return ok;
}

/* --- aud match: equals cfg->audience as a string, or contained in the array --- */
static int aud_matches(const cJSON *payload, const char *audience)
{
   if (!audience || !audience[0])
      return 1; /* unchecked */
   const cJSON *aud = cJSON_GetObjectItemCaseSensitive(payload, "aud");
   if (cJSON_IsString(aud))
      return strcmp(aud->valuestring, audience) == 0;
   if (cJSON_IsArray(aud))
   {
      const cJSON *e = NULL;
      cJSON_ArrayForEach(e, aud)
      {
         if (cJSON_IsString(e) && strcmp(e->valuestring, audience) == 0)
            return 1;
      }
   }
   return 0;
}

int kb_oidc_verify_jwt(const char *jwt, const kb_oidc_config_t *cfg, long now,
                       kb_verify_result_t *out)
{
   if (!jwt || !cfg || !out || !cfg->jwks_json[0])
      return 0;

   /* Split into three dot-separated segments. */
   const char *dot1 = strchr(jwt, '.');
   if (!dot1)
      return 0;
   const char *dot2 = strchr(dot1 + 1, '.');
   if (!dot2)
      return 0;
   const char *hdr_b64 = jwt;
   size_t hdr_len = (size_t)(dot1 - jwt);
   const char *pl_b64 = dot1 + 1;
   size_t pl_len = (size_t)(dot2 - pl_b64);
   const char *sig_b64 = dot2 + 1;
   size_t sig_len = strlen(sig_b64);
   if (hdr_len == 0 || pl_len == 0 || sig_len == 0 || strchr(sig_b64, '.'))
      return 0; /* a JWE (5 segments) or empty parts are not accepted */

   /* Decode + parse the header; pin alg == RS256. */
   char hdr_json[1024];
   if (b64url_decode_str(hdr_b64, hdr_len, hdr_json, sizeof(hdr_json)) != 0)
      return 0;
   cJSON *hdr = cJSON_Parse(hdr_json);
   if (!hdr)
      return 0;
   cJSON *alg = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
   if (!cJSON_IsString(alg) || strcmp(alg->valuestring, "RS256") != 0)
   {
      cJSON_Delete(hdr);
      return 0;
   }
   cJSON *kidj = cJSON_GetObjectItemCaseSensitive(hdr, "kid");
   char kid[256] = "";
   if (cJSON_IsString(kidj))
      snprintf(kid, sizeof(kid), "%s", kidj->valuestring);
   cJSON_Delete(hdr);

   /* Resolve the verifying key. */
   EVP_PKEY *pkey = jwks_pubkey_for_kid(cfg->jwks_json, kid);
   if (!pkey)
      return 0;

   /* Verify the signature over the raw "header.payload" bytes. */
   unsigned char sig[1024];
   size_t siglen = b64url_decode(sig_b64, sig_len, sig, sizeof(sig));
   int verified = 0;
   if (siglen != (size_t)-1 && siglen > 0)
      verified = rs256_verify(pkey, hdr_b64, (size_t)(dot2 - hdr_b64), sig, siglen);
   EVP_PKEY_free(pkey);
   if (!verified)
      return 0;

   /* Signature is good — now decode + validate the payload claims. */
   char *pl_json = malloc(pl_len + 1);
   if (!pl_json)
      return 0;
   if (b64url_decode_str(pl_b64, pl_len, pl_json, pl_len + 1) != 0)
   {
      free(pl_json);
      return 0;
   }
   cJSON *pl = cJSON_Parse(pl_json);
   free(pl_json);
   if (!pl)
      return 0;

   int ok = 0;
   do
   {
      /* exp: required, must be in the future (60s clock-skew leeway). */
      cJSON *exp = cJSON_GetObjectItemCaseSensitive(pl, "exp");
      if (!cJSON_IsNumber(exp))
         break;
      long expiry = (long)exp->valuedouble;
      if (expiry + 60 < now)
         break;

      /* iat ceiling (P1 I9): a hard server-side cap on accepted token age,
       * enforced regardless of the token's own exp, so a leaked/misconfigured
       * long-lived token cannot outlive the ceiling. iat is REQUIRED and numeric;
       * a missing/malformed iat, a future iat (beyond skew), or an age over the
       * ceiling all reject. The future check runs BEFORE any subtraction so a
       * now < iat can never underflow into a huge age that would pass the cap. */
      {
         const long skew = 60; /* symmetric clock-skew allowance (matches exp) */
         long max_age = cfg->max_token_age_secs > 0 ? cfg->max_token_age_secs : 900;
         cJSON *iat = cJSON_GetObjectItemCaseSensitive(pl, "iat");
         if (!cJSON_IsNumber(iat))
            break; /* missing or non-numeric iat */
         long iat_v = (long)iat->valuedouble;
         if (iat_v < 0)
            break; /* malformed */
         if (iat_v > now + skew)
            break; /* issued in the future beyond skew — reject before subtracting */
         if (now - iat_v > max_age)
            break; /* over-age: older than the ceiling, regardless of exp */
      }

      /* iss / aud when configured. */
      if (cfg->issuer[0])
      {
         cJSON *iss = cJSON_GetObjectItemCaseSensitive(pl, "iss");
         if (!cJSON_IsString(iss) || strcmp(iss->valuestring, cfg->issuer) != 0)
            break;
      }
      if (!aud_matches(pl, cfg->audience))
         break;

      /* All checks passed — derive identity from the verified payload only. */
      memset(out, 0, sizeof(*out));
      out->expiry = expiry;
      cJSON *sub = cJSON_GetObjectItemCaseSensitive(pl, "sub");
      snprintf(out->subject, sizeof(out->subject), "%s",
               cJSON_IsString(sub) ? sub->valuestring : "");
      if (cfg->scope_claim[0])
      {
         cJSON *sc = cJSON_GetObjectItemCaseSensitive(pl, cfg->scope_claim);
         if (cJSON_IsString(sc) && sc->valuestring[0])
         {
            snprintf(out->scope_kind, sizeof(out->scope_kind), "%s", cfg->scope_kind);
            snprintf(out->scope_id, sizeof(out->scope_id), "%s", sc->valuestring);
         }
      }
      ok = 1;
   } while (0);

   cJSON_Delete(pl);
   return ok;
}

/* --- seam registration --- */

static kb_oidc_config_t g_oidc_cfg;
static int g_oidc_cfg_set = 0;
static pthread_mutex_t g_oidc_lock = PTHREAD_MUTEX_INITIALIZER;
static kb_oidc_fleet_resolver_fn g_fleet_resolver = NULL;

#include <time.h>

void kb_oidc_set_fleet_resolver(kb_oidc_fleet_resolver_fn fn)
{
   pthread_mutex_lock(&g_oidc_lock);
   g_fleet_resolver = fn;
   pthread_mutex_unlock(&g_oidc_lock);
}

int kb_oidc_id_token_nonce(const char *jwt, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!jwt)
      return -1;
   const char *dot1 = strchr(jwt, '.');
   if (!dot1)
      return -1;
   const char *dot2 = strchr(dot1 + 1, '.');
   if (!dot2)
      return -1;
   size_t pl_len = (size_t)(dot2 - (dot1 + 1));
   if (pl_len == 0)
      return -1;
   char *pl_json = malloc(pl_len + 1);
   if (!pl_json)
      return -1;
   int rc = -1;
   if (b64url_decode_str(dot1 + 1, pl_len, pl_json, pl_len + 1) == 0)
   {
      cJSON *pl = cJSON_Parse(pl_json);
      cJSON *n = pl ? cJSON_GetObjectItemCaseSensitive(pl, "nonce") : NULL;
      if (cJSON_IsString(n) && n->valuestring && n->valuestring[0] && strlen(n->valuestring) < cap)
      {
         snprintf(out, cap, "%s", n->valuestring);
         rc = 0;
      }
      cJSON_Delete(pl);
   }
   free(pl_json);
   if (rc != 0)
      out[0] = '\0';
   return rc;
}

int kb_oidc_configured_issuer(char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   pthread_mutex_lock(&g_oidc_lock);
   int ok = (g_oidc_cfg_set && g_oidc_cfg.issuer[0]);
   if (ok)
      snprintf(out, cap, "%s", g_oidc_cfg.issuer);
   pthread_mutex_unlock(&g_oidc_lock);
   return ok ? 0 : -1;
}

/* Resolve the effective verification config: the registered one, with a
 * fleet-supplied JWKS preferred over the per-instance file. Returns 0 when no
 * config is registered. Shared by the bearer verifier and the login flow so the
 * two cannot end up trusting different key sets.
 *
 * Contract: the CALLER owns *cfg (a stack copy); this fills it and retains
 * nothing. The registered config is read under g_oidc_lock and the lock is
 * released before the fleet resolver runs, so a resolver that blocks on I/O
 * cannot stall a concurrent verification. The result is a snapshot to be used
 * immediately and never cached — callers may mutate their copy (the login flow
 * overrides the audience) without affecting the registered config. */
static int effective_config(kb_oidc_config_t *cfg)
{
   kb_oidc_fleet_resolver_fn fleet;
   pthread_mutex_lock(&g_oidc_lock);
   if (!g_oidc_cfg_set)
   {
      pthread_mutex_unlock(&g_oidc_lock);
      return 0;
   }
   *cfg = g_oidc_cfg;
   fleet = g_fleet_resolver;
   pthread_mutex_unlock(&g_oidc_lock);

   /* Fleet-wide JWKS (I10): when a resolver is registered and an issuer is
    * configured, prefer the shared Postgres key set so the fleet agrees on trusted
    * keys and IdP rotation converges; the file cfg->jwks_json is the fallback used
    * only when the resolver reports no fleet keys. */
   if (fleet && cfg->issuer[0])
   {
      char fleet_jwks[sizeof(cfg->jwks_json)];
      if (fleet(cfg->issuer, fleet_jwks, sizeof(fleet_jwks)) == 0 && fleet_jwks[0])
         snprintf(cfg->jwks_json, sizeof(cfg->jwks_json), "%s", fleet_jwks);
   }
   return 1;
}

int kb_oidc_service_mode(void)
{
   kb_oidc_config_t cfg;
   int configured = effective_config(&cfg);
   const char *requested = getenv("AIMEE_KB_OIDC_JWKS_FILE");
   if (!configured)
      return requested && requested[0] ? -1 : 0;
   return cfg.issuer[0] && cfg.audience[0] ? 1 : -1;
}

/* The ordinary OIDC verifier intentionally accepts provider-specific extra
 * protected-header fields. For the service connection we add one exact rule:
 * there must be exactly one string typ and it must be at+jwt. */
static int service_token_type_pinned(const char *jwt)
{
   if (!jwt)
      return 0;
   const char *dot = strchr(jwt, '.');
   if (!dot || dot == jwt)
      return 0;
   char header_json[1024];
   if (b64url_decode_str(jwt, (size_t)(dot - jwt), header_json, sizeof(header_json)) != 0)
      return 0;
   cJSON *header = cJSON_Parse(header_json);
   if (!cJSON_IsObject(header))
   {
      cJSON_Delete(header);
      return 0;
   }
   const cJSON *member = NULL;
   const cJSON *typ = NULL;
   int count = 0;
   cJSON_ArrayForEach(member, header)
   {
      if (member->string && strcmp(member->string, "typ") == 0)
      {
         typ = member;
         count++;
      }
   }
   int ok = count == 1 && cJSON_IsString(typ) && strcmp(typ->valuestring, "at+jwt") == 0;
   cJSON_Delete(header);
   return ok;
}

int kb_oidc_verify_service_token(const char *jwt, long now, kb_verify_result_t *out)
{
   kb_oidc_config_t cfg;
   if (!jwt || !out || now < 0 || !effective_config(&cfg) || !cfg.issuer[0] || !cfg.audience[0] ||
       !service_token_type_pinned(jwt))
      return 0;
   if (!kb_oidc_verify_jwt(jwt, &cfg, now, out) || !out->subject[0])
   {
      memset(out, 0, sizeof(*out));
      return 0;
   }
   return 1;
}

int kb_oidc_verify_id_token(const char *jwt, const char *expected_audience, long now,
                            kb_verify_result_t *out)
{
   kb_oidc_config_t cfg;
   if (!jwt || !expected_audience || !expected_audience[0] || !out)
      return 0;
   if (!effective_config(&cfg))
      return 0;
   /* THE AUDIENCE IS OVERRIDDEN, and that is the whole reason this exists rather
    * than the login calling the bearer verifier.
    *
    * AIMEE_KB_OIDC_AUDIENCE names kb as a RESOURCE SERVER — the audience an
    * access token carries when a client presents it to kb. An ID TOKEN is a
    * different thing: OIDC Core §3.1.3.7 step 3 requires its "aud" to be the
    * relying party's client_id. Verifying an id_token against the resource-server
    * audience would reject every real IdP's token, and the tempting fix — telling
    * operators to set AIMEE_KB_OIDC_AUDIENCE to the client_id — would quietly
    * make kb accept id_tokens as bearers on its own API.
    *
    * Empty is refused above rather than treated as "unchecked": for a bearer,
    * skipping the audience is a documented operator choice, but for an id_token
    * it would accept one minted for a different relying party of the same IdP. */
   snprintf(cfg.audience, sizeof(cfg.audience), "%s", expected_audience);
   return kb_oidc_verify_jwt(jwt, &cfg, now, out);
}

static int oidc_verify_fn(const char *presented, const char *configured, kb_verify_result_t *out,
                          void *ctx)
{
   (void)configured;
   (void)ctx;
   kb_oidc_config_t cfg;
   if (!effective_config(&cfg))
      return 0;
   return kb_oidc_verify_jwt(presented, &cfg, (long)time(NULL), out);
}

int kb_oidc_verifier_register(const kb_oidc_config_t *cfg)
{
   if (!cfg || !cfg->jwks_json[0])
      return -1;
   pthread_mutex_lock(&g_oidc_lock);
   g_oidc_cfg = *cfg;
   g_oidc_cfg_set = 1;
   pthread_mutex_unlock(&g_oidc_lock);
   /* Register (or, on a config reload / post-reset re-register, replace-by-name)
    * the verifier fn in the seam. kb_verifier_register dedupes by name, so this
    * is idempotent and survives a kb_verifier_reset that dropped the entry. */
   return kb_verifier_register("oidc", oidc_verify_fn, NULL);
}

/* --- configuration from a JWKS file (+ claim policy) --- */

int kb_oidc_register_from_file(const char *jwks_file, const char *issuer, const char *audience,
                               const char *scope_claim, const char *scope_kind)
{
   if (!jwks_file || !jwks_file[0])
      return -1;

   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   if (issuer)
      snprintf(cfg.issuer, sizeof(cfg.issuer), "%s", issuer);
   if (audience)
      snprintf(cfg.audience, sizeof(cfg.audience), "%s", audience);
   if (scope_claim)
      snprintf(cfg.scope_claim, sizeof(cfg.scope_claim), "%s", scope_claim);
   if (scope_kind)
      snprintf(cfg.scope_kind, sizeof(cfg.scope_kind), "%s", scope_kind);

   /* Optional hard token-age ceiling (P1 I9); default 900s (15 min) when unset or
    * non-positive. */
   const char *max_age_env = getenv("AIMEE_KB_OIDC_MAX_TOKEN_AGE");
   if (max_age_env && max_age_env[0])
   {
      long v = strtol(max_age_env, NULL, 10);
      if (v > 0)
         cfg.max_token_age_secs = v;
   }

   /* Read the JWKS document into the (bounded) config buffer. The file must fit
    * with room for the NUL; an empty, oversized, or unreadable file is a config
    * error. We read up to cap-1 bytes, then probe one more byte with fgetc: if a
    * byte remains the file is too large. (This is robust where a bare feof()
    * check is not — feof is only set after a read actually hits end-of-file.) */
   FILE *f = fopen(jwks_file, "rb");
   if (!f)
      return -1;
   size_t n = fread(cfg.jwks_json, 1, sizeof(cfg.jwks_json) - 1, f);
   int too_large = (fgetc(f) != EOF); /* an extra byte remained */
   fclose(f);
   if (n == 0 || too_large)
      return -1;
   cfg.jwks_json[n] = '\0';

   return kb_oidc_verifier_register(&cfg);
}

int kb_oidc_register_from_env(void)
{
   const char *jwks_file = getenv("AIMEE_KB_OIDC_JWKS_FILE");
   if (!jwks_file || !jwks_file[0])
      return 0; /* OIDC disabled — owner kb-token auth unchanged */
   return kb_oidc_register_from_file(
       jwks_file, getenv("AIMEE_KB_OIDC_ISSUER"), getenv("AIMEE_KB_OIDC_AUDIENCE"),
       getenv("AIMEE_KB_OIDC_SCOPE_CLAIM"), getenv("AIMEE_KB_OIDC_SCOPE_KIND"));
}
