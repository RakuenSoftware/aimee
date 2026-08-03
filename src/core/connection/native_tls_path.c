#include "native_tls_internal.h"

#include <stdio.h>
#include <stdlib.h>

const char *aimee_core_native_tls_home(void)
{
   static _Thread_local char path[4096];
   const char *override = getenv("AIMEE_HOME");
   if (override && override[0])
   {
      int n = snprintf(path, sizeof(path), "%s", override);
      return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
   }
   const char *home = getenv("HOME");
#ifdef _WIN32
   if (!home || !home[0])
      home = getenv("USERPROFILE");
#endif
   if (!home || !home[0])
      return NULL;
   const char *profile = getenv("AIMEE_PROFILE");
   int n = profile && profile[0]
               ? snprintf(path, sizeof(path), "%s/.config/aimee/profiles/%s", home, profile)
               : snprintf(path, sizeof(path), "%s/.config/aimee", home);
   return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
}
