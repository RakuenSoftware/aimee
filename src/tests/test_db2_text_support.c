/* Exhaustive parity tests for descriptor-owned DB2 UTF-8 repair. */
#include "util.h"

#include <assert.h>
#include <string.h>

size_t db2_support_text_sanitize_utf8(char *s);

static void assert_parity(const unsigned char *input, size_t len, size_t expected)
{
   char monolith[16];
   char support[16];
   assert(len < sizeof(monolith));
   memcpy(monolith, input, len);
   memcpy(support, input, len);
   monolith[len] = support[len] = '\0';

   size_t monolith_count = text_sanitize_utf8(monolith);
   size_t support_count = db2_support_text_sanitize_utf8(support);
   if (expected != (size_t)-1)
   {
      assert(monolith_count == expected);
      assert(support_count == expected);
   }
   assert(monolith_count == support_count);
   assert(memcmp(monolith, support, len + 1) == 0);
}

static void test_single_bytes(void)
{
   for (unsigned value = 1; value <= 255; value++)
   {
      unsigned char input[] = {(unsigned char)value};
      assert_parity(input, sizeof(input), value <= 0x7f ? 0 : 1);
   }
   assert_parity((const unsigned char *)"", 0, 0);
   assert(text_sanitize_utf8(NULL) == 0);
   assert(db2_support_text_sanitize_utf8(NULL) == 0);
}

static void test_valid_and_truncated_sequences(void)
{
   static const unsigned char valid[][4] = {
       {0xc2, 0x80, 0, 0},       {0xdf, 0xbf, 0, 0},    {0xe0, 0xa0, 0x80, 0},
       {0xed, 0x9f, 0xbf, 0},    {0xef, 0xbf, 0xbf, 0}, {0xf0, 0x90, 0x80, 0x80},
       {0xf4, 0x8f, 0xbf, 0xbf},
   };
   static const size_t lengths[] = {2, 2, 3, 3, 3, 4, 4};
   for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
   {
      assert_parity(valid[i], lengths[i], 0);
      for (size_t truncated = 1; truncated < lengths[i]; truncated++)
         assert_parity(valid[i], truncated, truncated);
   }
}

static void test_invalid_ranges_and_mixed_text(void)
{
   static const unsigned char invalid[][4] = {
       {0xc0, 0x80, 0, 0},       {0xc1, 0xbf, 0, 0},       {0xe0, 0x9f, 0x80, 0},
       {0xed, 0xa0, 0x80, 0},    {0xf0, 0x8f, 0x80, 0x80}, {0xf4, 0x90, 0x80, 0x80},
       {0xf5, 0x80, 0x80, 0x80}, {0xff, 0x80, 0x80, 0x80},
   };
   static const size_t lengths[] = {2, 2, 3, 3, 4, 4, 4, 4};
   for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
      assert_parity(invalid[i], lengths[i], lengths[i]);

   static const unsigned char mixed[] = {
       'a', 0xc2, 0xa2, '-', 0xe0, 0x80, 0x80, '-', 0xf0, 0x9f, 0x98, 0x80, 0x80, 'z',
   };
   assert_parity(mixed, sizeof(mixed), 4);
}

static void test_exhaustive_multibyte_space(void)
{
   unsigned char input[4];

   for (unsigned lead = 0xc2; lead <= 0xdf; lead++)
      for (unsigned second = 0x80; second <= 0xbf; second++)
      {
         input[0] = (unsigned char)lead;
         input[1] = (unsigned char)second;
         assert_parity(input, 2, 0);
      }

   for (unsigned lead = 0xe0; lead <= 0xef; lead++)
      for (unsigned second = 0x80; second <= 0xbf; second++)
         for (unsigned third = 0x80; third <= 0xbf; third++)
         {
            input[0] = (unsigned char)lead;
            input[1] = (unsigned char)second;
            input[2] = (unsigned char)third;
            int valid = !((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second > 0x9f));
            assert_parity(input, 3, valid ? 0 : 3);
         }

   for (unsigned lead = 0xf0; lead <= 0xf4; lead++)
      for (unsigned second = 0x80; second <= 0xbf; second++)
         for (unsigned third = 0x80; third <= 0xbf; third++)
            for (unsigned fourth = 0x80; fourth <= 0xbf; fourth++)
            {
               input[0] = (unsigned char)lead;
               input[1] = (unsigned char)second;
               input[2] = (unsigned char)third;
               input[3] = (unsigned char)fourth;
               int valid = !((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f));
               assert_parity(input, 4, valid ? 0 : 4);
            }
}

static void test_every_truncation_and_malformed_position(void)
{
   for (unsigned lead = 0xc2; lead <= 0xf4; lead++)
   {
      size_t need = lead <= 0xdf ? 2 : lead <= 0xef ? 3 : 4;
      unsigned char input[4] = {(unsigned char)lead, 0x80, 0x80, 0x80};
      if (lead == 0xe0)
         input[1] = 0xa0;
      else if (lead == 0xed)
         input[1] = 0x9f;
      else if (lead == 0xf0)
         input[1] = 0x90;
      else if (lead == 0xf4)
         input[1] = 0x8f;

      for (size_t truncated = 1; truncated < need; truncated++)
         assert_parity(input, truncated, truncated);

      for (size_t position = 1; position < need; position++)
      {
         unsigned char original = input[position];
         for (unsigned value = 1; value <= 255; value++)
         {
            if ((value & 0xc0) == 0x80)
               continue;
            input[position] = (unsigned char)value;
            assert_parity(input, need, (size_t)-1);
         }
         input[position] = original;
      }
   }
}

int main(void)
{
   test_single_bytes();
   test_valid_and_truncated_sequences();
   test_invalid_ranges_and_mixed_text();
   test_exhaustive_multibyte_space();
   test_every_truncation_and_malformed_position();
   return 0;
}
