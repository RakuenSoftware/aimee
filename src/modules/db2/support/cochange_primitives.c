/* Descriptor-owned DB2 process support for deterministic co-change policy. */
#include "db2_cochange.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cochange_name_cmp(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

int cochange_is_hex_sha(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t n = 0;
   for (; s[n]; n++)
   {
      char c = s[n];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return 0;
   }
   return n >= 4 && n <= 64;
}

int cochange_pairs_for_commit(char names[][128], int n, int max_files, db2_cochange_pair_t *out,
                              int out_cap)
{
   if (n < 0)
      n = 0;

   int distinct = 0;
   for (int i = 0; i < n; i++)
   {
      int duplicate = 0;
      for (int k = 0; k < distinct; k++)
      {
         if (strcmp(names[k], names[i]) == 0)
         {
            duplicate = 1;
            break;
         }
      }
      if (!duplicate)
      {
         if (distinct != i)
            memcpy(names[distinct], names[i], 128);
         distinct++;
      }
   }

   if (distinct < 2 || distinct > max_files)
      return 0;

   qsort(names, (size_t)distinct, 128, cochange_name_cmp);

   int count = 0;
   for (int i = 0; i < distinct && count < out_cap; i++)
      for (int j = i + 1; j < distinct && count < out_cap; j++)
      {
         snprintf(out[count].a, sizeof(out[count].a), "%s", names[i]);
         snprintf(out[count].b, sizeof(out[count].b), "%s", names[j]);
         count++;
      }
   return count;
}
