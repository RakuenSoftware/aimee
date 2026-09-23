/* Global hook installation does not grant policy authority outside a workspace. */
#ifndef AIMEE_WORKSPACE_HOOK_SCOPE_H
#define AIMEE_WORKSPACE_HOOK_SCOPE_H
#include "config.h"
#include <string.h>
static inline int workspace_hook_registered(const char *cwd)
{
   if (!cwd || !cwd[0])
      return 0;
   for (int i = 0; i < config_workspace_count(); i++)
   {
      const char *root = config_workspaces(i);
      if (!root || !root[0])
         continue;
      size_t n = strlen(root);
      while (n > 1 && root[n - 1] == '/')
         n--;
      if (strncmp(root, cwd, n) == 0 && (n == 1 || cwd[n] == '/' || cwd[n] == '\0'))
         return 1;
   }
   return 0;
}
#endif
