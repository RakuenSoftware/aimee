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
