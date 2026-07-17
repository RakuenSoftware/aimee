/* delegate_sandbox_image.c: resolve which docker image a delegate's sandbox runs.
 *
 * The delegate sandbox is `--network none`; its toolchain must be baked into the
 * image at build time. That toolchain is per-project (a Rust repo needs cargo, a C
 * repo gcc/make, a docs repo nothing), so the image is resolved per delegate from,
 * most specific first: the repo's .aimee/project.yaml, a per-workspace override, or
 * the global default.
 *
 * A repo's .aimee/project.yaml `sandbox` block takes one of three forms:
 *   sandbox: { image: <ref> }                     - a pre-baked image, used as-is
 *   sandbox: { from: <base>, packages: [a, b] }   - aimee builds a derived image
 *   sandbox: { dockerfile: <path> }               - aimee builds that Dockerfile
 * A build runs `docker build` with network (aimee-server drives the host daemon),
 * tags the result by content hash, and reuses it on later turns; the delegate then
 * RUNS that image `--network none`. The per-workspace/global forms are `image:` only. */

#include "delegate_sandbox_image.h"

#include "aimee.h" /* MAX_PATH_LEN */
#include "cJSON.h"
#include "config.h"
#include "guardrails.h"            /* git_repo_root */
#include "harness_memory_common.h" /* hmem_sha256_hex, HMEM_HASH_HEX_LEN */
#include "util.h"                  /* safe_strdup, safe_exec_capture_* */
#include "yaml.h"                  /* yaml_parse */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROJECT_YAML_MAX        (1u << 20) /* 1 MiB; a contract file is tiny */
#define SBX_TAG_MAX             64
#define DOCKERFILE_MAX          8192
#define DOCKER_BUILD_TIMEOUT_MS (10 * 60 * 1000) /* apt installs can be slow */

typedef struct
{
   char image[256];               /* pre-baked: use as-is */
   char from[128];                /* build: base image */
   char dockerfile[MAX_PATH_LEN]; /* build: path to a Dockerfile (repo-relative or abs) */
   char *packages_df;             /* build: generated Dockerfile text (from+packages); owned */
} sandbox_spec_t;

static void sandbox_spec_free(sandbox_spec_t *s)
{
   if (s)
   {
      free(s->packages_df);
      s->packages_df = NULL;
   }
}

static const char *resolve_docker_bin(void)
{
   const char *o = getenv("AIMEE_DOCKER_BIN");
   return (o && o[0]) ? o : "docker";
}

/* --- pure helpers (also unit-tested) --- */

static int package_name_valid(const char *pkg)
{
   if (!pkg || !pkg[0])
      return 0;
   if (!(isalnum((unsigned char)pkg[0])))
      return 0;
   for (const char *c = pkg; *c; c++)
   {
      if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '+' || *c == ':' ||
            *c == '-'))
         return 0;
   }
   return 1;
}

int delegate_sandbox_dockerfile_from_packages(const char *base, const char *const *pkgs, int npkgs,
                                              char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!base || !base[0])
      return -1;
   /* Base image ref may contain '/' and ':' (registry/repo:tag) — allow those too. */
   for (const char *c = base; *c; c++)
   {
      if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '+' || *c == ':' ||
            *c == '-' || *c == '/'))
         return -1;
   }

   char pkglist[4096];
   size_t pos = 0;
   for (int i = 0; i < npkgs; i++)
   {
      if (!package_name_valid(pkgs[i]))
         return -1;
      int n = snprintf(pkglist + pos, sizeof(pkglist) - pos, "%s%s", pos ? " " : "", pkgs[i]);
      if (n < 0 || (size_t)n >= sizeof(pkglist) - pos)
         return -1;
      pos += (size_t)n;
   }

   int n;
   if (npkgs > 0)
      n = snprintf(out, cap,
                   "FROM %s\n"
                   "RUN apt-get update && apt-get install -y --no-install-recommends %s && "
                   "rm -rf /var/lib/apt/lists/*\n",
                   base, pkglist);
   else
      n = snprintf(out, cap, "FROM %s\n", base);
   return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

void delegate_sandbox_content_tag(const char *content, char *tag, size_t cap)
{
   char hex[HMEM_HASH_HEX_LEN];
   hmem_sha256_hex(content ? content : "", content ? strlen(content) : 0, hex);
   hex[12] = '\0';
   snprintf(tag, cap, "aimee-sbx:%s", hex);
}

/* --- docker ops (impure) --- */

static int docker_image_exists(const char *tag)
{
   const char *argv[] = {resolve_docker_bin(), "image", "inspect", tag, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, 256);
   free(out);
   return rc == 0;
}

/* Build `dockerfile_text` into image `tag` with an empty context. Returns 0 on
 * success. Network is available at build time (the delegate RUN is where it is
 * removed). Serialised by the caller's lock so two turns don't race the same tag. */
static int docker_build(const char *tag, const char *dockerfile_text)
{
   char ctx[] = "/tmp/aimee-sbx-build-XXXXXX";
   if (!mkdtemp(ctx))
      return -1;
   char dfpath[MAX_PATH_LEN];
   snprintf(dfpath, sizeof(dfpath), "%s/Dockerfile", ctx);
   FILE *fp = fopen(dfpath, "w");
   int rc = -1;
   if (fp)
   {
      if (fputs(dockerfile_text, fp) >= 0 && fclose(fp) == 0)
      {
         const char *argv[] = {resolve_docker_bin(), "build", "-t", tag, "-f", dfpath, ctx, NULL};
         char *out = NULL;
         rc = safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &out, 65536,
                                                DOCKER_BUILD_TIMEOUT_MS);
         free(out);
      }
      fp = NULL;
   }
   unlink(dfpath);
   rmdir(ctx);
   return rc;
}

static pthread_mutex_t g_build_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ensure the build spec's image exists (build once, cached), writing its tag to
 * out[cap]. Returns 0 on success, -1 on failure. */
static int ensure_built(const char *dockerfile_text, char *out, size_t cap)
{
   char tag[SBX_TAG_MAX];
   delegate_sandbox_content_tag(dockerfile_text, tag, sizeof(tag));

   pthread_mutex_lock(&g_build_lock);
   int ok = docker_image_exists(tag) || docker_build(tag, dockerfile_text) == 0;
   pthread_mutex_unlock(&g_build_lock);
   if (!ok)
      return -1;
   snprintf(out, cap, "%s", tag);
   return 0;
}

/* --- cache management (list + gc of aimee-sbx:* images) --- */

/* Parse a `docker image ls` CreatedAt field ("2026-07-15 12:34:56 +0000 UTC")
 * into a UTC epoch. Docker always emits the local-daemon time with an explicit
 * offset; we read the wall-clock fields and the numeric offset and normalise to
 * UTC ourselves (no timegm/strptime, so no feature-macro or TZ dependence).
 * Returns 0 on success, -1 if the leading "Y-M-D H:M:S" does not parse. Pure. */
int delegate_sandbox_parse_created_epoch(const char *created, long long *out)
{
   if (!created || !out)
      return -1;
   int y, mo, d, h, mi, s;
   char sign = '+';
   int oh = 0, om = 0;
   int fields =
       sscanf(created, "%d-%d-%d %d:%d:%d %c%2d%2d", &y, &mo, &d, &h, &mi, &s, &sign, &oh, &om);
   if (fields < 6)
      return -1;
   if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 ||
       s > 60)
      return -1;
   /* days from 1970-01-01 to y-mo-d (proleptic Gregorian), via a civil-days algorithm. */
   int yy = (mo <= 2) ? y - 1 : y;
   int era = (yy >= 0 ? yy : yy - 399) / 400;
   unsigned yoe = (unsigned)(yy - era * 400);
   unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
   unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   long long days = (long long)era * 146097 + (long long)doe - 719468;
   long long epoch = days * 86400 + h * 3600 + mi * 60 + s;
   if (fields >= 7)
   {
      long long off = (long long)oh * 3600 + (long long)om * 60;
      epoch += (sign == '-') ? off : -off; /* subtract the offset to reach UTC */
   }
   *out = epoch;
   return 0;
}

#define SBX_IMG_MAX 512

typedef struct
{
   char tag[128];
   char id[80];
   char created[48];
   char size[32];
   long long created_epoch;
   int in_use;
} sbx_img_t;

/* True when a container (running or stopped) references image ref `ref`. `used`
 * is the newline-joined `docker ps -a` image column. Matches a whole line only,
 * so "aimee-sbx:ab" never matches "aimee-sbx:abcd". Pure. */
static int sbx_ref_in_use(const char *ref, const char *used)
{
   if (!ref || !*ref || !used)
      return 0;
   size_t rlen = strlen(ref);
   const char *p = used;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      if (len == rlen && memcmp(p, ref, rlen) == 0)
         return 1;
      if (!nl)
         break;
      p = nl + 1;
   }
   return 0;
}

static int sbx_epoch_desc(const void *a, const void *b)
{
   long long ea = ((const sbx_img_t *)a)->created_epoch;
   long long eb = ((const sbx_img_t *)b)->created_epoch;
   return (eb > ea) - (eb < ea); /* most-recent first */
}

/* Enumerate every aimee-sbx:* image, newest first, marking in-use. Returns the
 * count (>=0) into *n and 0, or -1 if the docker daemon is unreachable. */
static int sbx_collect(sbx_img_t *imgs, int cap, int *n)
{
   *n = 0;
   const char *ls[] = {resolve_docker_bin(),
                       "image",
                       "ls",
                       "--filter",
                       "reference=aimee-sbx:*",
                       "--format",
                       "{{.ID}}\t{{.Repository}}:{{.Tag}}\t{{.CreatedAt}}\t{{.Size}}",
                       NULL};
   char *out = NULL;
   if (safe_exec_capture(ls, &out, 1 << 20) != 0)
   {
      free(out);
      return -1;
   }

   char *used = NULL;
   const char *ps[] = {resolve_docker_bin(), "ps", "-a", "--format", "{{.Image}}", NULL};
   /* If `ps` fails while `ls` succeeded we cannot tell what is referenced: mark every
    * image in-use so gc removes nothing (fail-safe), while list still enumerates. */
   int ps_ok = (safe_exec_capture(ps, &used, 1 << 20) == 0);

   char *save = NULL;
   /* out may be NULL (command succeeded, no images) — strtok_r must not touch it. */
   for (char *line = out ? strtok_r(out, "\n", &save) : NULL; line && *n < cap;
        line = strtok_r(NULL, "\n", &save))
   {
      char *f_id = line;
      char *f_tag = strchr(f_id, '\t');
      if (!f_tag)
         continue;
      *f_tag++ = '\0';
      char *f_created = strchr(f_tag, '\t');
      if (!f_created)
         continue;
      *f_created++ = '\0';
      char *f_size = strchr(f_created, '\t');
      if (f_size)
         *f_size++ = '\0';

      sbx_img_t *im = &imgs[*n];
      memset(im, 0, sizeof(*im));
      snprintf(im->id, sizeof(im->id), "%s", f_id);
      snprintf(im->tag, sizeof(im->tag), "%s", f_tag);
      snprintf(im->created, sizeof(im->created), "%s", f_created);
      snprintf(im->size, sizeof(im->size), "%s", f_size ? f_size : "");
      im->created_epoch = 0;
      delegate_sandbox_parse_created_epoch(im->created, &im->created_epoch);
      im->in_use = !ps_ok || sbx_ref_in_use(im->tag, used) || sbx_ref_in_use(im->id, used);
      (*n)++;
   }
   free(out);
   free(used);
   qsort(imgs, (size_t)*n, sizeof(imgs[0]), sbx_epoch_desc);
   return 0;
}

char *delegate_sandbox_images_json(void)
{
   sbx_img_t *imgs = calloc(SBX_IMG_MAX, sizeof(*imgs));
   if (!imgs)
      return NULL;
   int n = 0;
   if (sbx_collect(imgs, SBX_IMG_MAX, &n) != 0)
   {
      free(imgs);
      return NULL;
   }
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "tag", imgs[i].tag);
      cJSON_AddStringToObject(o, "id", imgs[i].id);
      cJSON_AddStringToObject(o, "created", imgs[i].created);
      cJSON_AddStringToObject(o, "size", imgs[i].size);
      cJSON_AddBoolToObject(o, "in_use", imgs[i].in_use ? 1 : 0);
      cJSON_AddItemToArray(arr, o);
   }
   free(imgs);
   char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
   cJSON_Delete(arr);
   return s ? s : safe_strdup("[]");
}

/* Remove image `tag`. Held under g_build_lock so a removal cannot interleave with
 * ensure_built's exists-then-build on the same content-hash tag. The lock is taken
 * per-call, not across the whole gc sweep, so concurrent builds stall for one rm at
 * a time. This does NOT close the wider window: ensure_built releases the lock before
 * the delegate's `docker run`, so a reused-but-aged image can still be removed between
 * build and run — the turn then fails cleanly ("no such image") and the next turn
 * rebuilds it (content-addressed, cheap). Freshly built images are immune (they are
 * within-max-age). Returns 0 on success. */
static int docker_image_rm(const char *tag)
{
   const char *argv[] = {resolve_docker_bin(), "image", "rm", tag, NULL};
   char *out = NULL;
   pthread_mutex_lock(&g_build_lock);
   int rc = safe_exec_capture(argv, &out, 4096);
   pthread_mutex_unlock(&g_build_lock);
   free(out);
   return rc;
}

int delegate_sandbox_gc_should_remove(int in_use, int index, int keep_min, long long created_epoch,
                                      long long now, long max_age_secs, const char **reason_out)
{
   const char *reason;
   int remove = 0;
   if (in_use)
      reason = "in-use";
   else if (index < keep_min)
      reason = "kept-recent";
   else if (created_epoch > 0 && (now - created_epoch) < max_age_secs)
      reason = "within-max-age";
   else
   {
      remove = 1;
      reason = "aged-out";
   }
   if (reason_out)
      *reason_out = reason;
   return remove;
}

int delegate_sandbox_gc(long max_age_secs, int keep_min, int dry_run, char **report_json_out)
{
   if (report_json_out)
      *report_json_out = NULL;
   if (keep_min < 0)
      keep_min = 0;
   if (max_age_secs < 0)
      max_age_secs = 0;

   sbx_img_t *imgs = calloc(SBX_IMG_MAX, sizeof(*imgs));
   if (!imgs)
      return -1;
   int n = 0;
   if (sbx_collect(imgs, SBX_IMG_MAX, &n) != 0)
   {
      free(imgs);
      return -1;
   }

   long long now = (long long)time(NULL);
   int removed = 0, kept = 0;
   cJSON *arr = cJSON_CreateArray();

   /* imgs is newest-first; index < keep_min is a protected recent image. */
   for (int i = 0; i < n; i++)
   {
      const char *reason;
      int remove = delegate_sandbox_gc_should_remove(
          imgs[i].in_use, i, keep_min, imgs[i].created_epoch, now, max_age_secs, &reason);

      if (remove && !dry_run && docker_image_rm(imgs[i].tag) != 0)
      {
         remove = 0; /* rm failed (e.g. raced into use); report as kept. */
         reason = "rm-failed";
      }
      if (remove)
         removed++;
      else
         kept++;

      if (arr)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "tag", imgs[i].tag);
         cJSON_AddStringToObject(o, "size", imgs[i].size);
         cJSON_AddStringToObject(o, "reason", reason);
         cJSON_AddBoolToObject(o, "removed", (remove && !dry_run) ? 1 : 0);
         cJSON_AddItemToArray(arr, o);
      }
   }
   free(imgs);

   if (report_json_out && arr)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "removed", removed);
      cJSON_AddNumberToObject(root, "kept", kept);
      cJSON_AddBoolToObject(root, "dry_run", dry_run ? 1 : 0);
      cJSON_AddItemToObject(root, "images", arr);
      *report_json_out = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
   }
   else
   {
      cJSON_Delete(arr);
   }
   return 0;
}

/* --- project.yaml spec parsing --- */

/* Read a repo's .aimee/project.yaml `sandbox` block into `out`. Returns 0 if a
 * usable block was found, -1 otherwise. On a from+packages spec the generated
 * Dockerfile is stored in out->packages_df (caller frees via sandbox_spec_free). */
static int project_yaml_sandbox_spec(const char *cwd, char *repo_root, size_t root_cap,
                                     sandbox_spec_t *out)
{
   memset(out, 0, sizeof(*out));
   if (git_repo_root(cwd, repo_root, root_cap) != 0)
      return -1;

   char path[MAX_PATH_LEN];
   if (snprintf(path, sizeof(path), "%s/.aimee/project.yaml", repo_root) >= (int)sizeof(path))
      return -1;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return -1;
   }
   long n = ftell(fp);
   if (n <= 0 || n > (long)PROJECT_YAML_MAX || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return -1;
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return -1;
   }
   size_t rd = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[rd] = '\0';

   cJSON *doc = yaml_parse(buf);
   free(buf);
   if (!doc)
      return -1;

   int found = -1;
   cJSON *sb = cJSON_GetObjectItemCaseSensitive(doc, "sandbox");
   if (cJSON_IsObject(sb))
   {
      cJSON *image = cJSON_GetObjectItemCaseSensitive(sb, "image");
      cJSON *from = cJSON_GetObjectItemCaseSensitive(sb, "from");
      cJSON *packages = cJSON_GetObjectItemCaseSensitive(sb, "packages");
      cJSON *dockerfile = cJSON_GetObjectItemCaseSensitive(sb, "dockerfile");

      if (cJSON_IsString(image) && image->valuestring[0])
      {
         snprintf(out->image, sizeof(out->image), "%s", image->valuestring);
         found = 0;
      }
      else if (cJSON_IsString(from) && from->valuestring[0])
      {
         /* Collect package names into an argv for the pure Dockerfile generator. */
         int cnt = cJSON_IsArray(packages) ? cJSON_GetArraySize(packages) : 0;
         const char **argv = cnt > 0 ? calloc((size_t)cnt, sizeof(char *)) : NULL;
         int argc = 0;
         if (cnt > 0 && argv)
         {
            cJSON *p;
            cJSON_ArrayForEach(p, packages)
            {
               if (cJSON_IsString(p) && p->valuestring[0])
                  argv[argc++] = p->valuestring;
            }
         }
         char df[DOCKERFILE_MAX];
         if (delegate_sandbox_dockerfile_from_packages(from->valuestring, argv, argc, df,
                                                       sizeof(df)) == 0)
         {
            out->packages_df = safe_strdup(df);
            snprintf(out->from, sizeof(out->from), "%s", from->valuestring);
            found = out->packages_df ? 0 : -1;
         }
         free(argv);
      }
      else if (cJSON_IsString(dockerfile) && dockerfile->valuestring[0])
      {
         snprintf(out->dockerfile, sizeof(out->dockerfile), "%s", dockerfile->valuestring);
         found = 0;
      }
   }
   cJSON_Delete(doc);
   return found;
}

/* Read a Dockerfile (path may be repo-relative) into a malloc'd string, or NULL. */
static char *read_dockerfile(const char *repo_root, const char *df_path)
{
   char full[MAX_PATH_LEN];
   if (df_path[0] == '/')
      snprintf(full, sizeof(full), "%s", df_path);
   else
      snprintf(full, sizeof(full), "%s/%s", repo_root, df_path);
   FILE *fp = fopen(full, "r");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long n = ftell(fp);
   if (n <= 0 || n > (long)DOCKERFILE_MAX || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[rd] = '\0';
   return buf;
}

/* True when `cwd` is `ws` itself or a path beneath it. */
static int cwd_under_root(const char *cwd, const char *ws)
{
   size_t len = ws ? strlen(ws) : 0;
   return len > 0 && strncmp(cwd, ws, len) == 0 && (cwd[len] == '/' || cwd[len] == '\0');
}

int delegate_sandbox_resolve_image(const char *cwd, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!cwd || !cwd[0])
      return -1;

   /* 1. Repo contract: <git-root>/.aimee/project.yaml `sandbox` (image | build). */
   {
      char repo_root[MAX_PATH_LEN];
      sandbox_spec_t spec;
      if (project_yaml_sandbox_spec(cwd, repo_root, sizeof(repo_root), &spec) == 0)
      {
         int rc = -1;
         if (spec.image[0])
         {
            snprintf(out, cap, "%s", spec.image);
            rc = 0;
         }
         else if (spec.packages_df)
         {
            rc = ensure_built(spec.packages_df, out, cap);
         }
         else if (spec.dockerfile[0])
         {
            char *df = read_dockerfile(repo_root, spec.dockerfile);
            if (df)
            {
               rc = ensure_built(df, out, cap);
               free(df);
            }
         }
         sandbox_spec_free(&spec);
         if (rc == 0)
            return 0;
         /* A declared-but-unbuildable spec falls through to the lower scopes rather
          * than silently dropping the delegate to the default image with no signal;
          * the build failure is surfaced by docker's own logs. */
      }
   }

   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;

   /* 2. Per-workspace override (image ref only) — longest matching root wins. */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (!cfg.workspace_sandbox_image[i][0])
         continue;
      if (cwd_under_root(cwd, cfg.workspaces[i]))
      {
         size_t len = strlen(cfg.workspaces[i]);
         if (len > best_len)
         {
            best = i;
            best_len = len;
         }
      }
   }
   if (best >= 0)
   {
      snprintf(out, cap, "%s", cfg.workspace_sandbox_image[best]);
      return 0;
   }

   /* 3. Global default (image ref only). */
   if (cfg.delegate_sandbox_image[0])
   {
      snprintf(out, cap, "%s", cfg.delegate_sandbox_image);
      return 0;
   }

   return -1;
}
