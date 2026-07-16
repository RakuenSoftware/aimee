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

   /* --- pure: Dockerfile generation from from+packages --- */
   {
      char df[4096];
      const char *pkgs[] = {"gcc", "make", "libssl-dev"};
      assert(delegate_sandbox_dockerfile_from_packages("ubuntu:22.04", pkgs, 3, df, sizeof(df)) ==
             0);
      assert(strstr(df, "FROM ubuntu:22.04\n") == df);
      assert(strstr(df, "apt-get install -y --no-install-recommends gcc make libssl-dev") != NULL);

      /* zero packages -> just FROM */
      assert(delegate_sandbox_dockerfile_from_packages("alpine:3", NULL, 0, df, sizeof(df)) == 0);
      assert(strcmp(df, "FROM alpine:3\n") == 0);

      /* injection-y package name is rejected (no shell metacharacters reach RUN) */
      const char *bad[] = {"gcc; rm -rf /"};
      assert(delegate_sandbox_dockerfile_from_packages("ubuntu:22.04", bad, 1, df, sizeof(df)) ==
             -1);
      const char *bad2[] = {"$(whoami)"};
      assert(delegate_sandbox_dockerfile_from_packages("ubuntu:22.04", bad2, 1, df, sizeof(df)) ==
             -1);
      /* empty base rejected */
      assert(delegate_sandbox_dockerfile_from_packages("", pkgs, 1, df, sizeof(df)) == -1);
   }

   /* --- pure: content tag is deterministic + well-formed --- */
   {
      char t1[64], t2[64], t3[64];
      delegate_sandbox_content_tag("FROM ubuntu:22.04\n", t1, sizeof(t1));
      delegate_sandbox_content_tag("FROM ubuntu:22.04\n", t2, sizeof(t2));
      delegate_sandbox_content_tag("FROM alpine:3\n", t3, sizeof(t3));
      assert(strcmp(t1, t2) == 0); /* same content -> same tag (reuse) */
      assert(strcmp(t1, t3) != 0); /* different content -> different tag */
      assert(strncmp(t1, "aimee-sbx:", 10) == 0);
      assert(strlen(t1) == 10 + 12); /* prefix + 12 hex */
   }

   printf("all tests passed\n");
   return 0;
}
