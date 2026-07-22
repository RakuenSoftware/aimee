#include "kb_mgmt_token.h"

#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JSON_INT_MAX  INT64_C(9007199254740991)
#define HEADER_MAX    1024u
#define PAYLOAD_MAX   4096u
#define SIGNATURE_MAX 1024u

typedef struct
{
   char *p;
   size_t cap;
   size_t n;
} writer_t;

static int bounded(const char *s, size_t cap, size_t min, size_t max, size_t *n)
{
   if (!s || !cap)
      return 0;
   size_t z = strnlen(s, cap);
   if (z == cap || z < min || z > max)
      return 0;
   /* Claims are fixed-capacity transport records, not C string bags. Requiring
    * the unused tail to be zero prevents an embedded NUL from silently
    * normalizing two distinct admitted tuples to the same signed claim. */
   for (size_t i = z + 1; i < cap; ++i)
      if (s[i] != '\0')
         return 0;
   if (n)
      *n = z;
   return 1;
}

static int control_free(const char *s, size_t cap, size_t min, size_t max)
{
   size_t n;
   if (!bounded(s, cap, min, max, &n))
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
         return 0;
   return 1;
}

static int token(const char *s, size_t cap, size_t min, size_t max)
{
   size_t n;
   if (!bounded(s, cap, min, max, &n))
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

static int lower_hex(const char *s, size_t cap, size_t min, size_t max)
{
   size_t n;
   if (!bounded(s, cap, min, max, &n))
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
      if (c == '%')
      {
         if (i + 2 >= n ||
             !((s[i + 1] == '2' && s[i + 2] == '5') || (s[i + 1] == '3' && s[i + 2] == 'A')))
            return 0;
         i += 2;
      }
   }
   return 1;
}

static int identity_key(const char *s)
{
   size_t n;
   if (!bounded(s, 577, 1, 576, &n))
      return 0;
   if (n == 5 && memcmp(s, "owner", 5) == 0)
      return 1;
   if (n > 5 && memcmp(s, "cert:", 5) == 0)
   {
      const char *sep = memchr(s + 5, ':', n - 5);
      return sep && identity_component(s + 5, (size_t)(sep - s - 5)) &&
             lower_hex(sep + 1, n - (size_t)(sep + 1 - s) + 1, 1, 79);
   }
   if (n > 5 && memcmp(s, "oidc:", 5) == 0)
   {
      const char *sep = memchr(s + 5, ':', n - 5);
      return sep && !memchr(sep + 1, ':', n - (size_t)(sep + 1 - s)) &&
             identity_component(s + 5, (size_t)(sep - s - 5)) &&
             identity_component(sep + 1, n - (size_t)(sep + 1 - s));
   }
   return 0;
}

static int put(writer_t *w, const char *s, size_t n)
{
   if (!w || n > w->cap - w->n)
      return 0;
   memcpy(w->p + w->n, s, n);
   w->n += n;
   return 1;
}

static int quoted(writer_t *w, const char *s, size_t cap)
{
   size_t n;
   if (!bounded(s, cap, 0, cap - 1, &n) || !put(w, "\"", 1))
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      if (s[i] == '"' || s[i] == '\\')
      {
         if (!put(w, "\\", 1))
            return 0;
      }
      if (!put(w, s + i, 1))
         return 0;
   }
   return put(w, "\"", 1);
}

static int member(writer_t *w, const char *prefix, const char *value, size_t cap)
{
   return put(w, prefix, strlen(prefix)) && quoted(w, value, cap);
}

static int uint_member(writer_t *w, const char *prefix, int64_t value)
{
   char number[32];
   int n = snprintf(number, sizeof(number), "%lld", (long long)value);
   return n > 0 && (size_t)n < sizeof(number) && put(w, prefix, strlen(prefix)) &&
          put(w, number, (size_t)n);
}

static size_t b64_size(size_t n)
{
   return (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0);
}

static int b64_encode(const unsigned char *in, size_t n, char *out, size_t cap, size_t *out_n)
{
   static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   size_t need = b64_size(n), o = 0;
   if (!out || !out_n || need > cap)
      return 0;
   for (size_t i = 0; i < n; i += 3)
   {
      size_t left = n - i;
      uint32_t v = (uint32_t)in[i] << 16;
      if (left > 1)
         v |= (uint32_t)in[i + 1] << 8;
      if (left > 2)
         v |= in[i + 2];
      out[o++] = a[(v >> 18) & 63];
      out[o++] = a[(v >> 12) & 63];
      if (left > 1)
         out[o++] = a[(v >> 6) & 63];
      if (left > 2)
         out[o++] = a[v & 63];
   }
   *out_n = o;
   return 1;
}

static int valid(const kb_mgmt_token_claims_t *c)
{
   return c && control_free(c->issuer, sizeof(c->issuer), 1, 255) &&
          token(c->audience, sizeof(c->audience), 1, 127) && identity_key(c->subject) &&
          c->team_id > 0 && c->team_id <= JSON_INT_MAX &&
          c->capability == KB_MGMT_TOKEN_CAP_REMOTE_WRITES &&
          token(c->jti, sizeof(c->jti), 16, 128) &&
          token(c->correlation_id, sizeof(c->correlation_id), 1, 128) &&
          lower_hex(c->request_sha256, sizeof(c->request_sha256), 64, 64) &&
          control_free(c->peer_issuer, sizeof(c->peer_issuer), 1, 511) &&
          lower_hex(c->peer_serial, sizeof(c->peer_serial), 1, 79) &&
          lower_hex(c->peer_fingerprint, sizeof(c->peer_fingerprint), 64, 64) &&
          token(c->kid, sizeof(c->kid), 1, 64) && c->issued_at >= 0 &&
          c->issued_at <= JSON_INT_MAX && c->expires_at > c->issued_at &&
          c->expires_at <= JSON_INT_MAX && c->expires_at - c->issued_at <= 90;
}

kb_mgmt_token_result_t kb_mgmt_token_build(const kb_mgmt_token_claims_t *c,
                                           kb_mgmt_token_sign_fn signer, void *signer_ctx,
                                           char *jwt_out, size_t jwt_cap, size_t *jwt_len)
{
   if (jwt_len)
      *jwt_len = 0;
   if (jwt_out && jwt_cap)
      jwt_out[0] = '\0';
   if (!jwt_out || !jwt_len || !signer || !valid(c))
      return KB_MGMT_TOKEN_INVALID;

   char header[HEADER_MAX + 1], payload[PAYLOAD_MAX + 1];
   writer_t h = {header, HEADER_MAX, 0}, p = {payload, PAYLOAD_MAX, 0};
   static const char header_prefix[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":";
   int ok =
       put(&h, header_prefix, sizeof(header_prefix) - 1) && quoted(&h, c->kid, sizeof(c->kid)) &&
       put(&h, "}", 1) && put(&p, "{\"v\":1", 6) &&
       member(&p, ",\"iss\":", c->issuer, sizeof(c->issuer)) &&
       member(&p, ",\"aud\":", c->audience, sizeof(c->audience)) &&
       member(&p, ",\"sub\":", c->subject, sizeof(c->subject)) &&
       uint_member(&p, ",\"team_id\":", c->team_id) &&
       member(&p, ",\"cap\":", "remote_writes", 14) &&
       member(&p, ",\"jti\":", c->jti, sizeof(c->jti)) &&
       member(&p, ",\"correlation_id\":", c->correlation_id, sizeof(c->correlation_id)) &&
       member(&p, ",\"request_sha256\":", c->request_sha256, sizeof(c->request_sha256)) &&
       member(&p, ",\"peer_issuer\":", c->peer_issuer, sizeof(c->peer_issuer)) &&
       member(&p, ",\"peer_serial\":", c->peer_serial, sizeof(c->peer_serial)) &&
       member(&p, ",\"peer_fingerprint\":", c->peer_fingerprint, sizeof(c->peer_fingerprint)) &&
       uint_member(&p, ",\"iat\":", c->issued_at) && uint_member(&p, ",\"exp\":", c->expires_at) &&
       put(&p, "}", 1);
   if (!ok)
      return KB_MGMT_TOKEN_INVALID;

   char signing[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t hn = 0, pn = 0;
   if (!b64_encode((const unsigned char *)header, h.n, signing, sizeof(signing), &hn) ||
       hn + 1 > sizeof(signing) ||
       !b64_encode((const unsigned char *)payload, p.n, signing + hn + 1, sizeof(signing) - hn - 1,
                   &pn))
      return KB_MGMT_TOKEN_INVALID;
   signing[hn] = '.';
   size_t input_n = hn + 1 + pn;
   size_t max_wire = input_n + 1 + b64_size(SIGNATURE_MAX);
   if (max_wire > KB_MGMT_TOKEN_WIRE_MAX)
      return KB_MGMT_TOKEN_INVALID;
   if (jwt_cap <= max_wire)
      return KB_MGMT_TOKEN_OUTPUT_TOO_SMALL;

   unsigned char signature[SIGNATURE_MAX];
   memset(signature, 0, sizeof(signature));
   size_t signature_n = 0;
   int signed_ok = signer(signer_ctx, (const unsigned char *)signing, input_n, signature,
                          sizeof(signature), &signature_n);
   if (!signed_ok || signature_n < 256 || signature_n > sizeof(signature))
   {
      OPENSSL_cleanse(signature, sizeof(signature));
      OPENSSL_cleanse(signing, sizeof(signing));
      return KB_MGMT_TOKEN_SIGN_UNAVAILABLE;
   }

   memcpy(jwt_out, signing, input_n);
   jwt_out[input_n] = '.';
   size_t encoded_sig_n = 0;
   ok = b64_encode(signature, signature_n, jwt_out + input_n + 1, jwt_cap - input_n - 2,
                   &encoded_sig_n);
   OPENSSL_cleanse(signature, sizeof(signature));
   OPENSSL_cleanse(signing, sizeof(signing));
   if (!ok)
   {
      jwt_out[0] = '\0';
      return KB_MGMT_TOKEN_OUTPUT_TOO_SMALL;
   }
   *jwt_len = input_n + 1 + encoded_sig_n;
   jwt_out[*jwt_len] = '\0';
   return KB_MGMT_TOKEN_OK;
}
