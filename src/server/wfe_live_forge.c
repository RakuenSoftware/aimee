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
#include "log.h"
#include "mcp_git.h"
#include "wfe_blocks.h"
#include "wfe_iface.h" /* wfe_autonomous_base / wfe_autonomous_target_ok (merge-target rail) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Single-quote-escape `s` into a shell '...' literal in `dst` (cap n). Returns 0 on
 * success, -1 if it would TRUNCATE — the caller must abort on -1, never run a
 * half-built command. */
static int shq(char *dst, size_t n, const char *s)
{
   if (!dst || n == 0)
      return -1;
   size_t d = 0;
   for (size_t i = 0; s && s[i]; i++)
   {
      if (s[i] == '\'')
      {
         if (d + 4 >= n)
         {
            dst[n - 1] = '\0';
            return -1;
         }
         memcpy(dst + d, "'\\''", 4);
         d += 4;
      }
      else
      {
         if (d + 1 >= n)
         {
            dst[n - 1] = '\0';
            return -1;
         }
         dst[d++] = s[i];
      }
   }
   dst[d] = '\0';
   return 0;
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
   char cmd[256];
   snprintf(cmd, sizeof cmd, "gh pr checks %d 2>&1", num);
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   wfe_ci_status_t st = WFE_CI_NONE; /* default fail-closed */
   if (out && strstr(out, "no checks reported"))
      st = WFE_CI_NONE; /* no CI -> don't advance */
   else if (rc == 0)
      st = WFE_CI_SUCCESS; /* gh: all checks passed */
   else if (rc == 8)
      st = WFE_CI_PENDING; /* gh: some still running */
   else if (rc == 1)
      st = WFE_CI_FAILURE; /* gh: some failed */
   /* any other rc -> NONE (unknown) -> park */
   free(out);
   return st;
}

static int live_mergeable(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return -1;
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return -1;
   char cmd[256];
   snprintf(cmd, sizeof cmd, "gh pr view %d --json mergeable -q .mergeable 2>&1", num);
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   int res = -1; /* unknown -> park */
   if (rc == 0 && out)
   {
      if (strstr(out, "MERGEABLE"))
         res = 1;
      else if (strstr(out, "CONFLICTING"))
         res = 0;
   }
   free(out);
   return res;
}

static int live_is_merged(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return -1;
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return -1;
   char cmd[256];
   snprintf(cmd, sizeof cmd, "gh pr view %d --json state -q .state 2>&1", num);
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   int res = -1; /* unknown -> the engine parks (never merges on an undetermined state) */
   if (rc == 0 && out)
      res = strstr(out, "MERGED") ? 1 : 0;
   free(out);
   return res;
}

static wfe_merge_result_t live_merge(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_allowed())
      return WFE_MERGE_ERROR; /* disabled / protected target -> fail closed */
   int num = pr ? atoi(pr) : 0;
   if (num <= 0)
      return WFE_MERGE_ERROR;
   char cmd[256];
   snprintf(cmd, sizeof cmd, "gh pr merge %d --squash 2>&1", num);
   /* Re-check immediately before the mutating call: close the TOCTOU window where
    * the flag is flipped off / the base becomes protected after the entry check. */
   if (!forge_allowed())
      return WFE_MERGE_ERROR;
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   wfe_merge_result_t res;
   if (rc == 0)
      res = WFE_MERGE_OK;
   else if (out && strstr(out, "already merged"))
      res = WFE_MERGE_ALREADY;
   else if (out && (strstr(out, "not mergeable") || strstr(out, "conflict")))
      res = WFE_MERGE_NOT_MERGEABLE;
   else
      res = WFE_MERGE_ERROR; /* unknown -> park for a human */
   free(out);
   return res;
}

/* The repo's real default branch as the vaulted runner sees it: AIMEE_DEFAULT_BRANCH
 * override, else origin/HEAD (e.g. "origin/main" -> "main"). Returns 0 with `buf`
 * filled on success, -1 (and clears `buf`) if the trunk can't be determined -- there is
 * NO "main" guess, so a protected base can be permitted ONLY when we positively
 * resolved it to be the real trunk. Mirrors wfe_repo_default_branch() in the engine so
 * base:trunk means the same trunk on both sides of the forge seam. */
static int live_default_branch(char *buf, size_t n)
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
   char *out = mcp_git_run("git symbolic-ref --short refs/remotes/origin/HEAD 2>&1", &rc);
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
   (void)repo;
   if (out_pr_ref)
      out_pr_ref[0] = '\0';
   if (!forge_allowed() || !branch || !branch[0])
      return -1;
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
      if (live_default_branch(trunk, sizeof trunk) != 0 || strcmp(base, trunk) != 0)
      {
         aimee_log(LOG_WARN, "wfe-forge",
                   "refusing PR open against protected base '%s' (not the resolved repo trunk)",
                   base);
         return -1;
      }
   }

   /* Escape every interpolated value; ABORT on any truncation (never run a
    * half-built shell command). */
   char ebranch[256], etitle[512], ebody[1024], ebase[128];
   if (shq(ebranch, sizeof ebranch, branch) != 0 ||
       shq(etitle, sizeof etitle, title ? title : "") != 0 ||
       shq(ebody, sizeof ebody, body ? body : "") != 0 || shq(ebase, sizeof ebase, base) != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "live_open: arg too long for %s", branch);
      return -1;
   }

   /* Push the work-item branch through the vaulted runner. Worktrees share the
    * branch namespace, so the push resolves the branch the run committed to. */
   char cmd[2048];
   int rc;
   if (!forge_allowed()) /* re-check just before the mutating push (TOCTOU) */
      return -1;
   snprintf(cmd, sizeof cmd, "git push -u origin '%s' 2>&1", ebranch);
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "push of %s failed: %s", branch, out ? out : "");
      free(out);
      return -1;
   }
   free(out);

   if (!forge_allowed()) /* re-check just before the mutating PR create (TOCTOU) */
      return -1;
   snprintf(cmd, sizeof cmd, "gh pr create --head '%s' --base '%s' --title '%s' --body '%s' 2>&1",
            ebranch, ebase, etitle, ebody);
   out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "pr create for %s failed: %s", branch, out ? out : "");
      free(out);
      return -1;
   }
   int num = parse_pr_number(out);
   free(out);
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
   (void)repo;
   if (!forge_allowed() || !branch || !branch[0] || wfe_base_is_protected(branch))
      return -1;
   char ebranch[256];
   if (shq(ebranch, sizeof ebranch, branch) != 0)
      return -1;
   char cmd[512];
   snprintf(cmd, sizeof cmd, "git push -u origin '%s' 2>&1", ebranch);
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
      aimee_log(LOG_WARN, "wfe-forge", "publish feature base %s failed: %s", branch,
                out ? out : "");
   free(out);
   return rc == 0 ? 0 : -1;
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
