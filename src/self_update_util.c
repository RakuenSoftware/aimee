/* self_update_util.c: pure, dependency-free helpers for thin-client self-update
 * (version parsing/comparison, version-string validation, platform asset name).
 * Kept in a leaf translation unit (libc only) so it is trivially unit-testable
 * without the HTTP/command machinery in cmd_self_update.c. See cmd_self_update.h. */

#include "headers/cmd_self_update.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif

/* Parse up to 3 dot-separated numeric components from a version string, skipping
 * a leading 'v'/'V' and stopping at '-' or any non-digit/dot. */
static void parse_semver(const char *s, long out[3])
{
   out[0] = out[1] = out[2] = 0;
   if (!s)
      return;
   if (*s == 'v' || *s == 'V')
      s++;
   for (int i = 0; i < 3 && *s; i++)
   {
      char *end = NULL;
      long v = strtol(s, &end, 10);
      if (end == s)
         break; /* no digits where a component was expected */
      out[i] = v;
      s = end;
      if (*s != '.')
         break; /* end, or a "-suffix"/other -> stop */
      s++;
   }
}

int aimee_version_compare(const char *a, const char *b)
{
   long va[3], vb[3];
   parse_semver(a, va);
   parse_semver(b, vb);
   for (int i = 0; i < 3; i++)
   {
      if (va[i] < vb[i])
         return -1;
      if (va[i] > vb[i])
         return 1;
   }
   return 0;
}

int aimee_version_is_safe(const char *s)
{
   if (!s || !s[0])
      return 0;
   for (const char *p = s; *p; p++)
   {
      if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-'))
         return 0;
   }
   return 1;
}

const char *aimee_self_update_asset(void)
{
#ifdef _WIN32
   return "aimee-windows-x86_64.exe";
#else
   static char buf[64];
   struct utsname u;
   if (uname(&u) != 0)
      return NULL;
   const char *os = u.sysname; /* "Linux", "Darwin" */
   const char *arch = u.machine;
   if (strcmp(os, "Darwin") == 0)
   {
      /* The macOS release asset is a universal (arm64+x86_64) binary. */
      snprintf(buf, sizeof buf, "aimee-macos-universal");
      return buf;
   }
   if (strcmp(os, "Linux") == 0)
   {
      const char *a = NULL;
      if (strcmp(arch, "x86_64") == 0 || strcmp(arch, "amd64") == 0)
         a = "x86_64";
      else if (strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0)
         a = "arm64";
      if (!a)
         return NULL;
      snprintf(buf, sizeof buf, "aimee-linux-%s", a);
      return buf;
   }
   return NULL;
#endif
}
