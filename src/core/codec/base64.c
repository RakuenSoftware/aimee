#include <aimee/core/codec/base64.h>
#include <stdint.h>

static const char STANDARD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t aimee_codec_base64_capacity(size_t length, aimee_base64_variant_t variant)
{
   if (variant != AIMEE_BASE64_STANDARD && variant != AIMEE_BASE64_URL)
      return 0;
   size_t groups = length / 3, rem = length % 3;
   size_t tail = rem ? (variant == AIMEE_BASE64_URL ? rem + 1 : 4) : 0;
   if (groups > (SIZE_MAX - 1 - tail) / 4)
      return 0;
   return groups * 4 + tail + 1;
}

int aimee_codec_base64_encode(const unsigned char *in, size_t length, char *out, size_t capacity,
                              aimee_base64_variant_t variant, size_t *written)
{
   if (written)
      *written = 0;
   size_t need = aimee_codec_base64_capacity(length, variant);
   if (!written || !out || !need || capacity < need || (!in && length))
   {
      if (out && capacity)
         out[0] = '\0';
      return 0;
   }
   const char *alphabet = variant == AIMEE_BASE64_URL ? URL : STANDARD;
   size_t o = 0;
   for (size_t i = 0; i < length;)
   {
      size_t left = length - i;
      uint32_t value = (uint32_t)in[i++] << 16;
      if (left > 1)
         value |= (uint32_t)in[i++] << 8;
      if (left > 2)
         value |= in[i++];
      out[o++] = alphabet[(value >> 18) & 63];
      out[o++] = alphabet[(value >> 12) & 63];
      if (left > 1)
         out[o++] = alphabet[(value >> 6) & 63];
      else if (variant == AIMEE_BASE64_STANDARD)
         out[o++] = '=';
      if (left > 2)
         out[o++] = alphabet[value & 63];
      else if (variant == AIMEE_BASE64_STANDARD)
         out[o++] = '=';
   }
   out[o] = '\0';
   *written = o;
   return 1;
}

static int digit(unsigned char c, int url)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == (url ? '-' : '+'))
      return 62;
   if (c == (url ? '_' : '/'))
      return 63;
   return -1;
}

int aimee_codec_base64_decode(const char *in, size_t length, unsigned char *out, size_t capacity,
                              aimee_base64_profile_t profile, size_t *written)
{
   if (written)
      *written = 0;
   if (!in || !out || !written || profile < AIMEE_BASE64_STANDARD_CANONICAL ||
       profile > AIMEE_BASE64_URL_COMPAT)
      return 0;
   int url = profile == AIMEE_BASE64_URL_CANONICAL || profile == AIMEE_BASE64_URL_COMPAT;
   int strict = profile == AIMEE_BASE64_STANDARD_CANONICAL || profile == AIMEE_BASE64_URL_CANONICAL;
   size_t data = length;
   if (profile == AIMEE_BASE64_STANDARD_CANONICAL)
   {
      if (length % 4)
         return 0;
      if (data && in[data - 1] == '=')
         --data;
      if (data && in[data - 1] == '=')
         --data;
   }
   else if (profile == AIMEE_BASE64_URL_CANONICAL && length % 4 == 1)
      return 0;
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t o = 0;
   for (size_t i = 0; i < data; ++i)
   {
      unsigned char c = (unsigned char)in[i];
      if (profile == AIMEE_BASE64_STANDARD_TEXT_COMPAT)
      {
         if (c == '=')
            break;
         if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
      }
      int value = digit(c, url);
      if (value < 0)
         return 0;
      acc = (acc << 6) | (uint32_t)value;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o == capacity)
            return 0;
         out[o++] = (unsigned char)(acc >> bits);
         acc &= bits ? ((UINT32_C(1) << bits) - 1) : 0;
      }
   }
   if (strict && acc)
      return 0;
   *written = o;
   return 1;
}
