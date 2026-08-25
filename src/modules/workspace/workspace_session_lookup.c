/* Session worktree lookup and merged-branch policy helpers. */
#include "aimee.h"
#include "headers/util.h"
#include <aimee/workspace/workspace.h>

/* Check if the current branch has a merged PR. Returns 1 if merged. */
int check_merged_pr_for_branch(const char *git_dir)
{
   int rc;
   char cmd_buf[MAX_PATH_LEN + 128];
   if (git_dir && git_dir[0])
      snprintf(cmd_buf, sizeof(cmd_buf), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null",
               git_dir);
   else
      snprintf(cmd_buf, sizeof(cmd_buf), "git rev-parse --abbrev-ref HEAD 2>/dev/null");
   char *branch = run_cmd(cmd_buf, &rc);
   if (rc != 0 || !branch)
   {
      free(branch);
      return 0;
   }
   char *nl = strchr(branch, '\n');
   if (nl)
      *nl = '\0';

   /* Skip default branches. */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
   {
      free(branch);
      return 0;
   }

   /* Run `gh` inside the target repo. Without `cd`, `gh` inherits aimee-server's
    * cwd and can query the wrong GitHub repository. */
   char cmd[MAX_PATH_LEN + 256];
   if (git_dir && git_dir[0])
      snprintf(cmd, sizeof(cmd),
               "cd '%s' && gh pr list --head '%s' --state merged --json number --limit 1 "
               "2>/dev/null",
               git_dir, branch);
   else
      snprintf(cmd, sizeof(cmd),
               "gh pr list --head '%s' --state merged --json number --limit 1 2>/dev/null", branch);
   free(branch);

   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0;
   }

   int has_merged = strstr(out, "\"number\"") != NULL;
   free(out);
   if (!has_merged)
      return 0;

   /* A merged branch can still be active when HEAD contains newer commits. */
   char ahead_cmd[MAX_PATH_LEN + 256];
   if (git_dir && git_dir[0])
      snprintf(ahead_cmd, sizeof(ahead_cmd),
               "cd '%s' && git rev-list --count origin/main..HEAD 2>/dev/null", git_dir);
   else
      snprintf(ahead_cmd, sizeof(ahead_cmd), "git rev-list --count origin/main..HEAD 2>/dev/null");
   char *ahead = run_cmd(ahead_cmd, &rc);
   if (ahead)
   {
      int n = atoi(ahead);
      free(ahead);
      if (n > 0)
         return 0;
   }
   return 1;
}

/* Return the managed worktree for the most-specific tracked repository. */
const char *worktree_for_cwd(const session_state_t *state, const char *cwd)
{
   if (!state || !cwd || state->worktree_count == 0)
      return NULL;

   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < state->worktree_count; i++)
   {
      size_t root_len = strlen(state->worktrees[i].git_root);
      if (root_len == 0)
         continue;
      if (strncmp(cwd, state->worktrees[i].git_root, root_len) == 0 &&
          (cwd[root_len] == '/' || cwd[root_len] == '\0') && root_len > best_len)
      {
         best = i;
         best_len = root_len;
      }
   }

   if (best < 0)
      return NULL;

   size_t worktree_len = strlen(state->worktrees[best].worktree_path);
   if (strncmp(cwd, state->worktrees[best].worktree_path, worktree_len) == 0 &&
       (cwd[worktree_len] == '/' || cwd[worktree_len] == '\0'))
      return NULL;
   return state->worktrees[best].worktree_path;
}
