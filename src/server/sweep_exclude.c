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

static int is_dotdot_component(const char *p)
{
   /* p points at the start of a path component; true if it is exactly ".." */
   return p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/');
}

int sweep_path_safe(const char *path)
{
   if (!path || !path[0])
      return 0;
   if (path[0] == '/')
      return 0; /* no absolute paths */
   for (const char *p = path; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-' || c == '/';
      if (!ok)
         return 0; /* any other byte: shell meta, space, glob, quote, etc. */
      if (c == '.' && (p == path || p[-1] == '/') && is_dotdot_component(p))
         return 0; /* a ".." path component */
   }
   return 1;
}

int sweep_extract_seam_key(const char *proposal_md, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!proposal_md || !out || cap == 0)
      return 0;
   static const char *PREFIX = "# Deepen seam: ";
   const char *h = strstr(proposal_md, PREFIX);
   if (!h)
      return 0;
   h += strlen(PREFIX);
   size_t i = 0;
   while (h[i] && h[i] != '\n' && h[i] != '\r' && i + 1 < cap)
   {
      out[i] = h[i];
      i++;
   }
   out[i] = '\0';
   while (i > 0 && out[i - 1] == ' ') /* defensive trailing-space trim */
      out[--i] = '\0';
   return out[0] != '\0';
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
