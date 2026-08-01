#ifndef AIMEE_JSON_WIRE_H
#define AIMEE_JSON_WIRE_H

#include <stddef.h>
#include <string.h>

/* cJSON exposes decoded strings as NUL-terminated values, so an escaped JSON
 * NUL would otherwise make distinct wire values indistinguishable. This scan
 * runs before decoding. It deliberately skips escaped backslashes: the JSON
 * text "\\\\u0000" represents the literal characters "\\u0000", not NUL. */
static inline int json_wire_has_nul_escape(const char *s, size_t n)
{
   int in_string = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (!in_string)
      {
         if (s[i] == '"')
            in_string = 1;
      }
      else if (s[i] == '"')
         in_string = 0;
      else if (s[i] == '\\' && i + 1 < n)
      {
         if (s[i + 1] == 'u' && i + 5 < n && !memcmp(s + i + 2, "0000", 4))
            return 1;
         i++;
      }
   }
   return 0;
}

#endif
