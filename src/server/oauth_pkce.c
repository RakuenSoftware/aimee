/*
 * oauth_pkce.c: PKCE (RFC 7636) primitives.
 *
 * See oauth_pkce.h for the module-level contract.
 */
#include "oauth_pkce.h"
#include "platform_random.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#ifndef AIMEE_WINDOWS
#include <openssl/sha.h>
#endif

static const char BASE64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* RFC 7636 §4.1: unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~" */
static const char PKCE_UNRESERVED[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

int oauth_pkce_base64url_encode(const unsigned char *in, size_t inlen, char *out, size_t outlen)
{
   if (!out || outlen == 0)
      return -1;
   if (!in && inlen > 0)
      return -1;

   /* base64url no-pad output length = ceil(inlen * 4 / 3). */
   size_t needed = (inlen / 3) * 4;
   size_t rem = inlen % 3;
   if (rem == 1)
      needed += 2;
   else if (rem == 2)
      needed += 3;
   if (needed + 1 > outlen)
      return -1;

   size_t i = 0, j = 0;
   while (i + 3 <= inlen)
   {
      unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | (unsigned)in[i + 2];
      out[j++] = BASE64URL_ALPHABET[(v >> 18) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[(v >> 12) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[(v >> 6) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[v & 0x3F];
      i += 3;
   }
   if (rem == 1)
   {
      unsigned v = (unsigned)in[i] << 16;
      out[j++] = BASE64URL_ALPHABET[(v >> 18) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[(v >> 12) & 0x3F];
   }
   else if (rem == 2)
   {
      unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
      out[j++] = BASE64URL_ALPHABET[(v >> 18) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[(v >> 12) & 0x3F];
      out[j++] = BASE64URL_ALPHABET[(v >> 6) & 0x3F];
   }
   out[j] = '\0';
   return 0;
}

int oauth_pkce_generate_verifier(char *out, size_t len)
{
   if (!out)
      return -1;
   if (len < OAUTH_PKCE_VERIFIER_MIN || len > OAUTH_PKCE_VERIFIER_MAX)
      return -1;

   /* Use rejection sampling over a random byte stream so every output
    * character is drawn uniformly from the 66-character unreserved set.
    * Oversampling by 2x keeps the expected loop count well under len. */
   const size_t alphabet_len = sizeof(PKCE_UNRESERVED) - 1; /* 66 */
   size_t produced = 0;
   while (produced < len)
   {
      unsigned char buf[256];
      size_t want = len - produced;
      if (want > sizeof(buf) / 2)
         want = sizeof(buf) / 2;
      size_t draw = want * 2;
      if (platform_random_bytes(buf, draw) != 0)
         return -1;
      for (size_t k = 0; k < draw && produced < len; k++)
      {
         /* Accept bytes <= the largest multiple of alphabet_len below 256
          * (3 * 66 = 198). Rejecting the tail keeps the distribution flat. */
         if (buf[k] < 198)
            out[produced++] = PKCE_UNRESERVED[buf[k] % alphabet_len];
      }
   }
   out[len] = '\0';
   return 0;
}

int oauth_pkce_s256_challenge(const char *verifier, char *out, size_t outlen)
{
   if (!verifier || !out)
      return -1;
   size_t vlen = strlen(verifier);
   if (vlen < OAUTH_PKCE_VERIFIER_MIN || vlen > OAUTH_PKCE_VERIFIER_MAX)
      return -1;
   if (outlen < OAUTH_PKCE_CHALLENGE_LEN + 1)
      return -1;

#ifdef AIMEE_WINDOWS
   return -1;
#else
   unsigned char digest[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)verifier, vlen, digest);
   return oauth_pkce_base64url_encode(digest, sizeof(digest), out, outlen);
#endif
}

/* Percent-encode per RFC 3986 into |out| at offset |*pos| (capacity |outlen|).
 * Returns 0 on success, -1 if the buffer would overflow. */
static int append_encoded(char *out, size_t outlen, size_t *pos, const char *s)
{
   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      int unreserved = (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~');
      size_t need = unreserved ? 1 : 3;
      if (*pos + need + 1 > outlen)
         return -1;
      if (unreserved)
      {
         out[(*pos)++] = (char)c;
      }
      else
      {
         out[(*pos)++] = '%';
         out[(*pos)++] = "0123456789ABCDEF"[(c >> 4) & 0xF];
         out[(*pos)++] = "0123456789ABCDEF"[c & 0xF];
      }
   }
   return 0;
}

static int append_literal(char *out, size_t outlen, size_t *pos, const char *s)
{
   size_t n = strlen(s);
   if (*pos + n + 1 > outlen)
      return -1;
   memcpy(out + *pos, s, n);
   *pos += n;
   return 0;
}

int oauth_pkce_build_auth_url(const oauth_pkce_auth_request_t *req, char *out, size_t outlen)
{
   if (!req || !out || outlen == 0)
      return -1;
   if (!req->authorize_url || !*req->authorize_url)
      return -1;
   if (!req->client_id || !*req->client_id)
      return -1;
   if (!req->redirect_uri || !*req->redirect_uri)
      return -1;
   if (!req->code_challenge || !*req->code_challenge)
      return -1;

   size_t pos = 0;
   if (append_literal(out, outlen, &pos, req->authorize_url) != 0)
      return -1;

   /* Preserve any existing query string by switching the separator from
    * "?" to "&" when authorize_url already contains "?". */
   const char *sep = strchr(req->authorize_url, '?') ? "&" : "?";

   if (append_literal(out, outlen, &pos, sep) != 0)
      return -1;
   if (append_literal(out, outlen, &pos, "response_type=code&client_id=") != 0)
      return -1;
   if (append_encoded(out, outlen, &pos, req->client_id) != 0)
      return -1;
   if (append_literal(out, outlen, &pos, "&redirect_uri=") != 0)
      return -1;
   if (append_encoded(out, outlen, &pos, req->redirect_uri) != 0)
      return -1;
   if (append_literal(out, outlen, &pos, "&code_challenge=") != 0)
      return -1;
   if (append_encoded(out, outlen, &pos, req->code_challenge) != 0)
      return -1;
   if (append_literal(out, outlen, &pos, "&code_challenge_method=S256") != 0)
      return -1;

   if (req->scope && *req->scope)
   {
      if (append_literal(out, outlen, &pos, "&scope=") != 0)
         return -1;
      if (append_encoded(out, outlen, &pos, req->scope) != 0)
         return -1;
   }
   if (req->state && *req->state)
   {
      if (append_literal(out, outlen, &pos, "&state=") != 0)
         return -1;
      if (append_encoded(out, outlen, &pos, req->state) != 0)
         return -1;
   }
   if (req->nonce && *req->nonce)
   {
      if (append_literal(out, outlen, &pos, "&nonce=") != 0)
         return -1;
      if (append_encoded(out, outlen, &pos, req->nonce) != 0)
         return -1;
   }

   out[pos] = '\0';
   return (int)pos;
}
