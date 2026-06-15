/* wfe_custom.c: the config-defined block registry ($AIMEE_HOME/workflows/
 * blocks.yaml) + node-aware typed-I/O accessors. Built-in blocks stay in
 * wfe_def.c's catalog; this adds user-defined blocks (one generic WFE_BLK_CUSTOM
 * type, per-block behavior is data). Part of CORE so the client validates too.
 *
 * Lifetime: every string the registry exposes (argv, prompt) is OWNED (strdup'd)
 * here, so the registry does not depend on the parsed YAML tree staying alive and
 * a reload/reset can never dangle a previously-returned wfe_custom_block_t field.
 */
#include "wfe_def.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_home.h"
#include "yaml.h"

#define WFE_CUSTOM_MAX 64

static wfe_custom_block_t g_blocks[WFE_CUSTOM_MAX];
static int g_count = 0;
static int g_loaded = 0;
static int g_allow_command = 0;

static wfe_artifact_type_t artifact_from_name(const char *s)
{
   if (!s)
      return -1;
   for (wfe_artifact_type_t a = WFE_ART_NONE; a < WFE_ART__COUNT; a++)
      if (strcmp(wfe_artifact_name(a), s) == 0)
         return a;
   return -1;
}

static void free_block(wfe_custom_block_t *b)
{
   for (int i = 0; i < b->argc; i++)
      free(b->argv[i]);
   free(b->prompt);
   memset(b, 0, sizeof *b);
}

void wfe_custom_registry_reset(void)
{
   for (int i = 0; i < g_count; i++)
      free_block(&g_blocks[i]);
   g_count = 0;
   g_loaded = 0;
   g_allow_command = 0;
}

int wfe_custom_count(void)
{
   return g_count;
}
const wfe_custom_block_t *wfe_custom_at(int i)
{
   return (i >= 0 && i < g_count) ? &g_blocks[i] : NULL;
}
int wfe_custom_commands_allowed(void)
{
   return g_allow_command;
}

const wfe_custom_block_t *wfe_custom_lookup(const char *name)
{
   if (!name)
      return NULL;
   for (int i = 0; i < g_count; i++)
      if (strcmp(g_blocks[i].name, name) == 0)
         return &g_blocks[i];
   return NULL;
}

/* copy a string field that must fit a fixed buffer; reject (don't truncate). */
static int fit(char *dst, size_t cap, const char *src, const char *what, const char *who, char *err,
               size_t errlen)
{
   if (strlen(src) >= cap)
   {
      snprintf(err, errlen, "custom block '%s': %s too long (max %zu)", who, what, cap - 1);
      return -1;
   }
   snprintf(dst, cap, "%s", src);
   return 0;
}

static int parse_one(const cJSON *b, wfe_custom_block_t *out, char *err, size_t errlen)
{
   memset(out, 0, sizeof *out);
   const cJSON *jn = cJSON_GetObjectItemCaseSensitive(b, "name");
   if (!cJSON_IsString(jn) || !jn->valuestring[0])
   {
      snprintf(err, errlen, "custom block missing 'name'");
      return -1;
   }
   if (fit(out->name, sizeof out->name, jn->valuestring, "name", jn->valuestring, err, errlen) != 0)
      return -1;
   /* explicit duplicate scan over already-committed blocks (clear message, and
    * independent of wfe_block_from_name's registry fall-through). out is
    * &g_blocks[g_count] (not yet committed), so [0..g_count-1] excludes it. */
   for (int i = 0; i < g_count; i++)
      if (strcmp(g_blocks[i].name, out->name) == 0)
      {
         snprintf(err, errlen, "duplicate custom block '%s'", out->name);
         return -1;
      }
   /* shadow a built-in? the registry fall-through can only return CUSTOM, which
    * the duplicate scan above already rejected, so any non-UNKNOWN is built-in. */
   if (wfe_block_from_name(out->name) != WFE_BLK_UNKNOWN)
   {
      snprintf(err, errlen, "custom block '%s' shadows a built-in", out->name);
      return -1;
   }
   const cJSON *jc = cJSON_GetObjectItemCaseSensitive(b, "consumes");
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(b, "produces");
   out->consumes = artifact_from_name(cJSON_IsString(jc) ? jc->valuestring : "none");
   out->produces = artifact_from_name(cJSON_IsString(jp) ? jp->valuestring : "none");
   if ((int)out->consumes < 0)
   {
      snprintf(err, errlen, "custom block '%s': unknown consumes type", out->name);
      return -1;
   }
   if (out->produces != WFE_ART_BRANCH && out->produces != WFE_ART_NONE)
   {
      snprintf(err, errlen, "custom block '%s': produces must be 'branch' or 'none'", out->name);
      free_block(out);
      return -1;
   }
   const cJSON *je = cJSON_GetObjectItemCaseSensitive(b, "executor");
   const char *ex = cJSON_IsString(je) ? je->valuestring : "";
   if (strcmp(ex, "command") == 0)
   {
      out->executor = WFE_EXEC_COMMAND;
      const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(b, "command");
      if (!cJSON_IsArray(cmd) || cJSON_GetArraySize(cmd) == 0)
      {
         snprintf(err, errlen, "custom block '%s': command executor needs a non-empty argv array",
                  out->name);
         return -1;
      }
      if (cJSON_GetArraySize(cmd) > WFE_CUSTOM_ARGV_MAX)
      {
         snprintf(err, errlen, "custom block '%s': command argv exceeds %d elements", out->name,
                  WFE_CUSTOM_ARGV_MAX);
         return -1;
      }
      const cJSON *arg = NULL;
      cJSON_ArrayForEach(arg, cmd)
      {
         if (!cJSON_IsString(arg))
         {
            snprintf(err, errlen,
                     "custom block '%s': command argv must be all strings (quote bare "
                     "true/false/numbers)",
                     out->name);
            free_block(out);
            return -1;
         }
         if (strlen(arg->valuestring) >= WFE_CUSTOM_ARG_MAX)
         {
            snprintf(err, errlen, "custom block '%s': command argv element too long (max %d)",
                     out->name, WFE_CUSTOM_ARG_MAX - 1);
            free_block(out);
            return -1;
         }
         out->argv[out->argc] = strdup(arg->valuestring);
         if (!out->argv[out->argc])
         {
            free_block(out);
            return -1;
         }
         out->argc++;
      }
      out->argv[out->argc] = NULL;
   }
   else if (strcmp(ex, "delegate") == 0)
   {
      out->executor = WFE_EXEC_DELEGATE;
      const cJSON *jper = cJSON_GetObjectItemCaseSensitive(b, "persona");
      const cJSON *jpr = cJSON_GetObjectItemCaseSensitive(b, "prompt");
      if (!cJSON_IsString(jper) || !cJSON_IsString(jpr))
      {
         snprintf(err, errlen, "custom block '%s': delegate executor needs persona + prompt",
                  out->name);
         return -1;
      }
      if (fit(out->persona, sizeof out->persona, jper->valuestring, "persona", out->name, err,
              errlen) != 0)
         return -1;
      out->prompt = strdup(jpr->valuestring);
      if (!out->prompt)
      {
         free_block(out);
         return -1;
      }
   }
   else
   {
      snprintf(err, errlen, "custom block '%s': executor must be 'command' or 'delegate'",
               out->name);
      return -1;
   }
   return 0;
}

int wfe_custom_registry_load(const char *path, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   wfe_custom_registry_reset();

   char *buf = NULL;
   size_t got = 0;
#ifndef _WIN32
   /* POSIX (server + Linux/macOS client): symlink-safe, operator-owned, not
    * group/world-writable (the approval-key posture). */
   int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
   {
      g_loaded = 1;
      if (errno == ENOENT)
         return 0; /* genuinely no registry file => no custom blocks */
      /* ELOOP (a planted symlink), EACCES, etc. are NOT "no registry": refuse
       * rather than silently ignore a blocked/inaccessible registry. */
      snprintf(err, errlen, "%s: cannot open (%s)", path, strerror(errno));
      return -1;
   }
   /* "operator-owned" = owned by the running uid or root, and not group/world-
    * writable (mode alone would pass a 0700 file owned by another user). */
   const uid_t me = getuid();
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0022) ||
       (st.st_uid != me && st.st_uid != 0))
   {
      close(fd);
      g_loaded = 1;
      snprintf(err, errlen, "%s: not a regular operator-owned file (wrong owner/group-writable?)",
               path);
      return -1;
   }
   /* the parent directory must also be operator-owned, else a less-privileged
    * principal could swap the file under us. */
   {
      char dir[1100];
      snprintf(dir, sizeof dir, "%s", path);
      char *slash = strrchr(dir, '/');
      if (slash == dir)
         dir[1] = '\0'; /* parent is the root directory "/" */
      else if (slash)
         *slash = '\0';
      const char *d = (slash && dir[0]) ? dir : ".";
      struct stat ds;
      if (stat(d, &ds) != 0 || !S_ISDIR(ds.st_mode) || (ds.st_mode & 0022) ||
          (ds.st_uid != me && ds.st_uid != 0))
      {
         close(fd);
         g_loaded = 1;
         snprintf(err, errlen, "%s: parent dir not operator-owned (wrong owner/group-writable?)",
                  path);
         return -1;
      }
   }
   long sz = st.st_size;
   buf = malloc((size_t)(sz > 0 ? sz : 0) + 1);
   if (!buf)
   {
      close(fd);
      g_loaded = 1;
      return -1;
   }
   /* robust read: handle partial reads + retry EINTR; never silently treat a
    * read error as an empty (no-blocks) registry. */
   size_t want = (size_t)(sz > 0 ? sz : 0);
   while (got < want)
   {
      ssize_t r = read(fd, buf + got, want - got);
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         close(fd);
         free(buf);
         g_loaded = 1;
         snprintf(err, errlen, "%s: read failed (%s)", path, strerror(errno));
         return -1;
      }
      if (r == 0)
         break; /* EOF (file shrank) */
      got += (size_t)r;
   }
   close(fd);
   buf[got] = '\0';
#else
   /* Windows thin client: no POSIX symlink/permission/ownership model applies
    * (the operator-owned posture is a server-side, POSIX concern; the Windows
    * client reads the developer's own config). Plain buffered read. */
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      g_loaded = 1;
      if (errno == ENOENT)
         return 0;
      snprintf(err, errlen, "%s: cannot open (%s)", path, strerror(errno));
      return -1;
   }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   if (sz < 0)
      sz = 0;
   fseek(f, 0, SEEK_SET);
   buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      g_loaded = 1;
      return -1;
   }
   got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
#endif

   cJSON *root = yaml_parse(buf);
   free(buf);
   if (!root || !cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      g_loaded = 1;
      snprintf(err, errlen, "blocks.yaml: parse error");
      return -1;
   }
   const cJSON *ac = cJSON_GetObjectItemCaseSensitive(root, "allow_command");
   int allow = ac && cJSON_IsTrue(ac);

   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(root, "blocks");
   int rc = 0;
   if (blocks && cJSON_IsArray(blocks))
   {
      const cJSON *b = NULL;
      cJSON_ArrayForEach(b, blocks)
      {
         if (g_count >= WFE_CUSTOM_MAX)
         {
            snprintf(err, errlen, "too many custom blocks (max %d)", WFE_CUSTOM_MAX);
            rc = -1;
            break;
         }
         /* parse_one rejects a name that duplicates an already-committed block
          * (explicit scan) or shadows a built-in. */
         if (parse_one(b, &g_blocks[g_count], err, errlen) != 0)
         {
            rc = -1;
            break;
         }
         g_count++;
      }
   }
   cJSON_Delete(root); /* registry no longer references the tree (owns its copies) */
   if (rc != 0)
   {
      wfe_custom_registry_reset();
      g_loaded = 1;
      return -1;
   }
   g_allow_command = allow;
   g_loaded = 1; /* set only on the final clean exit */
   return 0;
}

int wfe_custom_registry_ensure(char *err, size_t errlen)
{
   if (g_loaded)
      return 0;
   char path[1100];
   snprintf(path, sizeof path, "%s/workflows/blocks.yaml", aimee_home());
   return wfe_custom_registry_load(path, err, errlen);
}

/* ---- node-aware typed I/O (custom resolves via the registry) ---- */
wfe_artifact_type_t wfe_node_output(const wfe_node_t *n)
{
   if (n && n->block == WFE_BLK_CUSTOM)
   {
      const wfe_custom_block_t *c = wfe_custom_lookup(n->custom_name);
      return c ? c->produces : WFE_ART_NONE;
   }
   return wfe_block_output(n ? n->block : WFE_BLK_UNKNOWN);
}

int wfe_node_accepts_input(const wfe_node_t *n, wfe_artifact_type_t in)
{
   if (n && n->block == WFE_BLK_CUSTOM)
   {
      const wfe_custom_block_t *c = wfe_custom_lookup(n->custom_name);
      return c ? (c->consumes == in) : 0;
   }
   return wfe_block_accepts_input(n ? n->block : WFE_BLK_UNKNOWN, in);
}

int wfe_node_requires_input(const wfe_node_t *n)
{
   if (n && n->block == WFE_BLK_CUSTOM)
   {
      const wfe_custom_block_t *c = wfe_custom_lookup(n->custom_name);
      return c ? (c->consumes != WFE_ART_NONE) : 0;
   }
   return wfe_block_requires_input(n ? n->block : WFE_BLK_UNKNOWN);
}
