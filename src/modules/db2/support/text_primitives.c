/* Descriptor-owned DB2 support for deterministic in-place UTF-8 repair. */
#include "db2_text.h"

size_t text_sanitize_utf8(char *s)
{
   if (!s)
      return 0;

   size_t replaced = 0;
   for (size_t i = 0; s[i];)
   {
      unsigned char c = (unsigned char)s[i];
      size_t need = 0;
      if (c <= 0x7f)
         need = 1;
      else if (c >= 0xc2 && c <= 0xdf)
         need = 2;
      else if (c >= 0xe0 && c <= 0xef)
         need = 3;
      else if (c >= 0xf0 && c <= 0xf4)
         need = 4;

      int valid = need != 0;
      for (size_t j = 1; valid && j < need; j++)
         valid = s[i + j] && ((unsigned char)s[i + j] & 0xc0) == 0x80;

      if (valid && need == 3)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f))
            valid = 0;
      }
      if (valid && need == 4)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f))
            valid = 0;
      }

      if (valid)
      {
         i += need;
         continue;
      }
      s[i++] = '?';
      replaced++;
   }
   return replaced;
}
