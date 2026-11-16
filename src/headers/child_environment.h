#ifndef AIMEE_CHILD_ENVIRONMENT_H
#define AIMEE_CHILD_ENVIRONMENT_H
#include <stdlib.h>
#include <string.h>
/* Call only in the forked child. Host-prepared overlays avoid reading/copying
 * process-global environ while concurrent server requests mutate their legacy
 * environment. This follows the existing child PATH setup convention. */
static inline int child_environment_overlay(char *const environment[])
{
   for (char *const *p = environment; p && *p; p++)
   {
      const char *equal = strchr(*p, '=');
      if (!equal || equal == *p || (size_t)(equal - *p) >= 64)
         return -1;
      char key[64];
      memcpy(key, *p, (size_t)(equal - *p));
      key[equal - *p] = 0;
      if (setenv(key, equal + 1, 1))
         return -1;
   }
   return 0;
}
/* Child-only full replacement: never mutate the producer's ambient environ. */
static inline int child_environment_install(char *const environment[], int replace)
{
   if (replace)
   {
      extern char **environ;
      environ = NULL; /* This is the forked child's private pointer. */
   }
   return child_environment_overlay(environment);
}
#endif
