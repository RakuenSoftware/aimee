/* harness_memory_watch.c — see harness_memory_watch.h. */

#include "harness_memory_watch.h"

#include "cJSON.h"
#include "cli_client.h"             /* cli_http_request, cli_v1_client_endpoint/bearer */
#include "harness_memory_audit.h"   /* hmem_audit */
#include "harness_memory_common.h"  /* hmem_resolve_project */
#include "harness_memory_hydrate.h" /* hmem_md_store_name, hmem_slug_from_path */
#include "harness_memory_scope.h"   /* hmem_scope_for_client */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Portable canonicalization: POSIX realpath resolves symlinks; on Windows
 * _fullpath returns an absolute path. Both return non-NULL on success and write
 * the absolute path to `out`. Mirrors the helper in harness_memory_hydrate.c
 * (MinGW lacks realpath). */
#ifdef _WIN32
#define hm_realpath(p, out) _fullpath((out), (p), PATH_MAX)
#else
#define hm_realpath(p, out) realpath((p), (out))
#endif

/* Bound the watch table so a pathological tree can't exhaust the per-user
 * inotify watch limit (/proc/sys/fs/inotify/max_user_watches, typically
 * 8192+). A memory dir has at most a handful of subdirs, so this is generous. */
#define HMEM_WATCH_MAX_DIRS 4096

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

struct hmem_watch
{
   int ifd;
   char memreal[PATH_MAX];
   struct
   {
      int wd;
      char path[PATH_MAX];
   } *dirs;
   int ndirs, dcap;
   char evbuf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
   ssize_t evlen, evpos;
};

/* Map an inotify watch descriptor back to the directory it watches. */
static const char *dir_for_wd(const hmem_watch_t *w, int wd)
{
   for (int i = 0; i < w->ndirs; i++)
      if (w->dirs[i].wd == wd)
         return w->dirs[i].path;
   return NULL;
}

/* Watch `dir` and (recursively) its subdirectories. Best-effort: a dir that
 * can't be watched is skipped. lstat (not stat) so symlinked dirs are not
 * followed — the watch tree can't escape memreal. */
static void add_watch_recursive(hmem_watch_t *w, const char *dir)
{
   if (w->ndirs >= HMEM_WATCH_MAX_DIRS)
      return;
   /* already watching this dir? */
   for (int i = 0; i < w->ndirs; i++)
      if (strcmp(w->dirs[i].path, dir) == 0)
         return;
   int wd = inotify_add_watch(w->ifd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
   if (wd < 0)
      return;
   if (w->ndirs == w->dcap)
   {
      int nc = w->dcap ? w->dcap * 2 : 16;
      void *t = realloc(w->dirs, (size_t)nc * sizeof(*w->dirs));
      if (!t)
      {
         inotify_rm_watch(w->ifd, wd);
         return;
      }
      w->dirs = t;
      w->dcap = nc;
   }
   w->dirs[w->ndirs].wd = wd;
   snprintf(w->dirs[w->ndirs].path, sizeof(w->dirs[w->ndirs].path), "%s", dir);
   w->ndirs++;

   DIR *d = opendir(dir);
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue;
      char sub[PATH_MAX];
      if ((size_t)snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name) >= sizeof(sub))
         continue;
      struct stat st;
      if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
         add_watch_recursive(w, sub);
   }
   closedir(d);
}

hmem_watch_t *hmem_watch_open(const char *memreal)
{
   if (!memreal || !memreal[0])
      return NULL;
   hmem_watch_t *w = calloc(1, sizeof(*w));
   if (!w)
      return NULL;
   w->ifd = inotify_init1(IN_NONBLOCK);
   if (w->ifd < 0)
   {
      free(w);
      return NULL;
   }
   snprintf(w->memreal, sizeof(w->memreal), "%s", memreal);
   add_watch_recursive(w, memreal);
   if (w->ndirs == 0) /* couldn't watch even the root */
   {
      close(w->ifd);
      free(w->dirs);
      free(w);
      return NULL;
   }
   return w;
}

int hmem_watch_poll(hmem_watch_t *w, char *name_out, size_t cap, int timeout_ms)
{
   if (!w || !name_out || cap == 0)
      return -1;
   /* Refill the event buffer only when the previous batch is drained, so a batch
    * holding several writes is returned one at a time without loss. */
   if (w->evpos >= w->evlen)
   {
      struct pollfd pfd = {.fd = w->ifd, .events = POLLIN};
      int pr = poll(&pfd, 1, timeout_ms);
      if (pr == 0)
         return 0;
      if (pr < 0)
         return (errno == EINTR) ? 0 : -1;
      ssize_t n = read(w->ifd, w->evbuf, sizeof(w->evbuf));
      if (n <= 0)
         return (n < 0 && (errno == EAGAIN || errno == EINTR)) ? 0 : -1;
      w->evlen = n;
      w->evpos = 0;
   }
   while (w->evpos < w->evlen)
   {
      struct inotify_event *ev = (struct inotify_event *)(w->evbuf + w->evpos);
      w->evpos += (ssize_t)(sizeof(*ev) + ev->len);
      /* Watch auto-removed (dir deleted/unmounted): drop the stale slot so the
       * table can't fill with dead entries and a reused wd can't mis-map to the
       * old path. IN_IGNORED carries no name (len 0), so handle it first. */
      if (ev->mask & IN_IGNORED)
      {
         for (int i = 0; i < w->ndirs; i++)
            if (w->dirs[i].wd == ev->wd)
            {
               w->dirs[i] = w->dirs[--w->ndirs];
               break;
            }
         continue;
      }
      if (ev->len == 0)
         continue;
      const char *dir = dir_for_wd(w, ev->wd);
      if (!dir)
         continue;
      char full[PATH_MAX];
      if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, ev->name) >= sizeof(full))
         continue;
      if (ev->mask & IN_ISDIR)
      {
         /* a new subdir appeared — start watching it (and its children) */
         if (ev->mask & (IN_CREATE | IN_MOVED_TO))
            add_watch_recursive(w, full);
         continue;
      }
      if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
      {
         if (hmem_md_store_name(full, w->memreal, name_out, cap) == 0)
            return 1;
      }
   }
   return 0; /* batch drained, nothing relevant this round */
}

void hmem_watch_free(hmem_watch_t *w)
{
   if (!w)
      return;
   if (w->ifd >= 0)
      close(w->ifd);
   free(w->dirs);
   free(w);
}

#else /* !__linux__ — no inotify; the watcher is a no-op backstop. */

struct hmem_watch
{
   int unused;
};

hmem_watch_t *hmem_watch_open(const char *memreal)
{
   (void)memreal;
   return NULL;
}

int hmem_watch_poll(hmem_watch_t *w, char *name_out, size_t cap, int timeout_ms)
{
   (void)w;
   (void)name_out;
   (void)cap;
   (void)timeout_ms;
   return -1;
}

void hmem_watch_free(hmem_watch_t *w)
{
   (void)w;
}

#endif /* __linux__ */

/* ---- driver: watch + import in real time -------------------------------- */

static char *watch_read_whole(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz < 0 || sz > 16 * 1024 * 1024)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t r = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[r] = '\0';
   return buf;
}

/* Import a single memory file (store name `name`) into the central store. */
static void watch_import(const char *memreal, const char *name, const char *project,
                         const char *endpoint, const char *bearer)
{
   char path[PATH_MAX];
   if ((size_t)snprintf(path, sizeof(path), "%s/%s.md", memreal, name) >= sizeof(path))
      return;
   char *content = watch_read_whole(path);
   if (!content)
      return;
   cJSON *b = cJSON_CreateObject();
   if (!b)
   {
      free(content);
      return;
   }
   cJSON_AddStringToObject(b, "project", project);
   cJSON_AddStringToObject(b, "name", name);
   cJSON_AddStringToObject(b, "type", "fact");
   cJSON_AddStringToObject(b, "body", content);
   char *bs = cJSON_PrintUnformatted(b);
   cJSON_Delete(b);
   free(content);
   if (!bs)
      return;
   int code = 0;
   cJSON *r =
       cli_http_request(endpoint, "POST", "/v1/harness_memory/upsert", bs, bearer, 15000, &code);
   free(bs);
   if (r && code >= 200 && code < 300)
      hmem_audit("import-watch", project, name, NULL);
   if (r)
      cJSON_Delete(r);
}

int harness_memory_watch_run(const char *cwd)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   const hmem_scope_t *scope = hmem_scope_for_client(getenv("AIMEE_HOOK_CLIENT"));
   if (!scope)
      return -1;

   char real[PATH_MAX];
   if (!hm_realpath((cwd && cwd[0]) ? cwd : ".", real))
      return -1;
   char slug[PATH_MAX * 2];
   hmem_slug_from_path(real, slug, sizeof(slug));

   char project[HMEM_PROJECT_KEY_MAX], rootdir[PATH_MAX];
   if (hmem_resolve_project(cwd, project, sizeof(project), rootdir, sizeof(rootdir)) != 0)
      return -1;

   char memdir[PATH_MAX];
   if ((size_t)snprintf(memdir, sizeof(memdir), "%s/%s/%s/%s", home, scope->projects_root, slug,
                        scope->memory_seg) >= sizeof(memdir))
      return -1;
   char memreal[PATH_MAX];
   if (!hm_realpath(memdir, memreal)) /* nothing to watch yet */
      return -1;

   hmem_watch_t *w = hmem_watch_open(memreal);
   if (!w)
      return -1;

   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
   {
      hmem_watch_free(w);
      return -1;
   }
   char *bearer = cli_v1_client_bearer();

   hmem_audit("watch-start", project, NULL, memreal);
   for (;;)
   {
      char name[PATH_MAX];
      int r = hmem_watch_poll(w, name, sizeof(name), 1000);
      if (r < 0)
         break;
      if (r == 1)
         watch_import(memreal, name, project, endpoint, bearer);
   }
   hmem_audit("watch-stop", project, NULL, NULL);
   free(endpoint);
   free(bearer);
   hmem_watch_free(w);
   return -1;
}
