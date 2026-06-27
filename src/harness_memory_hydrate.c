/* harness_memory_hydrate.c — see harness_memory_hydrate.h. */

#include "harness_memory_hydrate.h"

#include "cJSON.h"
#include "cli_client.h" /* cli_http_request, cli_v1_client_endpoint/bearer */
#include "harness_memory_audit.h"
#include "harness_memory_common.h"
#include "harness_memory_scope.h"
#include "harness_memory_spill.h"
#include "platform_path.h" /* platform_mkdir_p (portable mkdir -p) */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* Portable canonicalization: POSIX realpath resolves symlinks; on Windows
 * _fullpath returns an absolute path (no AF_UNIX-style symlink resolution, which
 * matches Windows' privileged-only symlink model). Both return non-NULL on
 * success and write the absolute path to `out`. */
#ifdef _WIN32
#define hm_realpath(p, out) _fullpath((out), (p), PATH_MAX)
#else
#define hm_realpath(p, out) realpath((p), (out))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void hmem_slug_from_path(const char *abspath, char *out, size_t cap)
{
   size_t j = 0;
   if (!abspath || cap == 0)
   {
      if (cap)
         out[0] = '\0';
      return;
   }
   for (size_t i = 0; abspath[i] && j + 1 < cap; i++)
   {
      char c = abspath[i];
      int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      out[j++] = alnum ? c : '-';
   }
   out[j] = '\0';
}

/* mkdir -p for the parent directories of a file path (best-effort). */
static void mkdir_parents(const char *file_path)
{
   char buf[PATH_MAX];
   snprintf(buf, sizeof(buf), "%s", file_path);
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         platform_mkdir_p(buf, 0700);
         *p = '/';
      }
   }
}

/* mkdir -p for a directory path (best-effort). */
static void mkdir_p(const char *dir)
{
   char buf[PATH_MAX];
   if ((size_t)snprintf(buf, sizeof(buf), "%s/", dir) >= sizeof(buf))
      return;
   mkdir_parents(buf);
}

/* Is the resolved parent dir of `target` confined under realpath(memdir)?
 * Defends against a symlinked component redirecting the write outside the
 * memory tree (parity with P3's re-materialize). */
static int target_confined(const char *target, const char *memreal)
{
   char dir[PATH_MAX];
   const char *base = strrchr(target, '/');
   if (!base)
      return 0;
   snprintf(dir, sizeof(dir), "%.*s", (int)(base - target), target);
   char dreal[PATH_MAX];
   if (!hm_realpath(dir, dreal))
      return 0;
   size_t n = strlen(memreal);
   return strncmp(dreal, memreal, n) == 0 && (dreal[n] == '/' || dreal[n] == '\0');
}

static const char *jstr(cJSON *o, const char *k)
{
   cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
   return (i && cJSON_IsString(i)) ? i->valuestring : NULL;
}

/* Atomic write: temp + fsync + rename, so a concurrent reader never sees a
 * partial hydrated file (parity with P3's re-materialize). */
static int write_file(const char *path, const char *content)
{
   mkdir_parents(path);
   size_t len = content ? strlen(content) : 0;
#ifndef _WIN32
   char dir[PATH_MAX];
   const char *base = strrchr(path, '/');
   if (base)
      snprintf(dir, sizeof(dir), "%.*s", (int)(base - path), path);
   else
      snprintf(dir, sizeof(dir), ".");
   char tmpl[PATH_MAX];
   if ((size_t)snprintf(tmpl, sizeof(tmpl), "%s/.hmem_hyd_XXXXXX", dir) >= sizeof(tmpl))
      return -1;
   int fd = mkstemp(tmpl);
   if (fd < 0)
      return -1;
   ssize_t w = (content && len) ? write(fd, content, len) : 0;
   fsync(fd);
   close(fd);
   if (w < 0 || (size_t)w != len || rename(tmpl, path) != 0)
   {
      unlink(tmpl);
      return -1;
   }
   return 0;
#else
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t w = content ? fwrite(content, 1, len, f) : 0;
   fclose(f);
   return (w == len) ? 0 : -1;
#endif
}

static char *read_whole(const char *path)
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

/* Replay any spilled writes (from a server-down fail-open) into the store, then
 * delete each consumed spill. Returns count consumed. */
static int consume_spills(const char *project, const char *endpoint, const char *bearer)
{
   char dir[PATH_MAX];
   if (hmem_spill_dir(project, dir, sizeof(dir)) != 0)
      return 0;
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)))
   {
      if (e->d_name[0] == '.') /* skips ., .., and .spill_ in-progress temps */
         continue;
      char fp[PATH_MAX];
      if ((size_t)snprintf(fp, sizeof(fp), "%s/%s", dir, e->d_name) >= sizeof(fp))
         continue;
      char *txt = read_whole(fp);
      if (!txt)
         continue;
      cJSON *env = cJSON_Parse(txt);
      free(txt);
      if (!env) /* corrupt/partial spill — leave it for inspection, don't replay */
      {
         hmem_audit("spill-corrupt", project, e->d_name, NULL);
         continue;
      }
      const char *p = jstr(env, "project"), *nm = jstr(env, "name"), *ty = jstr(env, "type"),
                 *bd = jstr(env, "body");
      if (p && nm)
      {
         cJSON *body = cJSON_CreateObject();
         cJSON_AddStringToObject(body, "project", p);
         cJSON_AddStringToObject(body, "name", nm);
         cJSON_AddStringToObject(body, "type", (ty && ty[0]) ? ty : "fact");
         cJSON_AddStringToObject(body, "body", bd ? bd : "");
         char *bs = cJSON_PrintUnformatted(body);
         cJSON_Delete(body);
         int st = 0;
         cJSON *r = bs ? cli_http_request(endpoint, "POST", "/v1/harness_memory/upsert", bs, bearer,
                                          5000, &st)
                       : NULL;
         free(bs);
         if (r && st >= 200 && st < 300)
         {
            remove(fp); /* ISO C (portable); unistd unlink isn't declared on Windows */
            hmem_audit("spill-consumed", p, nm, NULL);
            n++;
         }
         if (r)
            cJSON_Delete(r);
      }
      cJSON_Delete(env);
   }
   closedir(d);
   return n;
}

int harness_memory_hydrate(const char *cwd)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   const hmem_scope_t *scope = hmem_scope_for_client(getenv("AIMEE_HOOK_CLIENT"));
   if (!scope) /* no registered memory surface for this client */
      return -1;
   char real[PATH_MAX];
   if (!hm_realpath((cwd && cwd[0]) ? cwd : ".", real))
      return -1;
   char slug[PATH_MAX * 2];
   hmem_slug_from_path(real, slug, sizeof(slug));

   char project[256], rootdir[PATH_MAX];
   if (hmem_resolve_project(cwd, project, sizeof(project), rootdir, sizeof(rootdir)) != 0)
      return -1;

   char memdir[PATH_MAX];
   if ((size_t)snprintf(memdir, sizeof(memdir), "%s/%s/%s/%s", home, scope->projects_root, slug,
                        scope->memory_seg) >= sizeof(memdir))
      return -1;

   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return -1;
   char *bearer = cli_v1_client_bearer();

   /* Replay any spilled (server-down) writes into the store first, so the list
    * below reflects them. */
   int consumed = consume_spills(project, endpoint, bearer);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "project", project);
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   int status = 0;
   cJSON *resp = body_s ? cli_http_request(endpoint, "POST", "/v1/harness_memory/list", body_s,
                                           bearer, 15000, &status)
                        : NULL;
   free(body_s);
   free(endpoint);
   free(bearer);
   if (!resp || status < 200 || status >= 300)
   {
      if (resp)
         cJSON_Delete(resp);
      return (consumed > 0) ? consumed : -1;
   }

   /* Ensure the memory dir exists and resolve it, so we can confine every write
    * under the *real* directory (a symlinked component can't redirect us out). */
   mkdir_p(memdir);
   char memreal[PATH_MAX];
   if (!hm_realpath(memdir, memreal))
   {
      cJSON_Delete(resp);
      return -1;
   }

   int n = 0;
   cJSON *mems = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   cJSON *m = NULL;
   cJSON_ArrayForEach(m, mems)
   {
      const char *name = jstr(m, "name");
      const char *btext = jstr(m, "body");
      if (!name || !name[0] || name[0] == '/' || strstr(name, "..")) /* never escape memdir */
         continue;
      char target[PATH_MAX];
      if ((size_t)snprintf(target, sizeof(target), "%s/%s.md", memdir, name) >= sizeof(target))
         continue;
      mkdir_parents(target);
      if (target_confined(target, memreal) && write_file(target, btext ? btext : "") == 0)
         n++;
   }
   cJSON_Delete(resp);

   if (n > 0 || consumed > 0)
   {
      char detail[96];
      snprintf(detail, sizeof(detail), "hydrated=%d spills_consumed=%d", n, consumed);
      hmem_audit("reconcile", project, NULL, detail);
   }
   return n;
}
