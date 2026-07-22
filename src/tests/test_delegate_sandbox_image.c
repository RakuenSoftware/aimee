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

   /* --- pure: CreatedAt -> UTC epoch (the gc age signal) --- */
   {
      long long e = 0;
      /* 1970-01-01 00:00:00 UTC is epoch 0. */
      assert(delegate_sandbox_parse_created_epoch("1970-01-01 00:00:00 +0000 UTC", &e) == 0);
      assert(e == 0);
      /* 2000-01-01 00:00:00 UTC = 946684800. */
      assert(delegate_sandbox_parse_created_epoch("2000-01-01 00:00:00 +0000 UTC", &e) == 0);
      assert(e == 946684800LL);
      /* 2026-07-15 12:34:56 UTC = 1784118896. */
      assert(delegate_sandbox_parse_created_epoch("2026-07-15 12:34:56 +0000 UTC", &e) == 0);
      assert(e == 1784118896LL);
      /* A +02:00 wall-clock is 2h EARLIER in UTC than the same digits at +0000. */
      long long z = 0, plus = 0;
      assert(delegate_sandbox_parse_created_epoch("2026-07-15 12:34:56 +0000 UTC", &z) == 0);
      assert(delegate_sandbox_parse_created_epoch("2026-07-15 12:34:56 +0200 CEST", &plus) == 0);
      assert(plus == z - 2 * 3600);
      /* A -05:00 wall-clock is 5h LATER in UTC. */
      long long minus = 0;
      assert(delegate_sandbox_parse_created_epoch("2026-07-15 12:34:56 -0500 EST", &minus) == 0);
      assert(minus == z + 5 * 3600);
      /* Missing offset still parses (treated as UTC). */
      assert(delegate_sandbox_parse_created_epoch("2026-07-15 12:34:56", &e) == 0);
      assert(e == 1784118896LL);
      /* Garbage / NULL rejected. */
      assert(delegate_sandbox_parse_created_epoch("not-a-date", &e) == -1);
      assert(delegate_sandbox_parse_created_epoch(NULL, &e) == -1);
      assert(delegate_sandbox_parse_created_epoch("2026-13-01 00:00:00 +0000 UTC", &e) == -1);
   }

   /* --- pure: gc keep/remove decision --- */
   {
      const long long now = 1784118896LL; /* 2026-07-15 12:34:56 UTC */
      const long week = 7 * 24 * 3600;    /* max_age_secs */
      const char *r = NULL;

      /* in-use is never removed, even when old and unprotected. */
      assert(delegate_sandbox_gc_should_remove(1, 99, 0, now - 30 * week, now, week, &r) == 0);
      assert(strcmp(r, "in-use") == 0);

      /* the keep_min most-recent (index < keep_min) are protected regardless of age. */
      assert(delegate_sandbox_gc_should_remove(0, 0, 3, now - 30 * week, now, week, &r) == 0);
      assert(strcmp(r, "kept-recent") == 0);
      assert(delegate_sandbox_gc_should_remove(0, 2, 3, now - 30 * week, now, week, &r) == 0);
      assert(strcmp(r, "kept-recent") == 0);

      /* beyond keep_min but younger than max_age -> kept. */
      assert(delegate_sandbox_gc_should_remove(0, 5, 3, now - 3600, now, week, &r) == 0);
      assert(strcmp(r, "within-max-age") == 0);

      /* beyond keep_min and older than max_age -> removed. */
      assert(delegate_sandbox_gc_should_remove(0, 5, 3, now - 30 * week, now, week, &r) == 1);
      assert(strcmp(r, "aged-out") == 0);

      /* exactly at the boundary (age == max_age) is removed (< is the keep test). */
      assert(delegate_sandbox_gc_should_remove(0, 5, 3, now - week, now, week, &r) == 1);
      assert(strcmp(r, "aged-out") == 0);

      /* unknown created time (0) is treated as old enough to remove. */
      assert(delegate_sandbox_gc_should_remove(0, 5, 3, 0, now, week, &r) == 1);
      assert(strcmp(r, "aged-out") == 0);

      /* max_age 0 ("any age") removes everything not in use / not kept-recent. */
      assert(delegate_sandbox_gc_should_remove(0, 5, 3, now - 1, now, 0, &r) == 1);
      assert(strcmp(r, "aged-out") == 0);

      /* keep_min 0 protects nothing by recency. */
      assert(delegate_sandbox_gc_should_remove(0, 0, 0, now - 30 * week, now, week, &r) == 1);
      assert(strcmp(r, "aged-out") == 0);
   }

   printf("all tests passed\n");
   return 0;
}
