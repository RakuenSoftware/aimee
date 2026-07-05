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
#include <unistd.h> /* unlink, fsync */
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

int hmem_md_store_name(const char *fp, const char *memreal, char *out, size_t cap)
{
   if (!fp || !memreal || !out || cap == 0)
      return -1;
   size_t fl = strlen(fp), ml = strlen(memreal);
   /* Case-SENSITIVE ".md": on a case-sensitive fs "Foo.MD" is a distinct file
    * and must not be folded onto the canonical lowercase store name. */
   if (fl < 3 || memcmp(fp + fl - 3, ".md", 3) != 0)
      return -1;
   /* fp must sit strictly under memreal: longer than memreal, shared prefix, and
    * a '/' boundary. The `fl > ml` guard makes fp[ml] unambiguously in-bounds and
    * rejects a sibling like "<memreal>X/foo.md" (boundary byte is 'X', not '/'). */
   if (fl <= ml || strncmp(fp, memreal, ml) != 0 || fp[ml] != '/')
      return -1;
   const char *rel = fp + ml + 1;
   size_t rel_len = fl - (ml + 1) - 3; /* minus ".md" */
   if (rel_len == 0 || rel_len >= cap)
      return -1;
   memcpy(out, rel, rel_len);
   out[rel_len] = '\0';
   /* Nested names legitimately keep '/' (e.g. "topics/a") and a literal ".." mid-
    * token is fine (e.g. "v1..0"), but a ".." that IS a whole path component could
    * escape the root on write-back, so reject only that. */
   for (const char *p = strstr(out, ".."); p; p = strstr(p + 2, ".."))
   {
      int at_start = (p == out) || (p[-1] == '/');
      int at_end = (p[2] == '\0') || (p[2] == '/');
      if (at_start && at_end)
         return -1;
   }
   /* "MEMORY" is the harness index (MEMORY.md), matched EXACT-case — a user file
    * "memory.md" on a case-sensitive fs is a real memory, not the index. */
   if (strcmp(out, "MEMORY") == 0)
      return -1;
   return 0;
}

/* True if `name` is one of the `nk` strings in `known`. */
static int name_known(char *const *known, int nk, const char *name)
{
   for (int i = 0; i < nk; i++)
      if (strcmp(known[i], name) == 0)
         return 1;
   return 0;
}

/* Walk `absdir` recursively (absdir is always under the resolved memreal). Any
 * regular *.md file whose store name is not in `known` is a disk-only memory
 * (pre-existing file, external edit, or a fail-open write whose spill was lost):
 * import it into the store via upsert so DB1 becomes the union of disk + store.
 * lstat (not stat) means symlinked dirs/files are skipped — a symlink can't make
 * the scan escape memreal or import something outside it. */
static void import_orphans(const char *absdir, const char *memreal, char *const *known, int nk,
                           const char *project, const char *endpoint, const char *bearer,
                           int *count)
{
   DIR *d = opendir(absdir);
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (e->d_name[0] == '.') /* skip dotfiles, "." and ".." */
         continue;
      char fp[PATH_MAX];
      if ((size_t)snprintf(fp, sizeof(fp), "%s/%s", absdir, e->d_name) >= sizeof(fp))
         continue;
      struct stat st;
      if (lstat(fp, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         import_orphans(fp, memreal, known, nk, project, endpoint, bearer, count);
         continue;
      }
      if (!S_ISREG(st.st_mode))
         continue;
      char name[PATH_MAX];
      if (hmem_md_store_name(fp, memreal, name, sizeof(name)) != 0)
         continue;
      if (name_known(known, nk, name))
         continue;
      char *content = read_whole(fp);
      if (!content)
         continue;
      cJSON *b = cJSON_CreateObject();
      if (!b)
      {
         free(content);
         continue;
      }
      cJSON_AddStringToObject(b, "project", project);
      cJSON_AddStringToObject(b, "name", name);
      cJSON_AddStringToObject(b, "type", "fact");
      cJSON_AddStringToObject(b, "body", content);
      char *bs = cJSON_PrintUnformatted(b);
      cJSON_Delete(b);
      free(content);
      if (!bs)
         continue;
      int code = 0;
      cJSON *r =
          cli_http_request(endpoint, "POST", "/v1/harness_memory/upsert", bs, bearer, 15000, &code);
      free(bs);
      if (r && code >= 200 && code < 300)
      {
         hmem_audit("import", project, name, NULL);
         (*count)++;
      }
      if (r)
         cJSON_Delete(r);
   }
   closedir(d);
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

   /* Ensure the memory dir exists and resolve it up front, so we can confine
    * every write under the *real* directory (a symlinked component can't
    * redirect us out). Needed before we process any page. */
   mkdir_p(memdir);
   char memreal[PATH_MAX];
   if (!hm_realpath(memdir, memreal))
   {
      free(endpoint);
      free(bearer);
      return -1;
   }

   /* Every name DB1 knows about — LIVE *and* TOMBSTONED. The disk scan below
    * imports any *.md whose name is absent from this set; including tombstoned
    * names here is what stops a tombstone from being resurrected (a deleted
    * memory whose file lingers stays "known", so it is never re-imported).
    * known_ok guards completeness: if any insertion fails (OOM), the set is no
    * longer authoritative, so we skip the import pass entirely rather than risk
    * resurrecting a tombstone we failed to record. list_ok is the same guard for
    * a failed page — a partial known set must not drive the import. */
   char **known = NULL;
   int nk = 0, kcap = 0, known_ok = 1;
   int n = 0, removed = 0, list_ok = 1;

   /* Page through the store (include_deleted so tombstoned rows come back — we
    * materialize live rows and *remove* the file for each tombstone; DB1 is
    * authoritative). The full set, bodies included, can exceed one RPC response
    * buffer, so follow next_offset until has_more is false rather than asking
    * for everything at once (an over-large single response truncates and 502s,
    * which used to abort this hydrate before the import pass). */
   for (int offset = 0;;)
   {
      cJSON *body = cJSON_CreateObject();
      cJSON_AddStringToObject(body, "project", project);
      cJSON_AddNumberToObject(body, "include_deleted", 1);
      cJSON_AddNumberToObject(body, "offset", offset);
      char *body_s = cJSON_PrintUnformatted(body);
      cJSON_Delete(body);

      int status = 0;
      cJSON *resp = body_s ? cli_http_request(endpoint, "POST", "/v1/harness_memory/list", body_s,
                                              bearer, 15000, &status)
                           : NULL;
      free(body_s);
      if (!resp || status < 200 || status >= 300)
      {
         if (resp)
            cJSON_Delete(resp);
         list_ok = 0;
         break;
      }

      cJSON *mems = cJSON_GetObjectItemCaseSensitive(resp, "memories");
      cJSON *m = NULL;
      cJSON_ArrayForEach(m, mems)
      {
         const char *name = jstr(m, "name");
         const char *btext = jstr(m, "body");
         const char *del = jstr(m, "deleted_at");
         if (!name || !name[0] || name[0] == '/' || strstr(name, "..")) /* never escape memdir */
            continue;
         if (nk == kcap)
         {
            int nc = kcap ? kcap * 2 : 32;
            char **t = realloc(known, (size_t)nc * sizeof(*t));
            if (t)
            {
               known = t;
               kcap = nc;
            }
         }
         if (nk < kcap)
         {
            char *dup = strdup(name);
            if (dup)
               known[nk++] = dup;
            else
               known_ok = 0;
         }
         else
         {
            known_ok = 0;
         }
         char target[PATH_MAX];
         if ((size_t)snprintf(target, sizeof(target), "%s/%s.md", memdir, name) >= sizeof(target))
            continue;
         if (del && del[0]) /* tombstoned: ensure the on-disk file is gone */
         {
            if (target_confined(target, memreal) && unlink(target) == 0)
            {
               removed++;
               hmem_audit("tombstone-removed", project, name, NULL);
            }
            continue;
         }
         mkdir_parents(target);
         if (target_confined(target, memreal) && write_file(target, btext ? btext : "") == 0)
            n++;
      }

      cJSON *hm = cJSON_GetObjectItemCaseSensitive(resp, "has_more");
      cJSON *no = cJSON_GetObjectItemCaseSensitive(resp, "next_offset");
      int has_more = cJSON_IsBool(hm) && cJSON_IsTrue(hm);
      int next_offset = cJSON_IsNumber(no) ? (int)no->valuedouble : offset;
      cJSON_Delete(resp);
      /* Stop on the last page; the <= guard also prevents an infinite loop if a
       * misbehaving server never advances the cursor. */
      if (!has_more || next_offset <= offset)
         break;
      offset = next_offset;
   }

   /* Import disk-only memory files the store has never seen (pre-existing files,
    * external edits, or fail-open writes whose spill was lost). Skipped when the
    * known set is incomplete — see known_ok / list_ok above. */
   int imported = 0;
   if (list_ok && known_ok)
      import_orphans(memreal, memreal, known, nk, project, endpoint, bearer, &imported);

   for (int i = 0; i < nk; i++)
      free(known[i]);
   free(known);
   free(endpoint);
   free(bearer);

   if (n > 0 || consumed > 0 || removed > 0 || imported > 0)
   {
      char detail[160];
      snprintf(detail, sizeof(detail),
               "hydrated=%d spills_consumed=%d tombstones_removed=%d imported=%d", n, consumed,
               removed, imported);
      hmem_audit("reconcile", project, NULL, detail);
   }
   return n;
}
