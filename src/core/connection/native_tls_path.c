#include "native_tls_internal.h"
#include <aimee/core/connection/native_tls.h>

#include <stdio.h>
#include <stdlib.h>

static _Thread_local const char *call_home;

aimee_tls_t *aimee_tls_connect_home(int fd, const char *host, const char *home)
{
   const char *previous = call_home;
   call_home = home;
   aimee_tls_t *result = aimee_tls_connect(fd, host);
   call_home = previous;
   return result;
}

const char *aimee_core_native_tls_home(void)
{
   static _Thread_local char path[4096];
   if (call_home)
      return call_home;
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
