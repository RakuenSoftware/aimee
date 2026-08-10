/* workspace_scope.c — single-environment workspace confinement. See header. */
#include "workspace_scope.h"
#include "aimee_home.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#ifdef SYS_openat2
#include <linux/openat2.h>
#define WS_HAVE_OPENAT2 1
#endif
#endif

#define WS_NAME_MAX 64

static ws_scope_ref_validator_fn g_ref_validator;

void ws_scope_register_ref_validator(ws_scope_ref_validator_fn validator)
{
   g_ref_validator = validator;
}

int ws_scope_name_valid(const char *name)
{
   /* One owner for what a name may be. This used to restate the module's rule
    * in C -- the same alphabet, the same 64-byte cap, the same refusal of "."
    * and ".." -- with nothing in the build keeping the two copies in step, so
    * either could be tightened alone and the seams would disagree about the
    * same string.
    *
    * A name is simply a ref with no separator in it: the module answers a
    * slash-free ref by asking exactly the name question. So reject the
    * separator here, which is the only part that is about being a single
    * component, and let the module decide the rest. */
   if (!name || !name[0] || strchr(name, '/'))
      return 0;
   return ws_scope_project_ref_valid(name, strlen(name));
}

int ws_scope_project_ref_valid(const char *buf, size_t len)
{
   if (!g_ref_validator || !buf || len == 0 || len > WS_REF_MAX)
      return 0;
   int allowed = 0;
   return g_ref_validator(buf, len, &allowed) == 0 && allowed;
}

/* Split a VALIDATED ref into org (may be empty for a flat ref) + repo. Returns
 * the component count (1 or 2), or -1 if the ref fails validation. */
int ws_scope_ref_split(const char *ref, char *org, size_t org_cap, char *repo, size_t repo_cap)
{
   if (!ref || !org || !repo || org_cap == 0 || repo_cap == 0)
      return -1;
   size_t len = strlen(ref);
   if (!ws_scope_project_ref_valid(ref, len))
      return -1;
   const char *slash = memchr(ref, '/', len);
   if (!slash)
   {
      org[0] = '\0';
      if (len >= repo_cap)
         return -1;
      memcpy(repo, ref, len + 1);
      return 1;
   }
   size_t org_len = (size_t)(slash - ref), repo_len = len - org_len - 1;
   if (org_len >= org_cap || repo_len >= repo_cap)
      return -1;
   memcpy(org, ref, org_len);
   org[org_len] = '\0';
   memcpy(repo, slash + 1, repo_len + 1);
   return 2;
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

/* Resolve the deployment's workspace container. The shared environment root and
 * the legacy webusers tree are siblings beneath it. */
static int workspace_container(char *out, size_t cap)
{
   const char *env = getenv("AIMEE_WORKSPACES_DIR");
   int n;
   if (env && env[0] == '/')
      n = snprintf(out, cap, "%s", env);
   else
   {
      const char *home = aimee_home();
      if (!home || !home[0])
         return -1;
      n = snprintf(out, cap, "%s/workspaces", home);
   }
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

static unsigned legacy_hash(const char *s)
{
   unsigned h = 2166136261u;
   for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++)
      h = (h ^ *p) * 16777619u;
   return h;
}

/* Pick a collision-free, API-valid destination for legacy state. The common
 * case keeps the original project/org name. A conflict is retained under an
 * explicit legacy-<actor>-<name>-<hash> ref; nothing is overwritten. */
static int legacy_destination(const char *root, const char *actor, const char *entry,
                              const char *source, char *out, size_t cap, int *renamed)
{
   struct stat st;
   if (ws_scope_name_valid(entry))
   {
      int n = snprintf(out, cap, "%s/%s", root, entry);
      if (n > 0 && (size_t)n < cap && lstat(out, &st) != 0 && errno == ENOENT)
      {
         if (renamed)
            *renamed = 0;
         return 0;
      }
   }
   const char *a = ws_scope_name_valid(actor) ? actor : "actor";
   const char *e = ws_scope_name_valid(entry) ? entry : "state";
   unsigned seed = legacy_hash(source);
   for (unsigned i = 0; i < 1000; i++)
   {
      int n = snprintf(out, cap, "%s/legacy-%.12s-%.24s-%08x", root, a, e, seed + i);
      if (n <= 0 || (size_t)n >= cap)
         return -1;
      if (lstat(out, &st) != 0 && errno == ENOENT)
      {
         if (renamed)
            *renamed = 1;
         return 0;
      }
   }
   return -1;
}

/* Move every legacy <container>/webusers/<actor>/<entry> into the shared root.
 * Top-level entries are moved whole, so arbitrary editor files and nested org
 * layouts are preserved. A deployment lock makes the lazy migration safe when
 * two server processes overlap during an upgrade. */
static int migrate_legacy_webusers(const char *container, const char *root)
{
   char lockpath[PATH_MAX], legacy[PATH_MAX];
   if (snprintf(lockpath, sizeof(lockpath), "%s/.single-tenant-migration.lock", container) >=
           (int)sizeof(lockpath) ||
       snprintf(legacy, sizeof(legacy), "%s/webusers", container) >= (int)sizeof(legacy))
      return -1;
   int lfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (lfd < 0 || flock(lfd, LOCK_EX) != 0)
   {
      if (lfd >= 0)
         close(lfd);
      return -1;
   }

   DIR *users = opendir(legacy);
   if (!users)
   {
      int ok = errno == ENOENT;
      flock(lfd, LOCK_UN);
      close(lfd);
      return ok ? 0 : -1;
   }

   int failed = 0;
   struct dirent *ue;
   while ((ue = readdir(users)) != NULL)
   {
      if (strcmp(ue->d_name, ".") == 0 || strcmp(ue->d_name, "..") == 0 || ue->d_name[0] == '.')
         continue; /* old .registry/.locks stay as inert migration metadata */
      char userdir[PATH_MAX];
      if (snprintf(userdir, sizeof(userdir), "%s/%s", legacy, ue->d_name) >= (int)sizeof(userdir))
      {
         failed = 1;
         continue;
      }
      struct stat ust;
      if (lstat(userdir, &ust) != 0 || !S_ISDIR(ust.st_mode))
      {
         failed = 1;
         continue;
      }
      DIR *entries = opendir(userdir);
      if (!entries)
      {
         failed = 1;
         continue;
      }
      struct dirent *pe;
      while ((pe = readdir(entries)) != NULL)
      {
         if (strcmp(pe->d_name, ".") == 0 || strcmp(pe->d_name, "..") == 0)
            continue;
         char src[PATH_MAX], dst[PATH_MAX];
         if (snprintf(src, sizeof(src), "%s/%s", userdir, pe->d_name) >= (int)sizeof(src))
         {
            failed = 1;
            continue;
         }
         int renamed = 0;
         if (legacy_destination(root, ue->d_name, pe->d_name, src, dst, sizeof(dst), &renamed) !=
                 0 ||
             rename(src, dst) != 0)
         {
            fprintf(stderr, "aimee: legacy workspace migration failed for %s\n", src);
            failed = 1;
            continue;
         }
         if (renamed)
            fprintf(stderr, "aimee: legacy workspace conflict retained as %s\n", dst);
         else
            fprintf(stderr, "aimee: migrated legacy workspace state to %s\n", dst);
      }
      closedir(entries);
      (void)rmdir(userdir); /* only removes an empty, fully-migrated actor dir */
   }
   closedir(users);
   flock(lfd, LOCK_UN);
   close(lfd);
   return failed ? -1 : 0;
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

int ws_scope_openat2_dir(int dirfd, const char *name)
{
#ifdef WS_HAVE_OPENAT2
   struct open_how how;
   memset(&how, 0, sizeof(how));
   how.flags = O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
   how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
   long fd = syscall(SYS_openat2, dirfd, name, &how, sizeof(how));
   return fd < 0 ? -1 : (int)fd;
#else
   (void)dirfd;
   (void)name;
   errno = ENOSYS;
   return -1;
#endif
}

int ws_scope_openat2_available(void)
{
#ifdef WS_HAVE_OPENAT2
   static int cached = -1;
   if (cached < 0)
   {
      int fd = ws_scope_openat2_dir(AT_FDCWD, ".");
      if (fd >= 0)
      {
         close(fd);
         cached = 1;
      }
      else
         cached = 0; /* fail closed on ENOSYS/EINVAL/seccomp-blocked alike */
   }
   return cached;
#else
   return 0;
#endif
}

int ws_scope_environment_root(char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   char container[PATH_MAX];
   if (workspace_container(container, sizeof(container)) != 0 || mkdirs_0700(container) != 0)
      return -1;
   int n = snprintf(out, cap, "%s/environment", container);
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   if (mkdirs_0700(out) != 0 || migrate_legacy_webusers(container, out) != 0)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int ws_scope_user_root(const char *principal, int create, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   char actor[WS_NAME_MAX + 1];
   if (principal_name(principal, actor, sizeof(actor)) != 0)
      return -1;
   (void)actor;
   (void)create; /* shared root is materialized so legacy migration is atomic */
   return ws_scope_environment_root(out, cap);
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
   /* Accepts a flat name or a two-component <org>/<repo> ref; the split
    * validates each component with ws_scope_name_valid, so `project` cannot
    * traverse (the '/' is only ever a single org/repo separator). */
   char org[WS_NAME_MAX + 1], repo[WS_NAME_MAX + 1];
   if (!project || ws_scope_ref_split(project, org, sizeof(org), repo, sizeof(repo)) < 0)
      return -1;
   char root[PATH_MAX];
   /* create=1: the root must exist so we can canonicalize it. */
   if (ws_scope_user_root(principal, 1, root, sizeof(root)) != 0)
      return -1;
   char canon_root[PATH_MAX];
   if (!realpath(root, canon_root))
      return -1;

   char cand[PATH_MAX];
   int n = org[0] ? snprintf(cand, sizeof(cand), "%s/%s/%s", canon_root, org, repo)
                  : snprintf(cand, sizeof(cand), "%s/%s", canon_root, repo);
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
      /* canon_root is canonical and every ref component is validated, so cand
       * cannot traverse out. */
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
   char org[WS_NAME_MAX + 1], repo[WS_NAME_MAX + 1];
   if (!project || ws_scope_ref_split(project, org, sizeof(org), repo, sizeof(repo)) < 0)
      return -1;
   int base = ws_scope_open_user_root(principal);
   if (base < 0)
      return -1;
   /* base is pinned to the canonical environment root and every component is
    * opened individually with O_NOFOLLOW — a symlink planted at the org or the
    * project leaf is rejected, never followed, so the open cannot escape the
    * root and has no resolve-vs-use TOCTOU window. */
   if (org[0])
   {
      int orgfd = openat(base, org, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      int e = errno;
      close(base);
      if (orgfd < 0)
      {
         errno = e;
         return -1;
      }
      base = orgfd;
   }
   int fd = openat(base, repo, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | extra_flags);
   int e = errno;
   close(base);
   errno = e;
   return fd;
}
