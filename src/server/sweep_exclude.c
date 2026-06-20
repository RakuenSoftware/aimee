/* sweep_exclude.c: deterministic seam-exclusion identity for the deepening sweep.
 * See headers/sweep.h. */
#include "sweep.h"

#include <stdio.h>
#include <string.h>

void sweep_seam_key(const char *seam_file, const char *seam_symbol, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   /* An incomplete seam yields an empty key, never an ambiguous ":" / "file:" /
    * ":sym" that could collide — and an empty key never matches in exclusion. */
   if (!seam_file || !seam_file[0] || !seam_symbol || !seam_symbol[0])
   {
      out[0] = '\0';
      return;
   }
   snprintf(out, cap, "%s:%s", seam_file, seam_symbol);
}

int sweep_excluded(const char *seam_key, const char *const *settled, int n)
{
   if (!seam_key || !seam_key[0] || !settled)
      return 0;
   for (int i = 0; i < n; i++)
      if (settled[i] && strcmp(seam_key, settled[i]) == 0)
         return 1;
   return 0;
}
