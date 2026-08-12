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

#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/delegate_sandbox_image.h>

#include "aimee.h" /* MAX_PATH_LEN */
#include "cJSON.h"
#include "config.h"
#include "guardrails.h"            /* git_repo_root */
#include "harness_memory_common.h" /* hmem_sha256_hex, HMEM_HASH_HEX_LEN */
#include "sandbox_learned.h"       /* sandbox_learned_load — pre-bake the learned toolchain */
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
#define SBX_DEFAULT_BASE        "ubuntu:22.04" /* matches the docker backend default base */
#define DOCKERFILE_MAX          8192
#define DOCKER_BUILD_TIMEOUT_MS (10 * 60 * 1000) /* apt installs can be slow */

typedef struct
{
   char image[256];               /* pre-baked: use as-is */
   char from[128];                /* build: base image */
   char dockerfile[MAX_PATH_LEN]; /* build: path to a Dockerfile (repo-relative or abs) */
   char *packages_df;             /* build: generated Dockerfile text (from+packages); owned */
   /* ...and the tag naming it. Both come from ONE answer: a tag derived
    * separately from the text it is supposed to name is how an image gets
    * built under a name that describes something else. */
   char packages_tag[SBX_TAG_MAX];
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
static int ensure_built(const char *tag, const char *dockerfile_text, char *out, size_t cap)
{
   if (!tag || !tag[0] || !dockerfile_text)
      return -1;

   pthread_mutex_lock(&g_build_lock);
   int ok = docker_image_exists(tag) || docker_build(tag, dockerfile_text) == 0;
   pthread_mutex_unlock(&g_build_lock);
   if (!ok)
      return -1;
   snprintf(out, cap, "%s", tag);
   return 0;
}

/* --- cache management (list + gc of aimee-sbx:* images) --- */

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

#define SBX_IMG_MAX 512
typedef struct
{
   char tag[128];
   char id[80];
   char created[48];
   char size[32];
   int in_use;
} sbx_img_t;

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
      im->in_use = !ps_ok || sbx_ref_in_use(im->tag, used) || sbx_ref_in_use(im->id, used);
      (*n)++;
   }
   free(out);
   free(used);
   /* NOT sorted here. Recency ordering is part of the keep-the-most-recent
    * rule, so the module does it and returns verdicts in the order it was
    * given. Sorting again here would be a second ordering to disagree with. */
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
   if (s)
      return s;
   char *empty = malloc(3); /* never embed NULL — hand back an empty array */
   if (empty)
      memcpy(empty, "[]", 3);
   return empty;
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

   /* The whole inventory in one call: the decision is positional -- "keep the
    * keep_min most recent" -- so the ORDERING is part of the rule and is done
    * module-side. Nothing here re-sorts; the verdicts come back in the order
    * the images were sent. */
   size_t req_cap = AIMEE_DELEGATES_IMGGC_HEADER_LEN + (size_t)n * (12 + 2 * 512) + 64;
   size_t resp_cap = 8 + (size_t)n * 64 + 64;
   uint8_t *gc_req = malloc(req_cap);
   uint8_t *gc_resp = malloc(resp_cap);
   size_t gc_resp_len = 0;
   int judged = -1;
   if (gc_req && gc_resp)
   {
      size_t at = aimee_delegates_imggc_request_begin((unsigned)n, keep_min, now, max_age_secs,
                                                      gc_req, req_cap);
      for (int i = 0; at && i < n; i++)
         at = aimee_delegates_imggc_request_add(gc_req, req_cap, at, imgs[i].tag, imgs[i].created,
                                                imgs[i].in_use);
      if (at)
         judged = delegate_image_gc_judge(gc_req, at, gc_resp, resp_cap, &gc_resp_len);
   }
   free(gc_req);
   if (judged != 0)
   {
      /* No verdict: keep everything. An image kept costs disk; an image deleted
       * on a policy nothing applied costs a rebuild of something that may be in
       * use right now. */
      free(gc_resp);
      free(imgs);
      cJSON_Delete(arr);
      return -1;
   }

   for (int i = 0; i < n; i++)
   {
      char reason_buf[64] = "";
      int remove = 0;
      if (aimee_delegates_imggc_response_at(gc_resp, gc_resp_len, (unsigned)i, &remove, reason_buf,
                                            sizeof(reason_buf)) != 0)
      {
         remove = 0;
         snprintf(reason_buf, sizeof(reason_buf), "unjudged");
      }
      const char *reason = reason_buf;

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
   free(gc_resp);
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
         char df[DOCKERFILE_MAX], tag[SBX_TAG_MAX];
         if (delegate_image_spec_resolve(from->valuestring, argv, argc, NULL, tag, sizeof(tag), df,
                                         sizeof(df)) == 0)
         {
            out->packages_df = safe_strdup(df);
            snprintf(out->packages_tag, sizeof(out->packages_tag), "%s", tag);
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

/* Layer the project's learned apt toolchain onto `base`, building
 * `FROM <base> RUN apt-get install -y <learned>` and writing the derived image tag
 * to out[cap]. Returns 0 if a derived image was built (out = derived tag), or -1 if
 * there is nothing to overlay OR the derived build failed — in which case the caller
 * falls back to `base` (best-effort: the runtime package-access path still covers the
 * delegate, so a bad learned package must never strand it). */
static int apply_learned_overlay(const char *cwd, const char *base, char *out, size_t cap)
{
   char git_root[MAX_PATH_LEN];
   if (git_repo_root(cwd, git_root, sizeof(git_root)) != 0)
      return -1;
   char pk[SBX_LEARN_MAX][SBX_PKG_MAX];
   int n = sandbox_learned_load(git_root, pk, SBX_LEARN_MAX);
   if (n <= 0)
      return -1;
   const char *argv[SBX_LEARN_MAX];
   for (int i = 0; i < n; i++)
      argv[i] = pk[i];
   char df[DOCKERFILE_MAX], tag[SBX_TAG_MAX], built[SBX_TAG_MAX];
   if (delegate_image_spec_resolve(base, argv, n, NULL, tag, sizeof(tag), df, sizeof(df)) != 0)
      return -1;
   if (ensure_built(tag, df, built, sizeof(built)) != 0)
      return -1; /* build failed -> caller uses base */
   snprintf(out, cap, "%s", built);
   return 0;
}

int delegate_sandbox_resolve_image(const char *cwd, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!cwd || !cwd[0])
      return -1;

   char base[256] = "";
   int have_base = 0;
   /* Whether the learned toolchain may layer on top. False for an explicitly declared
    * project.yaml `image:`/`dockerfile:` (respect the author's exact image); true for a
    * `from`+`packages` build (augment it) and for the generic workspace/global/default
    * scopes (synthesize when the project declared no spec). */
   int overlay_ok = 1;

   /* 1. Repo contract: <git-root>/.aimee/project.yaml `sandbox` (image | build). */
   {
      char repo_root[MAX_PATH_LEN];
      sandbox_spec_t spec;
      if (project_yaml_sandbox_spec(cwd, repo_root, sizeof(repo_root), &spec) == 0)
      {
         if (spec.image[0])
         {
            snprintf(base, sizeof(base), "%s", spec.image);
            have_base = 1;
            overlay_ok = 0; /* explicit image: use exactly as authored */
         }
         else if (spec.packages_df)
         {
            /* The tag came back with the text it names; it is not re-derived. */
            char tag[SBX_TAG_MAX];
            if (ensure_built(spec.packages_tag, spec.packages_df, tag, sizeof(tag)) == 0)
            {
               snprintf(base, sizeof(base), "%s", tag);
               have_base = 1; /* from+packages: learned may augment on top */
            }
         }
         else if (spec.dockerfile[0])
         {
            overlay_ok = 0; /* explicit Dockerfile: respect it, even if it fails to build */
            /* A Dockerfile the operator committed: carried whole to be NAMED,
             * because the tag's shape must exist in one place or the same
             * content resolves to two names and nothing is ever reused. */
            char *df = read_dockerfile(repo_root, spec.dockerfile);
            char tag[SBX_TAG_MAX], named[DOCKERFILE_MAX];
            if (df &&
                delegate_image_spec_resolve(NULL, NULL, 0, df, tag, sizeof(tag), named,
                                            sizeof(named)) == 0 &&
                ensure_built(tag, named, tag, sizeof(tag)) == 0)
            {
               snprintf(base, sizeof(base), "%s", tag);
               have_base = 1;
            }
            free(df);
         }
         sandbox_spec_free(&spec);
         /* A declared-but-unbuildable spec falls through to the lower scopes rather
          * than silently dropping the delegate to the default image with no signal;
          * the build failure is surfaced by docker's own logs. */
      }
   }

   if (!config_present())
   {
      if (have_base)
      {
         snprintf(out, cap, "%s", base);
         return 0;
      }
      return -1;
   }

   /* 2. Per-workspace override (image ref only) — longest matching root wins. */
   if (!have_base)
   {
      int best = -1;
      size_t best_len = 0;
      for (int i = 0; i < config_workspace_count(); i++)
      {
         if (!config_workspace_sandbox_image(i)[0])
            continue;
         if (cwd_under_root(cwd, config_workspaces(i)))
         {
            size_t len = strlen(config_workspaces(i));
            if (len > best_len)
            {
               best = i;
               best_len = len;
            }
         }
      }
      if (best >= 0)
      {
         snprintf(base, sizeof(base), "%s", config_workspace_sandbox_image(best));
         have_base = 1;
      }
   }

   /* 3. Global default (image ref only). */
   if (!have_base && config_delegate_sandbox_image()[0])
   {
      snprintf(base, sizeof(base), "%s", config_delegate_sandbox_image());
      have_base = 1;
   }

   /* 4. Learned toolchain: layer the project's captured apt packages onto the base
    * (synthesizing FROM the backend default when no base was resolved). Best-effort —
    * a failed derived build falls back to the base image below. */
   if (config_delegate_sandbox_learn_packages() && overlay_ok)
   {
      const char *overlay_base = have_base ? base : SBX_DEFAULT_BASE;
      if (apply_learned_overlay(cwd, overlay_base, out, cap) == 0)
         return 0;
   }

   /* 5. The resolved base, or none (the backend applies its own default). */
   if (have_base)
   {
      snprintf(out, cap, "%s", base);
      return 0;
   }
   return -1;
}
