/* platform_random.c: portable cryptographic random generation (common) */
#include "platform_random.h"
#include <stdio.h>
#include <string.h>

/* platform_random_bytes() is implemented in posix/platform_random.c or
 * windows/platform_random.c depending on the build platform. */

int platform_random_hex(char *out, size_t hex_len)
{
   if (!out || hex_len == 0 || (hex_len % 2) != 0)
      return -1;

   size_t raw_len = hex_len / 2;
   unsigned char raw[256];
   if (raw_len > sizeof(raw))
      return -1;

   if (platform_random_bytes(raw, raw_len) != 0)
   {
      out[0] = '\0';
      return -1;
   }

   for (size_t i = 0; i < raw_len; i++)
      snprintf(out + i * 2, 3, "%02x", raw[i]);
   out[hex_len] = '\0';
   return 0;
}
