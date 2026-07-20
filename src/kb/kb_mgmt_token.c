#include "kb_mgmt_token.h"
#include "cJSON.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static char *enc(const unsigned char *p, size_t n)
{
   size_t z = (n + 2) / 3 * 4 + 1;
   char *o = calloc(1, z);
   if (!o)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < n; i += 3)
   {
      unsigned v = p[i] << 16;
      if (i + 1 < n)
         v |= p[i + 1] << 8;
      if (i + 2 < n)
         v |= p[i + 2];
      o[j++] = b64[(v >> 18) & 63];
      o[j++] = b64[(v >> 12) & 63];
      if (i + 1 < n)
         o[j++] = b64[(v >> 6) & 63];
      if (i + 2 < n)
         o[j++] = b64[v & 63];
   }
   return o;
}

char *kb_mgmt_token_mint(const char *pem, const char *kid, const char *issuer, const char *aud,
                         const char *sub, const char *cap, const char *cn, const char *jti,
                         long iat, long ttl)
{
   if (!pem || !kid || !*kid || !issuer || !*issuer || !aud || !*aud || !sub || !*sub || !cap ||
       !*cap || !cn || !*cn || !jti || !*jti || ttl <= 0 || ttl > 900)
      return NULL;
   cJSON *h = cJSON_CreateObject(), *p = cJSON_CreateObject();
   if (!h || !p)
   {
      cJSON_Delete(h);
      cJSON_Delete(p);
      return NULL;
   }
   cJSON_AddStringToObject(h, "alg", "RS256");
   cJSON_AddStringToObject(h, "typ", "JWT");
   cJSON_AddStringToObject(h, "kid", kid);
   cJSON_AddStringToObject(p, "iss", issuer);
   cJSON_AddStringToObject(p, "aud", aud);
   cJSON_AddStringToObject(p, "sub", sub);
   cJSON_AddStringToObject(p, "cap", cap);
   cJSON_AddStringToObject(p, "cert_cn", cn);
   cJSON_AddStringToObject(p, "jti", jti);
   cJSON_AddNumberToObject(p, "iat", (double)iat);
   cJSON_AddNumberToObject(p, "exp", (double)(iat + ttl));
   char *hr = cJSON_PrintUnformatted(h), *pr = cJSON_PrintUnformatted(p);
   cJSON_Delete(h);
   cJSON_Delete(p);
   if (!hr || !pr)
   {
      free(hr);
      free(pr);
      return NULL;
   }
   char *hs = enc((const unsigned char *)hr, strlen(hr)),
        *ps = enc((const unsigned char *)pr, strlen(pr));
   free(hr);
   free(pr);
   if (!hs || !ps)
   {
      free(hs);
      free(ps);
      return NULL;
   }
   size_t il = strlen(hs) + strlen(ps) + 2;
   char *input = malloc(il);
   if (!input)
   {
      free(hs);
      free(ps);
      return NULL;
   }
   snprintf(input, il, "%s.%s", hs, ps);
   free(hs);
   free(ps);
   BIO *bio = BIO_new_mem_buf(pem, -1);
   EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
   if (bio)
      BIO_free(bio);
   EVP_MD_CTX *md = key ? EVP_MD_CTX_new() : NULL;
   unsigned char sig[512];
   size_t sl = sizeof(sig);
   int ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
            EVP_DigestSignUpdate(md, input, strlen(input)) == 1 &&
            EVP_DigestSignFinal(md, sig, &sl) == 1;
   char *es = ok ? enc(sig, sl) : NULL;
   EVP_MD_CTX_free(md);
   EVP_PKEY_free(key);
   if (!es)
   {
      free(input);
      return NULL;
   }
   char *jwt = malloc(strlen(input) + strlen(es) + 2);
   if (jwt)
      sprintf(jwt, "%s.%s", input, es);
   free(input);
   free(es);
   return jwt;
}
