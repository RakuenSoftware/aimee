/* Descriptor-owned DB2 process support for certificate-serial canonicalization. */
#include "db2_cert_serial.h"

#include <ctype.h>
#include <string.h>

int kb_cert_serial_normalize(const char *serial, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!serial)
      return -1;
   const char *cursor = serial;
   if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
      cursor += 2;

   char normalized[512];
   size_t length = 0;
   for (; *cursor; ++cursor)
   {
      if (*cursor == ':' || *cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
         continue;
      if (length + 1 >= sizeof(normalized))
         return -1;
      normalized[length++] = (char)tolower((unsigned char)*cursor);
   }
   normalized[length] = '\0';

   const char *result = normalized;
   while (result[0] == '0' && result[1] != '\0')
      ++result;
   if (result[0] == '\0')
      result = "0";
   size_t result_length = strlen(result);
   if (result_length >= cap)
      return -1;
   memcpy(out, result, result_length + 1);
   return 0;
}
