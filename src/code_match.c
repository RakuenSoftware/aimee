/* code_match.c: see code_match.h. */
#include "code_match.h"
#include <string.h>

int code_match_line(const char *content, const char *marked_snippet)
{
   if (!content || !marked_snippet)
      return 0;
   const char *s = strstr(marked_snippet, ">>>");
   if (!s)
      return 0;
   s += 3;
   const char *e = strstr(s, "<<<");
   if (!e || e == s)
      return 0;
   size_t toklen = (size_t)(e - s);

   /* Find the first verbatim occurrence of the marked token in content. The
    * token is not NUL-terminated within marked_snippet, so compare bounded. */
   const char *found = NULL;
   for (const char *p = content; *p; p++)
   {
      if (strncmp(p, s, toklen) == 0)
      {
         found = p;
         break;
      }
   }
   if (!found)
      return 0;

   int line = 1;
   for (const char *p = content; p < found; p++)
      if (*p == '\n')
         line++;
   return line;
}
