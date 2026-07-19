/* wfe_router_catalog.c -- the router catalog I/O layer: built-in read-only
 * converse/research lanes + every $AIMEE_HOME/workflows/<name>.yaml's router metadata.
 * Kept out of wfe_router.c so the decision core stays I/O-free and unit-testable.
 */
#include "wfe_router.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_home.h"
#include "cJSON.h"
#include "yaml.h"

/* O_NOFOLLOW/O_CLOEXEC are POSIX; on non-POSIX (Windows/MinGW) degrade to 0. The
 * symlink-escape hardening they provide is Linux-deploy-only. */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static int obj_true(const cJSON *root, const char *key)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
   return it && (cJSON_IsTrue(it) ||
                 (cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, "true") == 0));
}

static const char *obj_str(const cJSON *root, const char *key)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static void add_lane(wfe_router_catalog_t *c, const char *id, int is_default, int read_only)
{
   if (c->n >= WFE_ROUTER_MAX_WF)
      return;
   wfe_router_wf_t *w = &c->wf[c->n++];
   memset(w, 0, sizeof *w);
   snprintf(w->id, sizeof w->id, "%s", id);
   w->is_default = is_default;
   w->read_only = read_only;
}

static int ends_yaml(const char *n)
{
   size_t l = strlen(n);
   return l > 5 && strcmp(n + l - 5, ".yaml") == 0;
}

/* Read an already-open fd (TOCTOU-safe: the caller open()s with O_NOFOLLOW and
 * fstat()s the same fd, so no symlink/path swap can occur between the type check
 * and the read). Returns a NUL-terminated buffer or NULL. */
static char *read_fd(int fd, long sz)
{
   if (sz < 0 || sz > (4 << 20)) /* 4MB sanity cap */
      return NULL;
   char *b = malloc((size_t)sz + 1);
   if (!b)
      return NULL;
   long off = 0;
   ssize_t rd;
   while (off < sz && (rd = read(fd, b + off, (size_t)(sz - off))) > 0)
      off += rd;
   b[off] = '\0';
   return b;
}

int wfe_router_catalog_load(wfe_router_catalog_t *out, char *err, size_t errlen)
{
   if (!out)
      return -1;
   if (err && errlen)
      err[0] = '\0';
   memset(out, 0, sizeof *out);
   /* built-in read-only lanes; research is the safe default. */
   add_lane(out, "converse", 0, 1);
   add_lane(out, "research", 1, 1);

   char dir[1024];
   int dn = snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   if (dn < 0 || (size_t)dn >= sizeof dir)
      return wfe_router_catalog_validate(out, err,
                                         errlen); /* home path too long -> built-ins only */
   DIR *d = opendir(dir);
   if (d)
   {
      struct dirent *e;
      while ((e = readdir(d)) != NULL)
      {
         if (!ends_yaml(e->d_name))
            continue;
         char path[2048];
         int pn = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
         if (pn < 0 || (size_t)pn >= sizeof path)
            continue; /* truncated path -> skip (don't operate on a wrong path) */
         /* O_NOFOLLOW makes the open fail (ELOOP) on a symlink, so a symlink in
          * the workflows dir cannot pull in a file from outside the trusted root;
          * fstat on the returned fd is TOCTOU-safe (same file, no path re-lookup). */
         int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
         if (fd < 0)
            continue;
         struct stat st;
         if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
         {
            close(fd);
            continue;
         }
         char *buf = read_fd(fd, (long)st.st_size);
         close(fd);
         if (!buf)
            continue;
         cJSON *root = yaml_parse(buf);
         free(buf);
         if (!root)
            continue;
         const char *name = obj_str(root, "name");
         /* validate the id charset+length (not just non-empty): a bad name is
          * skipped, never truncated into a colliding/injection-prone id. */
         if (name && wfe_router_id_valid(name) && !wfe_router_find(out, name) &&
             out->n < WFE_ROUTER_MAX_WF)
         {
            if (obj_true(root, "default"))
            {
               snprintf(err, errlen,
                        "workflow '%s' must not set 'default' (research is the built-in default)",
                        name);
               cJSON_Delete(root);
               closedir(d);
               return -1;
            }
            wfe_router_wf_t *w = &out->wf[out->n++];
            memset(w, 0, sizeof *w);
            snprintf(w->id, sizeof w->id, "%s", name);
            w->read_only = obj_true(root, "read_only");
            w->enforced = obj_true(root, "enforced"); /* S2: aimee-enforced workflow */
            const cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "intent_tags");
            if (tags && cJSON_IsArray(tags))
            {
               const cJSON *t = NULL;
               cJSON_ArrayForEach(t, tags)
               {
                  if (w->n_tags >= WFE_ROUTER_MAX_TAGS)
                     break;
                  if (cJSON_IsString(t) && t->valuestring && t->valuestring[0])
                     snprintf(w->tags[w->n_tags++], WFE_ROUTER_TAG_LEN, "%s", t->valuestring);
               }
            }
         }
         cJSON_Delete(root);
      }
      closedir(d);
   }
   return wfe_router_catalog_validate(out, err, errlen);
}
