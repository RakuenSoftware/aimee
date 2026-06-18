/* aimee_home.c: see aimee_home.h. */

#include "aimee_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *aimee_home(void)
{
   /* Thread-local: this is a per-call scratch buffer whose pointer is returned
    * to the caller. aimee-kb calls config_load() (which routes here) from
    * several worker threads concurrently (ingest watch/timer, reflection), so a
    * process-global static buffer is a data race (TSan-confirmed) and risks a
    * torn read of the path. Per-thread storage makes each call race-free while
    * preserving the re-read-getenv-every-call semantics. */
   static __thread char path[4096];

   /* 1. Explicit override wins. Operators or tests set AIMEE_HOME
    *    to an absolute path; we don't validate beyond non-empty. */
   const char *override = getenv("AIMEE_HOME");
   if (override && override[0])
   {
      snprintf(path, sizeof(path), "%s", override);
      return path;
   }

   const char *home = getenv("HOME");
   if (!home || !home[0])
      return NULL;

   /* 2. AIMEE_PROFILE selects a sibling under ~/.config/aimee/profiles/.
    *    Empty / whitespace-only values are treated as unset so
    *    `AIMEE_PROFILE=""` doesn't accidentally land in profiles//. */
   const char *profile = getenv("AIMEE_PROFILE");
   if (profile && profile[0])
   {
      int n = snprintf(path, sizeof(path), "%s/.config/aimee/profiles/%s", home, profile);
      if (n < 0 || (size_t)n >= sizeof(path))
         return NULL;
      return path;
   }

   /* 3. Default. */
   int n = snprintf(path, sizeof(path), "%s/.config/aimee", home);
   if (n < 0 || (size_t)n >= sizeof(path))
      return NULL;
   return path;
}

const char *aimee_profiles_dir(void)
{
   static __thread char path[4096]; /* per-call scratch; see aimee_home() */

   const char *override = getenv("AIMEE_HOME");
   if (override && override[0])
   {
      int n = snprintf(path, sizeof(path), "%s/profiles", override);
      if (n < 0 || (size_t)n >= sizeof(path))
         return NULL;
      return path;
   }

   const char *home = getenv("HOME");
   if (!home || !home[0])
      return NULL;

   int n = snprintf(path, sizeof(path), "%s/.config/aimee/profiles", home);
   if (n < 0 || (size_t)n >= sizeof(path))
      return NULL;
   return path;
}
