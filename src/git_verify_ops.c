#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "headers/git_verify_internal.h"
#include "headers/config.h"
#include "headers/guardrails.h"
#include "headers/dstr.h"
#include "headers/util.h"
#include "headers/mcp_git.h"

/* --- Verify scope + master-switch gate --- */

int verify_enabled_global(void)
{
   config_t cfg;
   return (config_load(&cfg) == 0) ? cfg.verify_enabled : 0;
}

/* See git_verify.h for the contract. Compares the canonical main-repo root of
 * the target against the session's registered worktree git_roots; cross-project
 * repos are out of scope unless config opts in. config_load /
 * session_state_load are both cheap relative to a push/PR. */
int verify_project_in_scope(const char *target_repo_root)
{
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.verify_cross_project)
      return 1;

   session_state_t state;
   session_state_load(&state, session_id());
   if (state.worktree_count == 0)
      return 1; /* no session home anchor — preserve single-repo behavior */

   char target_main[MAX_PATH_LEN];
   if (resolve_main_repo_root(target_repo_root, target_main, sizeof(target_main)) != 0)
      return 1; /* cannot resolve target — fail open rather than wrongly skip */

   for (int i = 0; i < state.worktree_count; i++)
   {
      char home_main[MAX_PATH_LEN];
      if (resolve_main_repo_root(state.worktrees[i].git_root, home_main, sizeof(home_main)) == 0 &&
          strcmp(home_main, target_main) == 0)
         return 1;
   }
   return 0;
}

int verify_gate_blocks(const char *target_root, const char *expected_commit, char *msg,
                       size_t msg_len)
{
   /* Out-of-scope (cross-project, default) repos are never gated. */
   if (!verify_project_in_scope(target_root))
      return 0;

   /* Auto-generate-and-gate an unconfigured repo only when the global verify
    * master switch is on. With verify disabled (default), an existing explicit
    * project.yaml with enforce:true still re-enables the gate, but a repo with
    * no config is neither created nor gated. */
   if (!verify_enabled_global())
   {
      char ypath[MAX_PATH_LEN];
      if (project_yaml_path(target_root, ypath, sizeof(ypath)) != 0 || access(ypath, F_OK) != 0)
         return 0;
   }

   verify_config_t vcfg;
   if (verify_load_config(target_root, &vcfg) != 0 || !vcfg.enforce)
      return 0;

   return !verify_check(target_root, expected_commit, msg, msg_len);
}

/* --- Semantic Conflict Resolver --- */

char *verify_resolve_conflicts(const char *project_root)
{
   dstr_t res;
   dstr_init(&res);

   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain=v2 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git status --porcelain=v2 2>/dev/null");

   int rc;
   char *status = run_cmd(cmd, &rc);
   if (rc != 0 || !status || !status[0])
   {
      free(status);
      return strdup("no conflicts detected");
   }

   int conflicted_count = 0;
   char *line = status;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      /* In porcelain v2, conflicted files start with 'u' */
      if (line[0] == 'u')
      {
         const char *fname = line;
         int spaces = 0;
         for (const char *p = line; *p && spaces < 10; p++)
         {
            if (*p == ' ')
               spaces++;
            if (spaces == 10)
            {
               fname = p + 1;
               break;
            }
         }

         dstr_appendf(&res, "file: %s\n", fname);
         conflicted_count++;

         /* Read file and find hunks */
         char full_path[MAX_PATH_LEN];
         if (project_root && project_root[0])
            snprintf(full_path, sizeof(full_path), "%s/%s", project_root, fname);
         else
            snprintf(full_path, sizeof(full_path), "%s", fname);

         FILE *f = fopen(full_path, "r");
         if (f)
         {
            char fline[1024];
            int in_hunk = 0;
            char head_ref[64] = "", base_ref[64] = "";
            while (fgets(fline, sizeof(fline), f))
            {
               if (strncmp(fline, "<<<<<<<", 7) == 0)
               {
                  in_hunk = 1;
                  dstr_append_str(&res, "  hunk:\n    ");
                  dstr_append_str(&res, fline + 2); /* indent */
                  char *nl2 = strchr(fline, '\n');
                  if (nl2)
                     *nl2 = '\0';
                  snprintf(head_ref, sizeof(head_ref), "%s", fline + 8);
                  while (head_ref[0] && isspace((unsigned char)head_ref[strlen(head_ref) - 1]))
                     head_ref[strlen(head_ref) - 1] = '\0';
               }
               else if (strncmp(fline, "=======", 7) == 0)
               {
                  dstr_append_str(&res, "    =======\n");
               }
               else if (strncmp(fline, ">>>>>>>", 7) == 0)
               {
                  dstr_append_str(&res, "    ");
                  dstr_append_str(&res, fline + 2);
                  char *nl2 = strchr(fline, '\n');
                  if (nl2)
                     *nl2 = '\0';
                  snprintf(base_ref, sizeof(base_ref), "%s", fline + 8);
                  while (base_ref[0] && isspace((unsigned char)base_ref[strlen(base_ref) - 1]))
                     base_ref[strlen(base_ref) - 1] = '\0';

                  /* Context logs */
                  if (head_ref[0])
                  {
                     char lcmd[256];
                     snprintf(lcmd, sizeof(lcmd),
                              "git log -1 --format=\"%%h %%s (%%an, %%ar)\" %s 2>/dev/null",
                              head_ref);
                     int lrc;
                     char *lout = run_cmd(lcmd, &lrc);
                     if (lrc == 0 && lout)
                        dstr_appendf(&res, "    context %s: %s", head_ref, lout);
                     free(lout);
                  }
                  if (base_ref[0])
                  {
                     char lcmd[256];
                     snprintf(lcmd, sizeof(lcmd),
                              "git log -1 --format=\"%%h %%s (%%an, %%ar)\" %s 2>/dev/null",
                              base_ref);
                     int lrc;
                     char *lout = run_cmd(lcmd, &lrc);
                     if (lrc == 0 && lout)
                        dstr_appendf(&res, "    context %s: %s", base_ref, lout);
                     free(lout);
                  }
                  in_hunk = 0;
                  dstr_append_char(&res, '\n');
               }
               else if (in_hunk)
               {
                  dstr_append_str(&res, "    ");
                  dstr_append_str(&res, fline);
               }
            }
            fclose(f);
         }
      }
      line = nl ? nl + 1 : NULL;
   }
   free(status);

   if (conflicted_count == 0)
   {
      dstr_free(&res);
      return strdup("no conflicts detected");
   }

   return dstr_steal(&res);
}

/* --- Environment Parity Check --- */

char *verify_check_env(verify_config_t *cfg)
{
   if (!cfg || cfg->env_count == 0)
      return strdup("no environment checks configured");

   dstr_t res;
   dstr_init(&res);
   int all_pass = 1;

   for (int i = 0; i < cfg->env_count; i++)
   {
      char cmd[128];
      snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", cfg->env_checks[i]);
      int rc = system(cmd);
      if (rc == 0)
         dstr_appendf(&res, "[PASS] %s: found in PATH\n", cfg->env_checks[i]);
      else
      {
         dstr_appendf(&res, "[FAIL] %s: NOT found in PATH\n", cfg->env_checks[i]);
         all_pass = 0;
      }
   }

   if (all_pass)
      dstr_append_str(&res, "\nenvironment parity: OK\n");
   else
      dstr_append_str(&res, "\nenvironment parity: FAILED (missing required tools)\n");

   return dstr_steal(&res);
}

/* --- Macro Git Operations --- */

char *verify_prepare_pr(const char *project_root, const char *base_branch)
{
   if (!base_branch || !base_branch[0])
      base_branch = "main";

   dstr_t res;
   dstr_init(&res);

   dstr_appendf(&res, "PR Readiness Report (base: %s)\n", base_branch);
   dstr_append_str(&res, "========================================\n\n");

   {
      char branch[256] = "";
      if (get_current_branch(branch, sizeof(branch)) == 0 && branch[0])
      {
         if (check_branch_has_merged_pr_for(branch))
         {
            dstr_appendf(&res,
                         "0. Branch Reuse: BLOCKED - branch '%s' already has a merged PR. "
                         "Create a new branch before opening another PR.\n\n",
                         branch);
            return dstr_steal(&res);
         }
      }
   }

   /* 1. Fetch base (bounded + non-interactive so verify can't hang on a remote;
    * cwd=NULL uses the thread-local run_cmd CWD, like the run_cmd calls below). */
   char cmd[256];
   int rc;
   const char *fetch_argv[] = {"fetch", "origin", base_branch, NULL};
   char *out = NULL;
   rc = git_net_exec(NULL, fetch_argv, &out, 4096);
   dstr_appendf(&res, "1. Fetching base branch... %s\n", (rc == 0) ? "PASS" : "FAIL");
   if (rc != 0 && out)
      dstr_append_str(&res, out);
   free(out);

   /* 2. Check ahead/behind */
   snprintf(cmd, sizeof(cmd), "git rev-list --count HEAD..origin/%s", base_branch);
   out = run_cmd(cmd, &rc);
   if (rc == 0 && out)
   {
      int behind = atoi(out);
      if (behind > 0)
         dstr_appendf(&res, "2. Comparison: BEHIND base by %d commit(s). Recommend rebase.\n",
                      behind);
      else
         dstr_append_str(&res, "2. Comparison: Up to date with base.\n");
   }
   free(out);

   /* 3. Check for conflicts (dry-run merge) */
   snprintf(cmd, sizeof(cmd), "git merge-tree $(git merge-base HEAD origin/%s) HEAD origin/%s",
            base_branch, base_branch);
   out = run_cmd(cmd, &rc);
   if (rc == 0 && out)
   {
      if (strstr(out, "<<<<<<<"))
         dstr_append_str(&res, "3. Conflicts: POTENTIAL CONFLICTS found with base branch.\n");
      else
         dstr_append_str(&res, "3. Conflicts: Clean merge with base branch.\n");
   }
   else
   {
      /* fallback if merge-tree fails or not available in expected format */
      dstr_append_str(&res, "3. Conflicts: Could not determine mergeability.\n");
   }
   free(out);

   /* 4. Run verification steps (dependency-aware) */
   verify_config_t vcfg;
   if (verify_load_config(project_root, &vcfg) == 0)
   {
      dstr_append_str(&res, "\n4. Verification Steps:\n");

      verify_thread_ctx_t contexts[MAX_VERIFY_STEPS];
      memset(contexts, 0, sizeof(contexts));

      for (int i = 0; i < vcfg.count; i++)
      {
         contexts[i].step = &vcfg.steps[i];
         contexts[i].index = i;
         contexts[i].total = vcfg.count;
         contexts[i].rc = -1;
         if (project_root && project_root[0])
            snprintf(contexts[i].project_root, sizeof(contexts[i].project_root), "%s",
                     project_root);
      }

      verify_run_waves(&vcfg, contexts);

      for (int i = 0; i < vcfg.count; i++)
      {
         dstr_appendf(&res, "   [%d/%d] %s: %s (%.1fs)\n", i + 1, vcfg.count, vcfg.steps[i].name,
                      (contexts[i].rc == 0) ? "PASS" : "FAIL", contexts[i].elapsed);
         free(contexts[i].output);
      }
   }
   else
   {
      dstr_append_str(&res, "\n4. Verification: No steps configured in .aimee/project.yaml\n");
   }

   return dstr_steal(&res);
}
