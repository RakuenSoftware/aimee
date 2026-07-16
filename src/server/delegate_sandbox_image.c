/* delegate_sandbox_image.c: resolve which docker image a delegate's sandbox runs.
 *
 * The delegate sandbox is `--network none`; its toolchain must be baked into the
 * image at build time. That toolchain is per-project (a Rust repo needs cargo, a C
 * repo gcc/make, a docs repo nothing), so the image is resolved per delegate from,
 * most specific first: the repo's .aimee/project.yaml, a per-workspace override, or
 * the global default. This phase resolves the pre-baked `image:` form only. */

#include "delegate_sandbox_image.h"

#include "aimee.h" /* MAX_PATH_LEN */
#include "cJSON.h"
#include "config.h"
#include "guardrails.h" /* git_repo_root */
#include "util.h"       /* safe_strdup */
#include "yaml.h"       /* yaml_parse */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest .aimee/project.yaml we will read (1 MiB); a contract file is tiny. */
#define PROJECT_YAML_MAX (1u << 20)

/* Read <git-root-of-cwd>/.aimee/project.yaml and return a malloc'd copy of its
 * `sandbox.image` string, or NULL when absent/unreadable/without the field. */
static char *project_yaml_sandbox_image(const char *cwd)
{
   char root[MAX_PATH_LEN];
   if (git_repo_root(cwd, root, sizeof(root)) != 0)
      return NULL;

   char path[MAX_PATH_LEN];
   if (snprintf(path, sizeof(path), "%s/.aimee/project.yaml", root) >= (int)sizeof(path))
      return NULL;

   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long n = ftell(fp);
   if (n <= 0 || n > (long)PROJECT_YAML_MAX || fseek(fp, 0, SEEK_SET) != 0)
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

   cJSON *doc = yaml_parse(buf);
   free(buf);
   if (!doc)
      return NULL;

   char *image = NULL;
   cJSON *sandbox = cJSON_GetObjectItemCaseSensitive(doc, "sandbox");
   if (cJSON_IsObject(sandbox))
   {
      cJSON *img = cJSON_GetObjectItemCaseSensitive(sandbox, "image");
      if (cJSON_IsString(img) && img->valuestring[0])
         image = safe_strdup(img->valuestring);
   }
   cJSON_Delete(doc);
   return image;
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

   /* 1. Repo contract: <git-root>/.aimee/project.yaml `sandbox.image`. */
   char *proj = project_yaml_sandbox_image(cwd);
   if (proj)
   {
      snprintf(out, cap, "%s", proj);
      free(proj);
      return 0;
   }

   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;

   /* 2. Per-workspace override — the most specific (longest) matching root wins. */
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

   /* 3. Global default. */
   if (cfg.delegate_sandbox_image[0])
   {
      snprintf(out, cap, "%s", cfg.delegate_sandbox_image);
      return 0;
   }

   return -1;
}
