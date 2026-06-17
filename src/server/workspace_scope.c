/* workspace_scope.c — per-webuser workspace isolation. See workspace_scope.h. */
#include "workspace_scope.h"
#include "aimee_home.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WS_NAME_MAX 64

int ws_scope_name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t n = strlen(name);
   if (n > WS_NAME_MAX)
      return 0;
   if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      return 0;
   /* First char alnum (no leading '.', '-' — keeps names off hidden files and
    * away from looking like CLI flags downstream). */
   unsigned char c0 = (unsigned char)name[0];
   int alnum0 = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9');
   if (!alnum0)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)name[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
      if (!ok)
         return 0;
   }
   return 1;
}

/* Extract the validated <name> from a "webuser:<name>" principal. Returns 0 and
 * fills name[cap] on success, -1 otherwise. */
static int principal_name(const char *principal, char *name, size_t cap)
{
   static const char *PFX = "webuser:";
   if (!principal)
      return -1;
   size_t pl = strlen(PFX);
   if (strncmp(principal, PFX, pl) != 0)
      return -1;
   const char *u = principal + pl;
   size_t ul = strlen(u);
   if (ul == 0 || ul >= cap)
      return -1;
   if (!ws_scope_name_valid(u))
      return -1;
   memcpy(name, u, ul + 1);
   return 0;
}

/* Resolve the webusers base dir (parent of all per-user roots) into out[cap].
 * Mirrors workspace_mirror's AIMEE_WORKSPACES_DIR / <aimee_home>/workspaces
 * convention, with a /webusers leaf. */
static int webusers_base(char *out, size_t cap)
{
   const char *env = getenv("AIMEE_WORKSPACES_DIR");
   int n;
   if (env && env[0] == '/')
      n = snprintf(out, cap, "%s/webusers", env);
   else
   {
      const char *home = aimee_home();
      if (!home || !home[0])
         return -1;
      n = snprintf(out, cap, "%s/workspaces/webusers", home);
   }
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* mkdir -p each component of `path` with mode 0700 (idempotent). Returns 0 or
 * -1. Only used for our own controlled paths (no user-supplied components past
 * validation). */
static int mkdirs_0700(const char *path)
{
   char tmp[PATH_MAX];
   size_t n = strlen(path);
   if (n == 0 || n >= sizeof(tmp))
      return -1;
   memcpy(tmp, path, n + 1);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

int ws_scope_user_root(const char *principal, int create, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   char name[WS_NAME_MAX + 1];
   if (principal_name(principal, name, sizeof(name)) != 0)
      return -1;
   char base[PATH_MAX];
   if (webusers_base(base, sizeof(base)) != 0)
      return -1;
   int n = snprintf(out, cap, "%s/%s", base, name);
   if (n < 0 || (size_t)n >= cap)
   {
      if (cap)
         out[0] = '\0';
      return -1;
   }
   if (create && mkdirs_0700(out) != 0)
      return -1;
   return 0;
}

/* 1 iff `child` lies within `root` with a '/' boundary (or equals root). Both
 * must be canonical absolute paths. */
static int path_within(const char *root, const char *child)
{
   size_t rl = strlen(root);
   if (rl == 0)
      return 0;
   if (strncmp(child, root, rl) != 0)
      return 0;
   return child[rl] == '/' || child[rl] == '\0';
}

int ws_scope_project_path(const char *principal, const char *project, int must_exist, char *out,
                          size_t cap)
{
   if (!out || cap == 0)
      return -1;
   if (!project || !ws_scope_name_valid(project))
      return -1;
   char root[PATH_MAX];
   /* create=1: the root must exist so we can canonicalize it. */
   if (ws_scope_user_root(principal, 1, root, sizeof(root)) != 0)
      return -1;
   char canon_root[PATH_MAX];
   if (!realpath(root, canon_root))
      return -1;

   char cand[PATH_MAX];
   int n = snprintf(cand, sizeof(cand), "%s/%s", canon_root, project);
   if (n < 0 || (size_t)n >= sizeof(cand))
      return -1;

   if (must_exist)
   {
      char resolved[PATH_MAX];
      if (!realpath(cand, resolved))
         return -1; /* doesn't exist or unresolvable */
      if (!path_within(canon_root, resolved))
         return -1; /* symlink escape */
      n = snprintf(out, cap, "%s", resolved);
   }
   else
   {
      /* Clone target: must not already exist (as anything — incl. a symlink
       * pointing elsewhere). lstat so a symlink is detected, not followed. */
      struct stat st;
      if (lstat(cand, &st) == 0)
         return -1; /* exists — caller (clone) refuses; a symlink here is a trap */
      /* canon_root is canonical and `project` is a single safe component, so
       * cand cannot traverse out. */
      n = snprintf(out, cap, "%s", cand);
   }
   if (n < 0 || (size_t)n >= cap)
   {
      if (cap)
         out[0] = '\0';
      return -1;
   }
   return 0;
}

int ws_scope_contains(const char *principal, const char *abs_path)
{
   if (!abs_path || abs_path[0] != '/')
      return 0;
   char root[PATH_MAX];
   if (ws_scope_user_root(principal, 1, root, sizeof(root)) != 0)
      return 0;
   char canon_root[PATH_MAX], canon_path[PATH_MAX];
   if (!realpath(root, canon_root) || !realpath(abs_path, canon_path))
      return 0;
   return path_within(canon_root, canon_path);
}

int ws_scope_open_user_root(const char *principal)
{
   char root[PATH_MAX];
   if (ws_scope_user_root(principal, 1, root, sizeof(root)) != 0)
      return -1;
   return open(root, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int ws_scope_open_project(const char *principal, const char *project, int extra_flags)
{
   if (!project || !ws_scope_name_valid(project))
      return -1;
   int base = ws_scope_open_user_root(principal);
   if (base < 0)
      return -1;
   /* base is pinned to the canonical per-principal root; `project` is a single
    * validated component; O_NOFOLLOW rejects a symlink planted at the leaf — so
    * this open cannot escape the root and has no resolve-vs-use TOCTOU window. */
   int fd = openat(base, project, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | extra_flags);
   int e = errno;
   close(base);
   errno = e;
   return fd;
}
