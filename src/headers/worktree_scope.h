/* Git isolation applies to repositories, not ordinary document directories.
 * Keep this filesystem-only predicate shared by the thin client and server. */
#ifndef AIMEE_WORKTREE_SCOPE_H
#define AIMEE_WORKTREE_SCOPE_H

#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Only return true for a positively identified non-Git path. Missing file
 * descendants are normal for Write; inspect their nearest existing ancestor.
 * Resolve that ancestor before walking so symlinks into repositories count.
 * A .git file (linked worktree/submodule) counts just like a .git directory.
 * Unknown paths, permissions failures and oversized paths keep enforcement. */
static inline int worktree_scope_path_non_git(const char *path)
{
   char probe[PATH_MAX], resolved[PATH_MAX], marker[PATH_MAX];
   struct stat st;
   if (!path || !path[0] || strlen(path) >= sizeof(probe))
      return 0;
   snprintf(probe, sizeof(probe), "%s", path);
#ifdef AIMEE_WINDOWS
   for (char *p = probe; *p; p++)
      if (*p == '\\')
         *p = '/';
#endif
   while (stat(probe, &st) != 0)
   {
      if (errno != ENOENT && errno != ENOTDIR)
         return 0;
      char *slash = strrchr(probe, '/');
      if (!slash)
         return 0;
      if (slash == probe || (slash == probe + 2 && probe[1] == ':'))
         slash[1] = '\0';
      else
         *slash = '\0';
   }
   if (!realpath(probe, resolved))
      return 0;
#ifdef AIMEE_WINDOWS
   for (char *p = resolved; *p; p++)
      if (*p == '\\')
         *p = '/';
#endif
   if (!S_ISDIR(st.st_mode))
   {
      char *slash = strrchr(resolved, '/');
      if (!slash)
         return 0;
      if (slash == resolved || (slash == resolved + 2 && resolved[1] == ':'))
         slash[1] = '\0';
      else
         *slash = '\0';
   }
   for (;;)
   {
      int n = snprintf(marker, sizeof(marker), "%s/.git", resolved);
      if (n < 0 || (size_t)n >= sizeof(marker))
         return 0;
      if (stat(marker, &st) == 0 || errno != ENOENT)
         return 0;
      char *slash = strrchr(resolved, '/');
      if (!slash)
         return 0;
      if (!slash[1])
         return 1;
      if (slash == resolved || (slash == resolved + 2 && resolved[1] == ':'))
         slash[1] = '\0';
      else
         *slash = '\0';
   }
}

static inline int worktree_scope_non_git(const char *cwd, const char *target)
{
   struct stat st;
   return cwd && cwd[0] && stat(cwd, &st) == 0 && S_ISDIR(st.st_mode) &&
          worktree_scope_path_non_git(cwd) &&
          (!target || !target[0] || worktree_scope_path_non_git(target));
}
#endif
