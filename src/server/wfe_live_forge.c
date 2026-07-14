/* wfe_live_forge.c: the live forge provider for the workflow engine (F4).
 *
 * full-autonomous-development WP-2 / criterion 5. Implements the wfe_forge_t seam
 * with REAL git push + `gh` PR/CI/merge, reusing the vaulted git runner
 * (mcp_git_run, which injects the forge token via git_cred_inject) and the autonomous
 * merge-target rail (wfe_autonomous_base / _target_ok).
 *
 * SECURITY: registered unless the operator has set wfe_live_forge_enabled=false
 * (default ON — operator ruling 2026-07-13, restoring the plan's default; see
 * config.h). Registered or not, EVERY op re-checks the flag AND the merge-target
 * rail via forge_allowed() and fails closed if either is off — including
 * immediately before each mutating git/gh call, so a config flip or a base
 * misconfig mid-op can't slip a push/PR/merge through (TOCTOU-safe). An
 * autonomous run can never MERGE into a protected branch; it may OPEN a
 * human-reviewed PR against the repo's own default branch (trunk) -- open-only, never
 * auto-merged (the default "build" workflow's terminal). Any OTHER protected base
 * stays refused. Real pushes additionally require forge creds (the vaulted git
 * runner); without them ops fail cleanly and the run parks. */
#include "aimee.h"

#include "wfe_live_forge.h"

#include "config.h"
#include "git_cred_inject.h" /* FD-mode env for the branch push (token on a memfd) */
#include "git_pr_api.h"      /* in-process GitHub REST: create/info/ci/merge */
#include "log.h"
#include "util.h" /* safe_exec_capture* */
#include "wfe_blocks.h"
#include "wfe_iface.h" /* wfe_autonomous_base / wfe_autonomous_target_ok (merge-target rail) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* The local checkout every forge op acts on: the work item's repo binding
 * resolved to a directory (else $AIMEE_WORKFLOW_REPO). All git here is argv-only
 * with an explicit -C dir — never a shell string, never the daemon's cwd. */
static const char *forge_dir(const char *repo)
{
   return wfe_repo_local(repo);
}

/* Push `branch` from `dir` through the sanctioned credential path: the vaulted
 * token rides a CLOEXEC memfd duplicated onto GIT_CRED_TOKEN_TARGET_FD in the
 * git child, where the askpass shim reads it — it never enters the child's
 * environment or argv (the same FD mode git_ops uses). NULL principal resolves
 * per-host vault -> server forge identity. */
static int forge_push_branch(const char *dir, const char *branch, const char *what)
{
   const char *argv[] = {"git", "-C", dir, "push", "-u", "origin", branch, NULL};
   int token_fd = -1;
   char **envp = git_cred_inject_build_env_for_repo(NULL, NULL, dir, NULL, environ, &token_fd);
   char *out = NULL;
   int rc = safe_exec_capture_cwd_env_fd_timeout(argv, dir, envp ? envp : environ, &out, 1 << 16,
                                                 120000, token_fd,
                                                 token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
   if (token_fd >= 0)
      close(token_fd);
   if (envp)
      git_cred_inject_free_env(envp);
   if (rc != 0)
      aimee_log(LOG_WARN, "wfe-forge", "%s %s failed: %s", what, branch, out ? out : "");
   free(out);
   return rc == 0 ? 0 : -1;
}

/* The master switch: the live forge does nothing unless the operator enabled it.
 * A config_load failure reads as DISABLED (fail closed). */
static int forge_on(void)
{
   config_t cfg;
   return config_load(&cfg) == 0 && cfg.wfe_live_forge_enabled;
}

/* A live forge op may proceed only if BOTH the operator switch is on AND the
 * autonomous merge-target rail allows it (never a protected branch). Fail closed.
 * Used by EVERY op (read and write) for a single, uniform safety predicate. */
static int forge_allowed(void)
{
   return forge_on() && wfe_autonomous_target_ok();
}

/* Parse the PR number from the canonical .../pull/<N> URL `gh pr create` prints.
 * Anchored on "/pull/" + a contiguous integer, so a later digit-bearing diagnostic
 * can't overwrite it. Returns 0 if not found / invalid. */
static int parse_pr_number(const char *text)
{
   if (!text)
      return 0;
   const char *m = strstr(text, "/pull/");
   if (!m)
      return 0;
   m += 6;
   if (*m < '0' || *m > '9')
      return 0;
   long v = strtol(m, NULL, 10);
   return (v > 0 && v <= 2147483647L) ? (int)v : 0;
}

static wfe_ci_status_t live_ci_status(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return WFE_CI_NONE; /* disabled / protected -> unknown -> park (never advance) */
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return WFE_CI_NONE;
   char err[160];
   switch (git_pr_ci_via_api(NULL, forge_dir(repo), num, err, sizeof err))
   {
   case GIT_PR_CI_SUCCESS:
      return WFE_CI_SUCCESS;
   case GIT_PR_CI_PENDING:
      return WFE_CI_PENDING;
   case GIT_PR_CI_FAILURE:
      return WFE_CI_FAILURE;
   case GIT_PR_CI_ERROR:
      aimee_log(LOG_WARN, "wfe-forge", "ci status for PR %d: %s", num, err);
      /* unknown -> NONE -> park (never advance) */
      return WFE_CI_NONE;
   case GIT_PR_CI_NONE:
   default:
      return WFE_CI_NONE;
   }
}

static int live_mergeable(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return -1;
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return -1;
   git_pr_info_t info;
   char err[160];
   if (git_pr_info_via_api(NULL, forge_dir(repo), num, &info, err, sizeof err) != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "mergeable for PR %d: %s", num, err);
      return -1; /* unknown -> park */
   }
   return info.mergeable; /* 1 / 0 / -1 (GitHub still computing -> park) */
}

static int live_is_merged(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return -1;
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return -1;
   git_pr_info_t info;
   char err[160];
   if (git_pr_info_via_api(NULL, forge_dir(repo), num, &info, err, sizeof err) != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "is_merged for PR %d: %s", num, err);
      return -1; /* unknown -> the engine parks (never merges on an undetermined state) */
   }
   return info.merged ? 1 : 0;
}

static wfe_merge_result_t live_merge(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return WFE_MERGE_ERROR; /* disabled / protected target -> fail closed */
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return WFE_MERGE_ERROR;
   /* Re-check immediately before the mutating call: close the TOCTOU window where
    * the flag is flipped off / the base becomes protected after the entry check. */
   if (!forge_allowed())
      return WFE_MERGE_ERROR;
   char err[200];
   switch (git_pr_merge_via_api(NULL, forge_dir(repo), num, err, sizeof err))
   {
   case 0:
      return WFE_MERGE_OK;
   case 1:
      return WFE_MERGE_ALREADY;
   case 2:
      aimee_log(LOG_WARN, "wfe-forge", "merge of PR %d: %s", num, err);
      return WFE_MERGE_NOT_MERGEABLE;
   default:
      aimee_log(LOG_WARN, "wfe-forge", "merge of PR %d: %s", num, err);
      return WFE_MERGE_ERROR; /* unknown -> park for a human */
   }
}

/* The repo's real default branch as the vaulted runner sees it: AIMEE_DEFAULT_BRANCH
 * override, else origin/HEAD (e.g. "origin/main" -> "main"). Returns 0 with `buf`
 * filled on success, -1 (and clears `buf`) if the trunk can't be determined -- there is
 * NO "main" guess, so a protected base can be permitted ONLY when we positively
 * resolved it to be the real trunk. Mirrors wfe_repo_default_branch() in the engine so
 * base:trunk means the same trunk on both sides of the forge seam. */
static int live_default_branch(const char *dir, char *buf, size_t n)
{
   if (!buf || n < 2)
      return -1;
   buf[0] = '\0';
   const char *env = getenv("AIMEE_DEFAULT_BRANCH");
   if (env && env[0])
   {
      snprintf(buf, n, "%s", env);
      return 0;
   }
   int rc = -1;
   const char *argv[] = {"git", "-C", dir, "symbolic-ref", "--short", "refs/remotes/origin/HEAD",
                         NULL};
   char *out = NULL;
   if (safe_exec_capture(argv, &out, 4096) != 0)
      rc = -1;
   else
      rc = 0;
   if (rc == 0 && out)
   {
      size_t l = strlen(out);
      while (l &&
             (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ' || out[l - 1] == '\t'))
         out[--l] = '\0';
      const char *name = (strncmp(out, "origin/", 7) == 0) ? out + 7 : out;
      if (name[0])
      {
         snprintf(buf, n, "%s", name);
         free(out);
         return 0;
      }
   }
   free(out);
   return -1; /* unresolved -> caller refuses a protected base (no "main" guess) */
}

static int live_open(const char *repo, const char *branch, const char *base, const char *title,
                     const char *body, char out_pr_ref[128])
{
   if (out_pr_ref)
      out_pr_ref[0] = '\0';
   if (!forge_allowed() || !branch || !branch[0])
      return -1;
   const char *dir = forge_dir(repo);
   /* Defence in depth beyond exec_pr_open: refuse an empty base, and refuse a protected
    * base UNLESS it is the repo's own default branch (trunk). pr.open only OPENS a PR
    * (never merges), so a human-reviewed PR against the trunk is the intended terminal;
    * any other protected base (a release-train branch, a non-trunk master, ...) is a
    * misconfig and stays refused. live_merge never merges into a protected base. */
   if (!base || !base[0])
      return -1;
   if (wfe_base_is_protected(base))
   {
      char trunk[64];
      if (live_default_branch(dir, trunk, sizeof trunk) != 0 || strcmp(base, trunk) != 0)
      {
         aimee_log(LOG_WARN, "wfe-forge",
                   "refusing PR open against protected base '%s' (not the resolved repo trunk)",
                   base);
         return -1;
      }
   }

   /* Push the work-item branch (worktrees share the branch namespace, so the
    * shared checkout resolves the branch the run committed to), then open the
    * PR in-process via the GitHub REST API — the forge credential stays inside
    * aimee-server (Authorization header) or on the push's memfd; it never
    * reaches a child environment, argv, or a gh process. */
   if (!forge_allowed()) /* re-check just before the mutating push (TOCTOU) */
      return -1;
   if (forge_push_branch(dir, branch, "push of") != 0)
      return -1;

   if (!forge_allowed()) /* re-check just before the mutating PR create (TOCTOU) */
      return -1;
   /* git_pr_create_via_api_ex strips AI attribution from the body itself. */
   char url[512] = "", err[200] = "";
   if (git_pr_create_via_api_ex(NULL, dir, branch, base, title ? title : "", body ? body : "", url,
                                sizeof url, err, sizeof err) != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "pr create for %s failed: %s", branch, err);
      return -1;
   }
   int num = parse_pr_number(url);
   if (num <= 0)
      return -1;
   snprintf(out_pr_ref, 128, "%d", num);
   aimee_log(LOG_INFO, "wfe-forge", "opened PR #%d for %s -> %s", num, branch, base);
   return 0;
}

/* Publish the durable feature branch (aimee/feat/<id>) to the forge so slice
 * sub-PRs can target it as their base. Pushes the local branch through the vaulted
 * runner; refuses a protected/empty branch (defence in depth). */
static int live_publish_base(const char *repo, const char *branch)
{
   if (!forge_allowed() || !branch || !branch[0] || wfe_base_is_protected(branch))
      return -1;
   return forge_push_branch(forge_dir(repo), branch, "publish feature base");
}

static const wfe_forge_t WFE_LIVE_FORGE = {live_ci_status, live_mergeable, live_is_merged,
                                           live_merge,     live_open,      live_publish_base};

void wfe_live_forge_register(void)
{
   /* Register ONLY when the operator has enabled it. While off, the engine keeps
    * its default fail-closed stub (pr.open re-loops; merge parks), so a submitted
    * proposal never touches the forge. */
   if (!forge_on())
   {
      aimee_log(LOG_INFO, "wfe-forge", "live forge OFF (wfe_live_forge_enabled=false)");
      return;
   }
   wfe_set_forge_provider(&WFE_LIVE_FORGE);
   aimee_log(LOG_WARN, "wfe-forge",
             "live forge ENABLED — autonomous runs can open + merge real PRs to %s",
             wfe_autonomous_base());
}
