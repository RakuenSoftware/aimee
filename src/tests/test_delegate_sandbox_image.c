#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "config.h"
#include "delegate_sandbox_image.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* Resolver precedence: repo .aimee/project.yaml > per-workspace override > global. */

int main(void)
{
   printf("delegate_sandbox_image: ");

   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-sbximg-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("HOME", home);
   platform_setenv("AIMEE_HOME", home);
   platform_setenv("AIMEE_NO_CACHE", "1");

   char ws_a[600];
   snprintf(ws_a, sizeof(ws_a), "%s/ws-a", home);

   /* --- config round-trip: global + per-workspace image persist --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* defaults */
      snprintf(cfg.delegate_sandbox_image, sizeof(cfg.delegate_sandbox_image), "global-img:1");
      cfg.workspace_count = 1;
      snprintf(cfg.workspaces[0], sizeof(cfg.workspaces[0]), "%s", ws_a);
      snprintf(cfg.workspace_sandbox_image[0], sizeof(cfg.workspace_sandbox_image[0]), "ws-img:1");
      assert(config_save(&cfg) == 0);

      static config_t got;
      memset(&got, 0, sizeof(got));
      assert(config_load(&got) == 0);
      assert(strcmp(got.delegate_sandbox_image, "global-img:1") == 0);
      assert(got.workspace_count == 1);
      assert(strcmp(got.workspaces[0], ws_a) == 0);
      assert(strcmp(got.workspace_sandbox_image[0], "ws-img:1") == 0);
   }

   /* --- resolve: cwd under a workspace -> its override (beats global) --- */
   {
      char cwd[700];
      snprintf(cwd, sizeof(cwd), "%s/sub/dir", ws_a); /* need not exist */
      char out[256] = "";
      assert(delegate_sandbox_resolve_image(cwd, out, sizeof(out)) == 0);
      assert(strcmp(out, "ws-img:1") == 0);
   }

   /* --- resolve: cwd outside any workspace, not a repo -> global default --- */
   {
      char out[256] = "";
      assert(delegate_sandbox_resolve_image("/nonexistent/path/xyz", out, sizeof(out)) == 0);
      assert(strcmp(out, "global-img:1") == 0);
   }

   /* --- resolve: repo .aimee/project.yaml sandbox.image wins over everything --- */
   {
      char repo[600];
      snprintf(repo, sizeof(repo), "%s/repo", home);
      char cmd[900];
      snprintf(cmd, sizeof(cmd), "git init -q '%s' && mkdir -p '%s/.aimee'", repo, repo);
      assert(system(cmd) == 0);
      char yaml_path[700];
      snprintf(yaml_path, sizeof(yaml_path), "%s/.aimee/project.yaml", repo);
      FILE *f = fopen(yaml_path, "w");
      assert(f);
      fputs("sandbox:\n  image: proj-img:1\n", f);
      fclose(f);

      char out[256] = "";
      assert(delegate_sandbox_resolve_image(repo, out, sizeof(out)) == 0);
      assert(strcmp(out, "proj-img:1") == 0);
   }

   /* --- resolve: nothing configured -> -1 (caller uses backend default) --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.delegate_sandbox_image[0] = '\0';
      cfg.workspace_count = 0;
      assert(config_save(&cfg) == 0);

      char out[256] = "sentinel";
      assert(delegate_sandbox_resolve_image("/nonexistent/path/xyz", out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }

   printf("all tests passed\n");
   return 0;
}
