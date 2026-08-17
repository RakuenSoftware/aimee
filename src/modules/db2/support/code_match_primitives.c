/* Descriptor-owned DB2 process support for code-search line enrichment. */
#include "db2_code_match.h"

#include <string.h>

int code_match_line(const char *content, const char *marked_snippet)
{
   if (!content || !marked_snippet)
      return 0;
   const char *start = strstr(marked_snippet, ">>>");
   if (!start)
      return 0;
   start += 3;
   const char *end = strstr(start, "<<<");
   if (!end || end == start)
      return 0;
   size_t token_length = (size_t)(end - start);

   const char *found = NULL;
   for (const char *cursor = content; *cursor; cursor++)
   {
      if (strncmp(cursor, start, token_length) == 0)
      {
         found = cursor;
         break;
      }
   }
   if (!found)
      return 0;

   int line = 1;
   for (const char *cursor = content; cursor < found; cursor++)
      if (*cursor == '\n')
         line++;
   return line;
}
