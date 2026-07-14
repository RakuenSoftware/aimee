/* ws_registry.c — deployment-global project-key registry + lifecycle lock.
 * See ws_registry.h and docs/proposals/pending/webchat-project-lifecycle.md. */
#include "ws_registry.h"
#include "log.h"
#include "workspace_scope.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define WSREG_REMOTE_MAX 1024
#define WSREG_VERSION    1

/* mkdir -p each component of `path` with mode 0700 (idempotent). */
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

/* Resolve <webusers_base>/<leaf> and mkdir the chain 0700 (idempotent — the
 * base may not exist yet on a fresh install). */
static int state_dir(const char *leaf, char *out, size_t cap)
{
   char base[PATH_MAX];
   if (ws_scope_webusers_base(base, sizeof(base)) != 0)
      return -1;
   int n = snprintf(out, cap, "%s/%s", base, leaf);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   return mkdirs_0700(out);
}

/* Entry filename for `ref`: '/' -> '%'. Both components are
 * ws_scope_name_valid ('%' is not in the alphabet), so this is injective. */
static int entry_name(const char *ref, char *out, size_t cap)
{
   size_t len = ref ? strlen(ref) : 0;
   if (!ws_scope_project_ref_valid(ref, len) || len >= cap)
      return -1;
   for (size_t i = 0; i <= len; i++)
      out[i] = (ref[i] == '/') ? '%' : ref[i];
   return 0;
}

int ws_reg_lock(const char *ref)
{
   char org[WS_REF_COMP_MAX + 1], repo[WS_REF_COMP_MAX + 1];
   if (ws_scope_ref_split(ref, org, sizeof(org), repo, sizeof(repo)) < 0)
      return -1;
   const char *first = org[0] ? org : repo;
   char dir[PATH_MAX], path[PATH_MAX];
   if (state_dir(".locks", dir, sizeof(dir)) != 0)
      return -1;
   int n = snprintf(path, sizeof(path), "%s/%s", dir, first);
   if (n < 0 || (size_t)n >= sizeof(path))
      return -1;
   int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0)
      return -1;
   if (flock(fd, LOCK_EX) != 0)
   {
      close(fd);
      return -1;
   }
   return fd;
}

/* Read an entry file: "1 <holders> <remote>\n". Returns 1 found, 0 absent,
 * -1 error. */
static int entry_read(const char *path, char *remote, size_t remote_cap, int *holders)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return errno == ENOENT ? 0 : -1;
   int ver = 0, h = 0;
   char rem[WSREG_REMOTE_MAX];
   int rc = fscanf(fp, "%d %d %1023s", &ver, &h, rem);
   fclose(fp);
   if (rc != 3 || ver != WSREG_VERSION || h < 0)
      return -1;
   if (holders)
      *holders = h;
   if (remote && remote_cap)
      snprintf(remote, remote_cap, "%s", rem);
   return 1;
}

/* Atomically write an entry file (tmp + fsync + rename). */
static int entry_write(const char *dir, const char *name, int holders, const char *remote)
{
   char tmp[PATH_MAX], final[PATH_MAX];
   if (snprintf(final, sizeof(final), "%s/%s", dir, name) >= (int)sizeof(final) ||
       snprintf(tmp, sizeof(tmp), "%s/.tmp-%s", dir, name) >= (int)sizeof(tmp))
      return -1;
   int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0)
      return -1;
   char buf[WSREG_REMOTE_MAX + 32];
   int n = snprintf(buf, sizeof(buf), "%d %d %s\n", WSREG_VERSION, holders, remote);
   if (n < 0 || n >= (int)sizeof(buf) || write(fd, buf, (size_t)n) != n || fsync(fd) != 0)
   {
      close(fd);
      unlink(tmp);
      return -1;
   }
   close(fd);
   if (rename(tmp, final) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static int entry_path(const char *ref, char *dir, size_t dir_cap, char *name, size_t name_cap,
                      char *path, size_t path_cap)
{
   if (state_dir(".registry", dir, dir_cap) != 0 || entry_name(ref, name, name_cap) != 0)
      return -1;
   int n = snprintf(path, path_cap, "%s/%s", dir, name);
   return (n < 0 || (size_t)n >= path_cap) ? -1 : 0;
}

int ws_reg_lookup(const char *ref, char *remote, size_t remote_cap, int *holders)
{
   char dir[PATH_MAX], name[WS_REF_MAX + 1], path[PATH_MAX];
   if (entry_path(ref, dir, sizeof(dir), name, sizeof(name), path, sizeof(path)) != 0)
      return -1;
   return entry_read(path, remote, remote_cap, holders);
}

int ws_reg_register(const char *ref, const char *remote)
{
   if (!remote || !remote[0] || strlen(remote) >= WSREG_REMOTE_MAX || strchr(remote, ' ') ||
       strchr(remote, '\n'))
      return -1;
   char dir[PATH_MAX], name[WS_REF_MAX + 1], path[PATH_MAX];
   if (entry_path(ref, dir, sizeof(dir), name, sizeof(name), path, sizeof(path)) != 0)
      return -1;
   char cur_remote[WSREG_REMOTE_MAX];
   int holders = 0;
   int found = entry_read(path, cur_remote, sizeof(cur_remote), &holders);
   if (found < 0)
      return -1;
   if (found == 1 && strcmp(cur_remote, remote) != 0)
      return 1; /* same key, different remote — caller 409s */
   return entry_write(dir, name, found == 1 ? holders + 1 : 1, remote);
}

int ws_reg_unregister(const char *ref)
{
   char dir[PATH_MAX], name[WS_REF_MAX + 1], path[PATH_MAX];
   if (entry_path(ref, dir, sizeof(dir), name, sizeof(name), path, sizeof(path)) != 0)
      return -1;
   char cur_remote[WSREG_REMOTE_MAX];
   int holders = 0;
   if (entry_read(path, cur_remote, sizeof(cur_remote), &holders) != 1)
      return -1;
   if (holders <= 1)
   {
      if (unlink(path) != 0 && errno != ENOENT)
         return -1;
      return 0;
   }
   if (entry_write(dir, name, holders - 1, cur_remote) != 0)
      return -1;
   return holders - 1;
}

/* Read a published clone's credential-free remote sidecar (.aimee/remote,
 * single line). Returns 0 + out, -1 when absent/unreadable. */
static int sidecar_read(const char *proj_path, char *out, size_t cap)
{
   char p[PATH_MAX];
   if (snprintf(p, sizeof(p), "%s/.aimee/remote", proj_path) >= (int)sizeof(p))
      return -1;
   FILE *fp = fopen(p, "r");
   if (!fp)
      return -1;
   char line[WSREG_REMOTE_MAX];
   const char *got = fgets(line, sizeof(line), fp);
   fclose(fp);
   if (!got)
      return -1;
   line[strcspn(line, "\n")] = '\0';
   if (!line[0])
      return -1;
   snprintf(out, cap, "%s", line);
   return 0;
}

/* Accumulate one published clone into the (already wiped) registry. */
static void rebuild_add(const char *ref, const char *proj_path)
{
   char remote[WSREG_REMOTE_MAX];
   if (sidecar_read(proj_path, remote, sizeof(remote)) != 0)
   {
      /* No sidecar (legacy clone) — register under a per-ref unknown marker so
       * holder counting still works; delete treats UNKNOWN conservatively. */
      snprintf(remote, sizeof(remote), "unknown://%s", ref);
   }
   int rc = ws_reg_register(ref, remote);
   if (rc == 1)
   {
      /* Same ref, different remote across trees — shouldn't exist; keep the
       * first registration and count the holder anyway (conservative). */
      char cur[WSREG_REMOTE_MAX];
      int holders = 0;
      if (ws_reg_lookup(ref, cur, sizeof(cur), &holders) == 1)
      {
         char dir[PATH_MAX], name[WS_REF_MAX + 1], path[PATH_MAX];
         if (entry_path(ref, dir, sizeof(dir), name, sizeof(name), path, sizeof(path)) == 0)
            entry_write(dir, name, holders + 1, cur);
      }
      aimee_log(LOG_WARN, "ws.registry",
                "rebuild: ref '%s' held with divergent remotes — counting holder, keeping "
                "first-seen remote",
                ref);
   }
}

/* Is `path` a published project dir (has a .git entry)? */
static int is_project_dir(const char *path)
{
   char g[PATH_MAX];
   if (snprintf(g, sizeof(g), "%s/.git", path) >= (int)sizeof(g))
      return 0;
   struct stat st;
   return lstat(g, &st) == 0;
}

int ws_reg_rebuild(void)
{
   char base[PATH_MAX], regdir[PATH_MAX];
   if (ws_scope_webusers_base(base, sizeof(base)) != 0)
      return -1;
   if (state_dir(".registry", regdir, sizeof(regdir)) != 0)
      return -1;
   /* Wipe the registry; counts derive from published clones only. */
   DIR *rd = opendir(regdir);
   if (rd)
   {
      struct dirent *e;
      while ((e = readdir(rd)) != NULL)
      {
         if (e->d_name[0] == '.')
            continue;
         char p[PATH_MAX];
         if (snprintf(p, sizeof(p), "%s/%s", regdir, e->d_name) < (int)sizeof(p))
            unlink(p);
      }
      closedir(rd);
   }

   DIR *bd = opendir(base);
   if (!bd)
      return 0; /* fresh install: nothing to count */
   struct dirent *user;
   while ((user = readdir(bd)) != NULL)
   {
      if (user->d_name[0] == '.' || !ws_scope_name_valid(user->d_name))
         continue;
      char uroot[PATH_MAX];
      if (snprintf(uroot, sizeof(uroot), "%s/%s", base, user->d_name) >= (int)sizeof(uroot))
         continue;
      DIR *ud = opendir(uroot);
      if (!ud)
         continue;
      struct dirent *lvl1;
      while ((lvl1 = readdir(ud)) != NULL)
      {
         if (lvl1->d_name[0] == '.' || !ws_scope_name_valid(lvl1->d_name))
            continue;
         char p1[PATH_MAX];
         if (snprintf(p1, sizeof(p1), "%s/%s", uroot, lvl1->d_name) >= (int)sizeof(p1))
            continue;
         struct stat st;
         if (lstat(p1, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
         if (is_project_dir(p1))
         {
            rebuild_add(lvl1->d_name, p1); /* flat project */
            continue;
         }
         /* org dir: its children are projects */
         DIR *od = opendir(p1);
         if (!od)
            continue;
         struct dirent *lvl2;
         while ((lvl2 = readdir(od)) != NULL)
         {
            if (lvl2->d_name[0] == '.' || !ws_scope_name_valid(lvl2->d_name))
               continue;
            char p2[PATH_MAX], ref[WS_REF_MAX + 1];
            if (snprintf(p2, sizeof(p2), "%s/%s", p1, lvl2->d_name) >= (int)sizeof(p2))
               continue;
            if (lstat(p2, &st) != 0 || !S_ISDIR(st.st_mode) || !is_project_dir(p2))
               continue;
            if (snprintf(ref, sizeof(ref), "%s/%s", lvl1->d_name, lvl2->d_name) >= (int)sizeof(ref))
               continue;
            rebuild_add(ref, p2);
         }
         closedir(od);
      }
      closedir(ud);
   }
   closedir(bd);
   return 0;
}
