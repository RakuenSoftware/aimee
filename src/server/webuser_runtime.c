/* webuser_runtime.c — per-webuser tmpfs runtime dir (fail-closed). See header. */
#include "webuser_runtime.h"
#include "workspace_scope.h" /* ws_scope_name_valid */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

/* From linux/magic.h — declared here so the TU needs no kernel headers. */
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

#define WR_NAME_MAX 64

int webuser_runtime_is_tmpfs(const char *path)
{
   if (!path || !path[0])
      return -1;
   struct statfs sf;
   if (statfs(path, &sf) != 0)
      return -1;
   if ((unsigned long)sf.f_type == TMPFS_MAGIC || (unsigned long)sf.f_type == RAMFS_MAGIC)
      return 1;
   return 0;
}

/* Validated <name> from a "webuser:<name>" principal, or -1. */
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
   if (ul == 0 || ul >= cap || !ws_scope_name_valid(u))
      return -1;
   memcpy(name, u, ul + 1);
   return 0;
}

/* Resolve the runtime base (parent of per-user dirs) into out[cap]. */
static int runtime_base(char *out, size_t cap)
{
   const char *env = getenv("AIMEE_RUNTIME_DIR");
   int n;
   if (env && env[0] == '/')
      n = snprintf(out, cap, "%s", env);
   else
   {
      const char *xdg = getenv("XDG_RUNTIME_DIR");
      if (xdg && xdg[0] == '/')
         n = snprintf(out, cap, "%s/aimee", xdg);
      else
         n = snprintf(out, cap, "/dev/shm/aimee");
   }
   if (n < 0 || (size_t)n >= cap)
   {
      if (cap)
         out[0] = '\0';
      return -1;
   }
   return 0;
}

/* Build (NO side effects: no mkdir, no tmpfs check) the per-user runtime dir
 * path into out[cap], and optionally the base into base_out. Returns 0, or -1 on
 * a malformed principal / buffer overflow (out emptied). Shared by the creating
 * resolver and cleanup so cleanup never depends on — or triggers — creation. */
static int resolve_dir(const char *principal, char *out, size_t cap, char *base_out,
                       size_t base_cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   char name[WR_NAME_MAX + 1];
   if (principal_name(principal, name, sizeof(name)) != 0)
      return -1;
   char base[PATH_MAX];
   if (runtime_base(base, sizeof(base)) != 0)
      return -1;
   int n = snprintf(out, cap, "%s/webusers/%s", base, name);
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   if (base_out)
   {
      int bn = snprintf(base_out, base_cap, "%s", base);
      if (bn < 0 || (size_t)bn >= base_cap)
         return -1;
   }
   return 0;
}

int webuser_runtime_dir(const char *principal, char *out, size_t cap)
{
   char base[PATH_MAX];
   /* Build + cap-validate the FULL path before creating anything, so an error
    * path creates nothing (not even the shared /webusers parent). */
   if (resolve_dir(principal, out, cap, base, sizeof(base)) != 0)
      return -1;
   char parent[PATH_MAX];
   if (snprintf(parent, sizeof(parent), "%s/webusers", base) >= (int)sizeof(parent))
   {
      out[0] = '\0';
      return -1;
   }

   /* Create the base, then assert it is tmpfs BEFORE creating anything under it.
    * Fail-closed: a non-tmpfs base must never host credential sockets. On any
    * error `out` is emptied (postcondition: valid path iff return 0). */
   if ((mkdir(base, 0700) != 0 && errno != EEXIST) || webuser_runtime_is_tmpfs(base) != 1 ||
       (mkdir(parent, 0700) != 0 && errno != EEXIST) || (mkdir(out, 0700) != 0 && errno != EEXIST))
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

void webuser_runtime_cleanup(const char *principal)
{
   char dir[PATH_MAX];
   /* Side-effect-free resolve: cleanup must work without re-creating the dir or
    * depending on the tmpfs gate. */
   if (resolve_dir(principal, dir, sizeof(dir), NULL, 0) != 0)
      return;
   /* The per-user runtime dir holds only flat sockets/files we created (0700,
    * our UID) — no subdirs — so unlink each entry then rmdir. No system(), no
    * recursion, no shell-injection surface. */
   int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (dfd >= 0)
   {
      DIR *d = fdopendir(dfd);
      if (d)
      {
         struct dirent *e;
         while ((e = readdir(d)) != NULL)
         {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
               continue;
            unlinkat(dfd, e->d_name, 0);
         }
         closedir(d); /* closes dfd */
      }
      else
         close(dfd);
   }
   rmdir(dir);
}
