#include <aimee/core/codec/base64.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void vectors(void)
{
   static const char *raw[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
   static const char *standard[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
   static const char *url[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
   for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i)
   {
      for (int variant = AIMEE_BASE64_STANDARD; variant <= AIMEE_BASE64_URL; ++variant)
      {
         char encoded[32];
         unsigned char decoded[32];
         size_t n = 99, d = 99;
         const char *expected = variant == AIMEE_BASE64_URL ? url[i] : standard[i];
         assert(aimee_codec_base64_capacity(strlen(raw[i]), variant) == strlen(expected) + 1);
         assert(aimee_codec_base64_encode((const unsigned char *)raw[i], strlen(raw[i]), encoded,
                                          sizeof(encoded), variant, &n));
         assert(n == strlen(expected) && !strcmp(encoded, expected));
         assert(aimee_codec_base64_decode(encoded, n, decoded, sizeof(decoded),
                                          variant == AIMEE_BASE64_URL
                                              ? AIMEE_BASE64_URL_CANONICAL
                                              : AIMEE_BASE64_STANDARD_CANONICAL,
                                          &d));
         assert(d == strlen(raw[i]) && !memcmp(decoded, raw[i], d));
      }
   }
}

static void refuses(const char *text, aimee_base64_profile_t profile)
{
   unsigned char output[64];
   size_t n = 99;
   assert(!aimee_codec_base64_decode(text, strlen(text), output, sizeof(output), profile, &n));
   assert(n == 0);
}

static void malformed(void)
{
   static const char *std_bad[] = {
       "A",    "AA",   "AAA",   "=AAA",   "A===",     "====", "Zg=", "Zg===",
       "Zh==", "Zm9=", " Zg==", "Zg==\n", "Zg==junk", "-w==", "_w=="};
   static const char *url_bad[] = {"A",   "AAAAA", "Zg=", "Zg==", "Zh", "Zm9",
                                   " Zg", "Zg\n",  "Zg!", "+w",   "/w"};
   for (size_t i = 0; i < sizeof(std_bad) / sizeof(std_bad[0]); ++i)
      refuses(std_bad[i], AIMEE_BASE64_STANDARD_CANONICAL);
   for (size_t i = 0; i < sizeof(url_bad) / sizeof(url_bad[0]); ++i)
      refuses(url_bad[i], AIMEE_BASE64_URL_CANONICAL);
   unsigned char output[4];
   size_t n = 0;
   assert(aimee_codec_base64_decode(" Z h =ignored", 13, output, sizeof(output),
                                    AIMEE_BASE64_STANDARD_TEXT_COMPAT, &n) &&
          n == 1 && output[0] == 'f');
   assert(aimee_codec_base64_decode("Zh", 2, output, sizeof(output), AIMEE_BASE64_URL_COMPAT, &n) &&
          n == 1 && output[0] == 'f');
   assert(aimee_codec_base64_decode("A", 1, output, sizeof(output), AIMEE_BASE64_URL_COMPAT, &n) &&
          n == 0);
   refuses("Zg=", AIMEE_BASE64_URL_COMPAT);
}

static void boundaries(void)
{
   char out[5] = "xxxx";
   size_t n = 99;
   assert(!aimee_codec_base64_capacity(SIZE_MAX, AIMEE_BASE64_STANDARD));
   assert(!aimee_codec_base64_capacity(SIZE_MAX, AIMEE_BASE64_URL));
   assert(!aimee_codec_base64_capacity(1, (aimee_base64_variant_t)99));
   assert(!aimee_codec_base64_encode((const unsigned char *)"f", 1, out, 4, AIMEE_BASE64_STANDARD,
                                     &n) &&
          n == 0 && out[0] == '\0');
   assert(aimee_codec_base64_encode(NULL, 0, out, 1, AIMEE_BASE64_STANDARD, &n) && n == 0 &&
          !out[0]);
   assert(!aimee_codec_base64_encode(NULL, 1, out, sizeof(out), AIMEE_BASE64_STANDARD, &n));
   unsigned char decoded[2] = {0x11, 0x22};
   assert(aimee_codec_base64_decode("Zg==", 4, decoded, 1, AIMEE_BASE64_STANDARD_CANONICAL, &n) &&
          n == 1 && decoded[0] == 'f' && decoded[1] == 0x22);
   assert(!aimee_codec_base64_decode("Zm8=", 4, decoded, 1, AIMEE_BASE64_STANDARD_CANONICAL, &n) &&
          n == 0 && decoded[1] == 0x22);
   assert(!aimee_codec_base64_decode("Zg", 2, decoded, 2, (aimee_base64_profile_t)99, &n));
   const char binary_text[] = {'Z', 'g', '\0', '!'};
   assert(!aimee_codec_base64_decode(binary_text, sizeof(binary_text), decoded, sizeof(decoded),
                                     AIMEE_BASE64_URL_CANONICAL, &n));
}

static void binary_roundtrips(void)
{
   unsigned char input[1024], output[1024];
   char encoded[1400];
   for (size_t i = 0; i < sizeof(input); ++i)
      input[i] = (unsigned char)(i * 73u + i / 7u);
   for (size_t length = 0; length <= sizeof(input); ++length)
      for (int variant = AIMEE_BASE64_STANDARD; variant <= AIMEE_BASE64_URL; ++variant)
      {
         size_t n = 0, decoded = 0;
         assert(aimee_codec_base64_encode(input, length, encoded, sizeof(encoded), variant, &n));
         assert(aimee_codec_base64_decode(encoded, n, output, sizeof(output),
                                          variant == AIMEE_BASE64_URL
                                              ? AIMEE_BASE64_URL_CANONICAL
                                              : AIMEE_BASE64_STANDARD_CANONICAL,
                                          &decoded));
         assert(decoded == length && !memcmp(input, output, length));
      }
}

static void all_byte_values(void)
{
   static const char alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   for (unsigned value = 0; value < 65536; ++value)
   {
      unsigned char input[2] = {(unsigned char)(value >> 8), (unsigned char)value}, decoded[2];
      char encoded[8];
      size_t n = 0, d = 0;
      assert(aimee_codec_base64_encode(input, sizeof(input), encoded, sizeof(encoded),
                                       AIMEE_BASE64_URL, &n));
      assert(aimee_codec_base64_decode(encoded, n, decoded, sizeof(decoded),
                                       AIMEE_BASE64_URL_CANONICAL, &d));
      assert(d == sizeof(input) && !memcmp(input, decoded, d));
      /* Every alternative spelling made by setting an unused bit must fail. */
      unsigned last = ((unsigned)value & 15u) << 2;
      for (unsigned unused = 1; unused < 4; ++unused)
      {
         encoded[2] = alphabet[last | unused];
         refuses(encoded, AIMEE_BASE64_URL_CANONICAL);
      }
   }
   for (unsigned value = 0; value < 256; ++value)
   {
      unsigned char input = (unsigned char)value;
      char encoded[8];
      size_t n = 0;
      assert(aimee_codec_base64_encode(&input, 1, encoded, sizeof(encoded), AIMEE_BASE64_URL, &n));
      unsigned last = (value & 3u) << 4;
      for (unsigned unused = 1; unused < 16; ++unused)
      {
         encoded[1] = alphabet[last | unused];
         refuses(encoded, AIMEE_BASE64_URL_CANONICAL);
      }
   }
}

int main(void)
{
   vectors();
   malformed();
   boundaries();
   binary_roundtrips();
   all_byte_values();
   puts("core base64: RFC vectors, strictness, legacy profiles, bounds and exhaustive unused-bit "
        "checks passed");
   return 0;
}
