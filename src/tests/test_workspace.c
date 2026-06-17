#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "platform_test_util.h"

static void remove_tree(const char *path)
{
   DIR *dir = opendir(path);
   if (!dir)
      return;

   struct dirent *ent;
   while ((ent = readdir(dir)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
         continue;

      char child[1024];
      struct stat st;
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
         remove_tree(child);
      else
         unlink(child);
   }
   closedir(dir);
   rmdir(path);
}

static void create_git_repo(const char *path)
{
   mkdir(path, 0755);
   char git_dir[1024];
   snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
   mkdir(git_dir, 0755);
}

static void init_real_git_repo(const char *path)
{
   char cmd[2048];
   mkdir(path, 0755);
   snprintf(cmd, sizeof(cmd), "git init -q '%s'", path);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.email test@example.invalid", path);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.name 'Aimee Test'", path);
   assert(system(cmd) == 0);
}

static void write_text_file(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(text, f);
   fclose(f);
}

static int branch_exists(const char *repo, const char *name)
{
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' rev-parse --verify --quiet 'refs/heads/%s' >/dev/null 2>&1", repo, name);
   return system(cmd) == 0;
}

static int file_contains(const char *path, const char *needle)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   return strstr(buf, needle) != NULL;
}

int main(void)
{
   printf("workspace: ");

   /* Use isolated temp dir */
   char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-workspace-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* --- discover_projects: empty directory --- */
   {
      char empty[512];
      snprintf(empty, sizeof(empty), "%s/empty", tmpdir);
      mkdir(empty, 0755);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count = workspace_discover_projects(empty, MAX_WORKSPACE_DEPTH, projects,
                                              MAX_DISCOVERED_PROJECTS);
      assert(count == 0);
   }

   /* --- discover_projects: single git repo at root --- */
   {
      char repo[512];
      snprintf(repo, sizeof(repo), "%s/single-repo", tmpdir);
      create_git_repo(repo);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(repo, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
      assert(strstr(projects[0], "single-repo") != NULL);
   }

   /* --- discover_projects: multiple repos in subdirectories --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/multi", tmpdir);
      mkdir(ws, 0755);

      char repo1[512], repo2[512], repo3[512];
      snprintf(repo1, sizeof(repo1), "%s/proj-a", ws);
      snprintf(repo2, sizeof(repo2), "%s/proj-b", ws);
      snprintf(repo3, sizeof(repo3), "%s/proj-c", ws);
      create_git_repo(repo1);
      create_git_repo(repo2);
      create_git_repo(repo3);

      /* Also create a non-git directory that should be skipped */
      char non_git[512];
      snprintf(non_git, sizeof(non_git), "%s/docs", ws);
      mkdir(non_git, 0755);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 3);
   }

   /* --- discover_projects: nested repos (deep directory structure) --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/nested", tmpdir);
      mkdir(ws, 0755);

      char level1[512], level2[512], deep_repo[512];
      snprintf(level1, sizeof(level1), "%s/org", ws);
      mkdir(level1, 0755);
      snprintf(level2, sizeof(level2), "%s/team", level1);
      mkdir(level2, 0755);
      snprintf(deep_repo, sizeof(deep_repo), "%s/deep-proj", level2);
      create_git_repo(deep_repo);

      /* Also a repo at level 1 */
      char shallow_repo[512];
      snprintf(shallow_repo, sizeof(shallow_repo), "%s/shallow-proj", ws);
      create_git_repo(shallow_repo);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 2);

      /* Verify both repos found */
      int found_deep = 0, found_shallow = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(projects[i], "deep-proj"))
            found_deep = 1;
         if (strstr(projects[i], "shallow-proj"))
            found_shallow = 1;
      }
      assert(found_deep);
      assert(found_shallow);
   }

   /* --- discover_projects: a project containing projects --- */
   {
      /* A git repo may itself contain nested git repos. Each nested repo is its
       * own project, so discovery must register the parent AND recurse into it
       * to find the children (a project never absorbs its sub-projects). */
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/project-of-projects", tmpdir);
      mkdir(ws, 0755);

      char repo[512];
      snprintf(repo, sizeof(repo), "%s/parent-repo", ws);
      create_git_repo(repo);

      /* A nested repo directly under the parent, and one a level deeper. */
      char nested[512], deep_parent[512], deep_nested[512];
      snprintf(nested, sizeof(nested), "%s/submodule", repo);
      create_git_repo(nested);
      snprintf(deep_parent, sizeof(deep_parent), "%s/libs", repo);
      mkdir(deep_parent, 0755);
      snprintf(deep_nested, sizeof(deep_nested), "%s/vendored", deep_parent);
      create_git_repo(deep_nested);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 3);

      int found_parent = 0, found_submodule = 0, found_vendored = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(projects[i], "parent-repo/submodule"))
            found_submodule = 1;
         else if (strstr(projects[i], "parent-repo/libs/vendored"))
            found_vendored = 1;
         else if (strstr(projects[i], "parent-repo"))
            found_parent = 1;
      }
      assert(found_parent);
      assert(found_submodule);
      assert(found_vendored);
   }

   /* --- discover_projects: depth limiting --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/depth-test", tmpdir);
      mkdir(ws, 0755);

      /* Create a repo 3 levels deep */
      char l1[512], l2[512], l3[512];
      snprintf(l1, sizeof(l1), "%s/a", ws);
      mkdir(l1, 0755);
      snprintf(l2, sizeof(l2), "%s/b", l1);
      mkdir(l2, 0755);
      snprintf(l3, sizeof(l3), "%s/deep-repo", l2);
      create_git_repo(l3);

      /* With depth 1, should NOT find it */
      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count = workspace_discover_projects(ws, 1, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 0);

      /* With depth 3, SHOULD find it */
      count = workspace_discover_projects(ws, 3, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
   }

   /* --- discover_projects: skips noise directories --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/skip-noise", tmpdir);
      mkdir(ws, 0755);

      /* Create node_modules with a .git inside (should be skipped) */
      char nm[512];
      snprintf(nm, sizeof(nm), "%s/node_modules", ws);
      mkdir(nm, 0755);
      char nm_repo[512];
      snprintf(nm_repo, sizeof(nm_repo), "%s/some-pkg", nm);
      create_git_repo(nm_repo);

      /* Create a real project */
      char real[512];
      snprintf(real, sizeof(real), "%s/real-proj", ws);
      create_git_repo(real);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
      assert(strstr(projects[0], "real-proj") != NULL);
   }

   /* --- discovery: skips linked git worktrees, keeps the real checkout --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/wt-ws", tmpdir);
      mkdir(ws, 0755);

      /* Real checkout: .git is a directory. */
      char realrepo[512];
      snprintf(realrepo, sizeof(realrepo), "%s/main", ws);
      create_git_repo(realrepo);

      /* Linked worktree sibling: .git is a regular file ("gitdir: <path>"),
       * a duplicate working copy of the same repo — must NOT be discovered. */
      char wt[512], wtgit[600];
      snprintf(wt, sizeof(wt), "%s/wt-copy", ws);
      mkdir(wt, 0755);
      snprintf(wtgit, sizeof(wtgit), "%s/.git", wt);
      write_text_file(wtgit, "gitdir: /home/x/main/.git/worktrees/wt-copy\n");

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1); /* only the real checkout, not the worktree */
      char real_abs[MAX_PATH_LEN];
      assert(realpath(realrepo, real_abs) != NULL);
      assert(strcmp(projects[0], real_abs) == 0);

      /* A worktree added explicitly (as the scan root) is still honored. */
      int c2 =
          workspace_discover_projects(wt, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(c2 == 1);
      char wt_abs[MAX_PATH_LEN];
      assert(realpath(wt, wt_abs) != NULL);
      assert(strcmp(projects[0], wt_abs) == 0);
   }

   /* --- discovery: a self-referential dir symlink does not cause re-discovery --- */
   {
      char ws[512], repo[512], link[600];
      snprintf(ws, sizeof(ws), "%s/sym-ws", tmpdir);
      mkdir(ws, 0755);
      snprintf(repo, sizeof(repo), "%s/router", ws);
      create_git_repo(repo);
      /* "src -> ." inside the repo: following it would loop and re-discover the
       * repo once per depth level (the smoothrouter bug). */
      snprintf(link, sizeof(link), "%s/src", repo);
      assert(symlink(".", link) == 0);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1); /* discovered exactly once, not N times */
      assert(strstr(projects[0], "router") != NULL);
   }

   /* --- style_read: missing file returns NULL --- */
   {
      char *style = style_read("nonexistent-project-xyz");
      assert(style == NULL);
   }

   /* --- active_root: prefers matching configured workspace over index 0 --- */
   {
      char ws1[512], ws2[512], project[512], nested[512], resolved[MAX_PATH_LEN];
      snprintf(ws1, sizeof(ws1), "%s/ws-one", tmpdir);
      snprintf(ws2, sizeof(ws2), "%s/ws-two", tmpdir);
      snprintf(project, sizeof(project), "%s/project", ws2);
      snprintf(nested, sizeof(nested), "%s/project/subdir", ws2);
      assert(mkdir(ws1, 0755) == 0);
      assert(mkdir(ws2, 0755) == 0);
      assert(mkdir(project, 0755) == 0);
      assert(mkdir(nested, 0755) == 0);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.workspaces[0], sizeof(cfg.workspaces[0]), "%s", ws1);
      snprintf(cfg.workspaces[1], sizeof(cfg.workspaces[1]), "%s", ws2);
      cfg.workspace_count = 2;

      assert(workspace_active_root(&cfg, nested, resolved, sizeof(resolved)) == 0);
      assert(strcmp(resolved, ws2) == 0);
   }

   /* --- active_root: falls back to cwd when it is outside configured workspaces --- */
   {
      char outside[512], resolved[MAX_PATH_LEN], expected[MAX_PATH_LEN];
      snprintf(outside, sizeof(outside), "%s/outside", tmpdir);
      assert(mkdir(outside, 0755) == 0);
      assert(realpath(outside, expected) != NULL);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(workspace_active_root(&cfg, outside, resolved, sizeof(resolved)) == 0);
      assert(strcmp(resolved, expected) == 0);
   }

   /* --- active_root: uses git top-level for nested repo directories --- */
   {
      char repo[512], srcdir[512], subdir[512], resolved[MAX_PATH_LEN];
      snprintf(repo, sizeof(repo), "%s/real-git", tmpdir);
      init_real_git_repo(repo);
      snprintf(srcdir, sizeof(srcdir), "%s/src", repo);
      snprintf(subdir, sizeof(subdir), "%s/src/nested", repo);
      assert(mkdir(srcdir, 0755) == 0);
      assert(mkdir(subdir, 0755) == 0);

      assert(workspace_active_root(NULL, subdir, resolved, sizeof(resolved)) == 0);
      assert(strcmp(resolved, repo) == 0);
   }

   /* --- count_active_worktrees_for_root: counts repo-local .aimee/worktrees entries --- */
   {
      char parent[512];
      snprintf(parent, sizeof(parent), "%s/wt-count-parent", tmpdir);
      mkdir(parent, 0755);

      char repo[512];
      snprintf(repo, sizeof(repo), "%s/myrepo", parent);
      create_git_repo(repo);

      assert(count_active_worktrees_for_root(repo) == 0);

      char aimee_dir[512], managed[512], sid1[512], sid2[512], wt1[512], wt2[512];
      snprintf(aimee_dir, sizeof(aimee_dir), "%s/.aimee", repo);
      snprintf(managed, sizeof(managed), "%s/worktrees", aimee_dir);
      snprintf(sid1, sizeof(sid1), "%s/aabbccdd", managed);
      snprintf(sid2, sizeof(sid2), "%s/11223344", managed);
      snprintf(wt1, sizeof(wt1), "%s/main", sid1);
      snprintf(wt2, sizeof(wt2), "%s/task01", sid2);
      mkdir(aimee_dir, 0755);
      mkdir(managed, 0755);
      mkdir(sid1, 0755);
      mkdir(sid2, 0755);
      mkdir(wt1, 0755);
      mkdir(wt2, 0755);

      assert(count_active_worktrees_for_root(repo) == 2);

      rmdir(wt1);
      assert(count_active_worktrees_for_root(repo) == 1);

      /* Legacy sibling paths are still counted during migration. */
      char legacy[512];
      snprintf(legacy, sizeof(legacy), "%s/.aimee-myrepo-aabbccdd", parent);
      mkdir(legacy, 0755);
      assert(count_active_worktrees_for_root(repo) == 2);
   }

   /* --- worktree_managed_git_root: maps managed worktrees back to the owner repo --- */
   {
      char repo[512], managed_path[512], resolved[MAX_PATH_LEN];
      snprintf(repo, sizeof(repo), "%s/managed-root-repo", tmpdir);
      snprintf(managed_path, sizeof(managed_path), "%s/.aimee/worktrees/abcdef12/main/src", repo);
      assert(worktree_managed_git_root(managed_path, resolved, sizeof(resolved)) == 0);
      assert(strcmp(resolved, repo) == 0);
      assert(worktree_managed_git_root(repo, resolved, sizeof(resolved)) != 0);
   }

   /* --- delegate apply: copies changes back and refuses parent conflicts --- */
   {
      char repo[512], delegate_wt[512], cmd[2048], tracked[512], remove_me[512], added[512];
      char err[512];
      snprintf(repo, sizeof(repo), "%s/delegate-apply-parent", tmpdir);
      snprintf(delegate_wt, sizeof(delegate_wt), "%s/delegate-apply-child", tmpdir);
      init_real_git_repo(repo);
      snprintf(tracked, sizeof(tracked), "%s/tracked.txt", repo);
      snprintf(remove_me, sizeof(remove_me), "%s/remove.txt", repo);
      write_text_file(tracked, "base\n");
      write_text_file(remove_me, "remove\n");
      snprintf(cmd, sizeof(cmd),
               "git -C '%s' add tracked.txt remove.txt && git -C '%s' commit -q -m base", repo,
               repo);
      assert(system(cmd) == 0);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q '%s' HEAD", repo, delegate_wt);
      assert(system(cmd) == 0);

      snprintf(tracked, sizeof(tracked), "%s/tracked.txt", delegate_wt);
      snprintf(remove_me, sizeof(remove_me), "%s/remove.txt", delegate_wt);
      snprintf(added, sizeof(added), "%s/added.txt", delegate_wt);
      write_text_file(tracked, "delegate edit\n");
      unlink(remove_me);
      write_text_file(added, "new file\n");

      assert(worktree_apply_delegate_changes_to_parent(delegate_wt, repo, err, sizeof(err)) == 3);
      snprintf(tracked, sizeof(tracked), "%s/tracked.txt", repo);
      snprintf(remove_me, sizeof(remove_me), "%s/remove.txt", repo);
      snprintf(added, sizeof(added), "%s/added.txt", repo);
      assert(file_contains(tracked, "delegate edit"));
      assert(file_contains(added, "new file"));
      assert(access(remove_me, F_OK) != 0);

      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove -f '%s'", repo, delegate_wt);
      assert(system(cmd) == 0);
      snprintf(cmd, sizeof(cmd), "git -C '%s' reset -q --hard HEAD && git -C '%s' clean -q -fd",
               repo, repo);
      assert(system(cmd) == 0);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q '%s' HEAD", repo, delegate_wt);
      assert(system(cmd) == 0);
      snprintf(tracked, sizeof(tracked), "%s/tracked.txt", repo);
      write_text_file(tracked, "parent edit\n");
      snprintf(tracked, sizeof(tracked), "%s/tracked.txt", delegate_wt);
      write_text_file(tracked, "delegate edit\n");
      assert(worktree_apply_delegate_changes_to_parent(delegate_wt, repo, err, sizeof(err)) < 0);
      assert(strstr(err, "existing parent change") != NULL);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove -f '%s'", repo, delegate_wt);
      assert(system(cmd) == 0);
   }

   /* --- worktree_apply_anchor_wip: a delegate worktree carries the parent's
    *     uncommitted working-tree state, not merely its last commit --- */
   {
      char anchor[512], wt[512], cmd[2048], path[640];
      snprintf(anchor, sizeof(anchor), "%s/anchor-wip", tmpdir);
      init_real_git_repo(anchor);
      /* Initial commit: a tracked file plus one we'll later delete. */
      snprintf(path, sizeof(path), "%s/tracked.txt", anchor);
      write_text_file(path, "committed\n");
      snprintf(path, sizeof(path), "%s/gone.txt", anchor);
      write_text_file(path, "delete me\n");
      snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -q -m init", anchor,
               anchor);
      assert(system(cmd) == 0);

      /* A second worktree, clean at the same HEAD — stands in for the freshly
       * created delegate worktree (based on the anchor's HEAD). */
      snprintf(wt, sizeof(wt), "%s/anchor-wip-child", tmpdir);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q '%s' HEAD", anchor, wt);
      assert(system(cmd) == 0);

      /* Parent makes UNCOMMITTED changes: edit a tracked file, delete a tracked
       * file, and add an untracked (non-ignored) file in a subdirectory. */
      snprintf(path, sizeof(path), "%s/tracked.txt", anchor);
      write_text_file(path, "WIP edit\n");
      snprintf(path, sizeof(path), "%s/gone.txt", anchor);
      assert(unlink(path) == 0);
      snprintf(path, sizeof(path), "%s/sub", anchor);
      assert(mkdir(path, 0755) == 0);
      snprintf(path, sizeof(path), "%s/sub/new.txt", anchor);
      write_text_file(path, "brand new\n");

      /* Replay the parent's WIP into the child worktree. */
      worktree_apply_anchor_wip(anchor, wt);

      /* The child now reflects the parent's working tree, not its last commit. */
      snprintf(path, sizeof(path), "%s/tracked.txt", wt);
      assert(file_contains(path, "WIP edit"));
      assert(!file_contains(path, "committed"));
      snprintf(path, sizeof(path), "%s/sub/new.txt", wt);
      assert(file_contains(path, "brand new")); /* untracked file carried */
      snprintf(path, sizeof(path), "%s/gone.txt", wt);
      assert(access(path, F_OK) != 0); /* tracked deletion carried */

      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove -f '%s'", anchor, wt);
      assert(system(cmd) == 0);
   }

   /* --- apply-back tolerates carried WIP: parent has an uncommitted file the
    *     delegate didn't touch (carried in by worktree_apply_anchor_wip); the
    *     delegate edits a *different* file. Apply-back must succeed, not refuse. --- */
   {
      char repo[512], dwt[512], cmd[2048], err[256];
      snprintf(repo, sizeof(repo), "%s/carry-applyback-parent", tmpdir);
      snprintf(dwt, sizeof(dwt), "%s/carry-applyback-child", tmpdir);
      init_real_git_repo(repo);
      char a[600], b[600];
      snprintf(a, sizeof(a), "%s/a.txt", repo);
      snprintf(b, sizeof(b), "%s/b.txt", repo);
      write_text_file(a, "a-committed\n");
      write_text_file(b, "b-committed\n");
      snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -q -m base", repo, repo);
      assert(system(cmd) == 0);

      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q '%s' HEAD", repo, dwt);
      assert(system(cmd) == 0);

      /* Parent makes an uncommitted WIP edit to a.txt, carried into the delegate. */
      write_text_file(a, "a-parent-wip\n");
      worktree_apply_anchor_wip(repo, dwt);
      /* Delegate independently edits b.txt. */
      char dwt_b[600];
      snprintf(dwt_b, sizeof(dwt_b), "%s/b.txt", dwt);
      write_text_file(dwt_b, "b-delegate-edit\n");

      err[0] = '\0';
      int rc = worktree_apply_delegate_changes_to_parent(dwt, repo, err, sizeof(err));
      assert(rc >= 0); /* must NOT refuse on the carried a.txt */
      assert(err[0] == '\0');
      assert(file_contains(b, "b-delegate-edit")); /* delegate's real work landed */
      assert(file_contains(a, "a-parent-wip"));    /* parent WIP preserved */

      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove -f '%s'", repo, dwt);
      assert(system(cmd) == 0);
   }

   /* --- worktree_delegate_work_name: deterministic + agreement (the fix for the
    *     two-worktrees-per-delegation bug). The dispatch path and the server
    *     compute path must derive the SAME work-name so they resolve to one
    *     shared sibling worktree instead of two random divergent ones. --- */
   {
      char a[32] = "", b[32] = "", c[32] = "";
      assert(worktree_delegate_work_name("deleg-14", a, sizeof(a)) == 0);
      assert(worktree_delegate_work_name("deleg-14", b, sizeof(b)) == 0);
      assert(strcmp(a, b) == 0);  /* deterministic: same sid -> same name */
      assert(strlen(a) == 8);     /* 8 hex chars */
      for (int i = 0; i < 8; i++) /* lowercase hex */
         assert((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));

      /* Agreement: two sids that share the first 8 chars (the worktree-dir
       * component both paths use) yield the SAME name — so a dispatch sid of
       * "deleg-14" and a compute sid of "deleg-14-suffix" still collapse to one
       * worktree. */
      assert(worktree_delegate_work_name("deleg-14-suffix", c, sizeof(c)) == 0);
      assert(strcmp(a, c) == 0);

      /* Distinct delegate ids generally differ (no accidental collapse). */
      char d[32] = "";
      assert(worktree_delegate_work_name("deleg-15", d, sizeof(d)) == 0);
      assert(strcmp(a, d) != 0);

      /* arg guards */
      assert(worktree_delegate_work_name(NULL, a, sizeof(a)) == -1);
      assert(worktree_delegate_work_name("x", NULL, 32) == -1);
      char tiny[4];
      assert(worktree_delegate_work_name("x", tiny, sizeof(tiny)) == -1);
   }

   /* --- worktree GC deletes the branch of a merged worktree it removes, but
    *     preserves the branch when a worktree is removed only under --force
    *     (commits ahead of base). This is what stops merged branches from
    *     piling up after their worktrees are GC'd. --- */
   {
      char repo[512], cmd[2048], path[640], wt[640];
      snprintf(repo, sizeof(repo), "%s/gc-repo", tmpdir);
      init_real_git_repo(repo);
      snprintf(path, sizeof(path), "%s/base.txt", repo);
      write_text_file(path, "base\n");
      snprintf(cmd, sizeof(cmd),
               "git -C '%s' add -A && git -C '%s' commit -q -m base && git -C '%s' branch -M main",
               repo, repo, repo);
      assert(system(cmd) == 0);

      /* Merged session worktree: a branch sitting exactly at main (0 ahead). */
      snprintf(cmd, sizeof(cmd), "mkdir -p '%s/.aimee/worktrees/sessA'", repo);
      assert(system(cmd) == 0);
      snprintf(wt, sizeof(wt), "%s/.aimee/worktrees/sessA/main", repo);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q -b feat/merged '%s' main", repo, wt);
      assert(system(cmd) == 0);
      assert(branch_exists(repo, "feat/merged"));

      worktree_gc_options_t opts;
      worktree_gc_options_init(&opts);
      opts.max_age_days = 0; /* don't gate on idle time in the test */
      worktree_gc_candidate_t cands[WORKTREE_GC_MAX_CANDIDATES];
      int n = worktree_gc_scan(repo, &opts, cands, WORKTREE_GC_MAX_CANDIDATES);
      assert(n == 1);
      assert(cands[0].eligible == 1);
      assert(cands[0].commits_ahead == 0);
      assert(worktree_gc_apply(repo, cands, n, &opts) == 1);
      assert(access(wt, F_OK) != 0);               /* worktree removed */
      assert(!branch_exists(repo, "feat/merged")); /* merged branch deleted too */
      assert(branch_exists(repo, "main"));         /* base branch untouched */

      /* Ahead session worktree: removed only because of --force; its branch
       * carries unmerged commits and MUST survive. */
      snprintf(cmd, sizeof(cmd), "mkdir -p '%s/.aimee/worktrees/sessB'", repo);
      assert(system(cmd) == 0);
      snprintf(wt, sizeof(wt), "%s/.aimee/worktrees/sessB/main", repo);
      snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add -q -b feat/ahead '%s' main", repo, wt);
      assert(system(cmd) == 0);
      snprintf(path, sizeof(path), "%s/extra.txt", wt);
      write_text_file(path, "ahead\n");
      snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -q -m ahead", wt, wt);
      assert(system(cmd) == 0);

      worktree_gc_options_init(&opts);
      opts.max_age_days = 0;
      opts.force = 1; /* remove despite commits ahead */
      n = worktree_gc_scan(repo, &opts, cands, WORKTREE_GC_MAX_CANDIDATES);
      assert(n == 1);
      assert(cands[0].eligible == 1);
      assert(cands[0].commits_ahead == 1);
      assert(worktree_gc_apply(repo, cands, n, &opts) == 1);
      assert(access(wt, F_OK) != 0);             /* worktree removed under force */
      assert(branch_exists(repo, "feat/ahead")); /* unmerged branch preserved */
   }

   /* Cleanup */
   remove_tree(tmpdir);

   printf("all tests passed\n");
   return 0;
}
