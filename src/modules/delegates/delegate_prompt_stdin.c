/* delegate_prompt_stdin.c: stdin prompt reader for delegate commands. */
#include "aimee.h"

#include <stdio.h>
#include <stdlib.h>

char *delegate_read_prompt_stdin(void)
{
   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
      fatal("out of memory reading stdin prompt");

   for (;;)
   {
      if (len + 1024 >= cap)
      {
         if (cap >= 2 * 1024 * 1024)
         {
            free(buf);
            fatal("stdin prompt too large (max 2MB)");
         }
         size_t next = cap * 2;
         if (next > 2 * 1024 * 1024 + 1)
            next = 2 * 1024 * 1024 + 1;
         char *nb = realloc(buf, next);
         if (!nb)
         {
            free(buf);
            fatal("out of memory reading stdin prompt");
         }
         buf = nb;
         cap = next;
      }

      size_t n = fread(buf + len, 1, cap - len - 1, stdin);
      len += n;
      if (n == 0)
      {
         if (ferror(stdin))
         {
            free(buf);
            fatal("failed to read stdin prompt");
         }
         break;
      }
   }

   buf[len] = '\0';
   if (len == 0)
   {
      free(buf);
      fatal("stdin prompt is empty");
   }
   return buf;
}
