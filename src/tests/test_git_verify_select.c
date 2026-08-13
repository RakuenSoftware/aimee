#include "modules/git/git_verify_select.h"

#include "modules/git/git_verify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void run(const char *cmd)
{
   int rc = system(cmd);
   assert(rc == 0);
}

static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(text, f);
   fclose(f);
}

static void read_cmd(char *out, size_t out_len, const char *cmd)
{
   FILE *p = popen(cmd, "r");
   assert(p != NULL);
   assert(fgets(out, (int)out_len, p) != NULL);
   assert(pclose(p) == 0);
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
}

static void test_path_match(void)
{
   assert(verify_path_match("src/**", "src/git_verify.c"));
   assert(verify_path_match("**/*.c", "src/git_verify.c"));
   assert(verify_path_match("*.md", "docs/proposals/x.md"));
   assert(!verify_path_match("src/**", "docs/proposals/x.md"));
   assert(verify_path_list_matches("docs/**, src/**", "src/a.c"));
   printf("  path_match: ok\n");
}

static void test_select_skips_unmatched_change(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-verify-select-XXXXXX", platform_tmpdir());
   char *root = mkdtemp(tmpl);
   assert(root != NULL);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email a@b && git config user.name a && "
            "mkdir -p src .aimee && echo 'int main(void){return 0;}' > src/a.c && "
            "echo ok > README.md && git add . && git commit -qm init",
            root);
   run(cmd);

   char tree[128], state_path[1024];
   snprintf(cmd, sizeof(cmd), "cd '%s' && git rev-parse HEAD^{tree}", root);
   read_cmd(tree, sizeof(tree), cmd);
   snprintf(state_path, sizeof(state_path), "%s/.aimee/.last-verify", root);
   char line[512];
   snprintf(line, sizeof(line), "1 %s failed=0/total=2 steps=lint:0,docs:0\n", tree);
   write_text(state_path, line);
   snprintf(cmd, sizeof(cmd), "%s/README.md", root);
   write_text(cmd, "changed\n");

   verify_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.incremental = 1;
   cfg.count = 2;
   snprintf(cfg.steps[0].name, sizeof(cfg.steps[0].name), "lint");
   snprintf(cfg.steps[0].paths, sizeof(cfg.steps[0].paths), "src/**");
   snprintf(cfg.steps[1].name, sizeof(cfg.steps[1].name), "docs");
   snprintf(cfg.steps[1].paths, sizeof(cfg.steps[1].paths), "*.md");
   verify_thread_ctx_t ctx[2];
   memset(ctx, 0, sizeof(ctx));
   int state[2] = {0, 0};
   int remaining = 2;
   verify_incremental_apply(root, &cfg, ctx, state, &remaining);
   assert(ctx[0].skipped == 1);
   assert(state[0] == 2);
   assert(ctx[1].skipped == 0);
   assert(state[1] == 0);
   assert(remaining == 1);
   printf("  select_skips_unmatched_change: ok\n");
}

/* Regression: a session worktree lives at <repo>/.aimee/worktrees/<hash>/main,
 * so its toplevel basename is the literal "main". The verify config must still
 * key on the canonical repo name (here "myrepo"), not collide every repo's
 * worktree onto a shared projects/main config. */
static void test_worktree_config_keys_on_repo_name(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-verify-projname-XXXXXX", platform_tmpdir());
   char *tmp = mkdtemp(tmpl);
   assert(tmp != NULL);

   char repo[1024];
   snprintf(repo, sizeof(repo), "%s/myrepo", tmp);
   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "mkdir -p '%s' && cd '%s' && git init -q && git config user.email a@b && "
            "git config user.name a && echo ok > README.md && git add . && "
            "git commit -qm init && "
            "git worktree add -q '%s/.aimee/worktrees/abc123/main' -b wt >/dev/null 2>&1",
            repo, repo, repo);
   run(cmd);

   char worktree[1280];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/abc123/main", repo);

   char yaml[2048];
   assert(project_yaml_path(worktree, yaml, sizeof(yaml)) == 0);
   /* Must resolve to the real repo name, never the worktree's "main" basename. */
   assert(strstr(yaml, "/projects/myrepo/project.yaml") != NULL);
   assert(strstr(yaml, "/projects/main/") == NULL);

   /* The main checkout resolves to the same name, so worktrees share its config. */
   char main_yaml[2048];
   assert(project_yaml_path(repo, main_yaml, sizeof(main_yaml)) == 0);
   assert(strcmp(yaml, main_yaml) == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
   run(cmd);
   printf("  worktree_config_keys_on_repo_name: ok\n");
}

int main(void)
{
   printf("test_git_verify_select:\n");
   test_path_match();
   test_select_skips_unmatched_change();
   test_worktree_config_keys_on_repo_name();
   printf("All git_verify_select tests passed.\n");
   return 0;
}
