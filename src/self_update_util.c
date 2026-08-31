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

int aimee_version_is_semver(const char *s)
{
   if (!s)
      return 0;
   if (*s == 'v' || *s == 'V')
      s++;
   /* Comparable only if it starts with a numeric component. A dev/branch build
    * version like "testing-b4a856b" is deliberately NOT a semver (see
    * publish-testing.yml), so ordering against it is meaningless. */
   return *s >= '0' && *s <= '9';
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

/* Strip a leading 'v' so callers never render "vv0.3.0". */
static const char *notice_vnum(const char *s)
{
   return (s && (s[0] == 'v' || s[0] == 'V')) ? s + 1 : s;
}

int aimee_self_update_notice_for(const char *server_ver, long server_time, const char *client_ver,
                                 long client_time, char *out, size_t cap)
{
   if (!out || cap == 0)
      return 0;
   out[0] = '\0';
   if (!server_ver || !server_ver[0])
      return 0;
   if (!client_ver)
      client_ver = "";

   /* A released server is orderable by semver, and `self-update` can fetch the
    * exact matching release asset -- so semver stays the authority whenever the
    * server reports one. */
   if (aimee_version_is_semver(server_ver))
   {
      if (aimee_version_compare(server_ver, client_ver) <= 0)
         return 0;
      snprintf(out, cap,
               "aimee-server is v%s but this client is v%s. Run `aimee self-update` to "
               "catch up (keeps client and server in lockstep).",
               notice_vnum(server_ver), notice_vnum(client_ver));
      return 1;
   }

   /* The server is a dev/branch build ("testing-<sha>"), whose version string is
    * deliberately not orderable -- and returning 0 here is what made a week-old
    * client invisible against a :testing deployment. Version strings are not the
    * only orderable thing both sides carry: each binary is stamped with its HEAD
    * commit time precisely for stale-binary detection, so use it.
    *
    * Timestamps decide only whether to SPEAK. They deliberately do not steer the
    * reader to `aimee self-update`, which resolves a release asset by version and
    * has no artifact to fetch for a "testing-<sha>" server. Naming a remedy that
    * cannot work would be worse than the silence this replaces. */
   if (server_time <= 0 || client_time <= 0 || server_time <= client_time)
      return 0;

   long days = (server_time - client_time) / 86400;
   if (days > 0)
      snprintf(out, cap,
               "aimee-server runs development build %s, built %ld day(s) ahead of this client "
               "(%s). Where the two disagree, command output can be wrong or silently empty; "
               "install a client from the server's own build.",
               server_ver, days, notice_vnum(client_ver));
   else
      snprintf(out, cap,
               "aimee-server runs development build %s, built ahead of this client (%s). Where "
               "the two disagree, command output can be wrong or silently empty; install a "
               "client from the server's own build.",
               server_ver, notice_vnum(client_ver));
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
