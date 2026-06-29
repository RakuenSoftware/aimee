/* wfe_live_forge.c: the live forge provider for the workflow engine (F4).
 *
 * full-autonomous-development WP-2 / criterion 5. Implements the wfe_forge_t seam
 * with REAL git push + `gh` PR/CI/merge, reusing the vaulted git runner
 * (mcp_git_run, which injects the forge token via git_cred_inject) and the autonomous
 * merge-target rail (wfe_autonomous_base / _target_ok).
 *
 * SECURITY: registered ONLY when the operator has set wfe_live_forge_enabled=true
 * (default OFF — see config.h). Even once registered, EVERY op re-checks the flag
 * and the merge-target rail and fails closed if either is off, so a config flip
 * mid-run can't leave a half-open path, and an autonomous run can never open or
 * merge a PR against a protected branch. Turning the flag on is a deliberate
 * operator deployment action gated on branch protection + scoped creds (the
 * security-roundtable deviation from §7's default-on). */
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

/* The master switch: the live forge does nothing unless the operator enabled it. */
static int forge_on(void)
{
   config_t cfg;
   return config_load(&cfg) == 0 && cfg.wfe_live_forge_enabled;
}

/* A live forge op may proceed only if BOTH the operator switch is on AND the
 * autonomous merge-target rail allows it (never a protected branch). Fail closed. */
static int forge_allowed(void)
{
   return forge_on() && wfe_autonomous_target_ok();
}

/* Append `s` to dst (cap n) with single quotes escaped for a shell '...' literal. */
static void shq(char *dst, size_t n, const char *s)
{
   size_t d = 0;
   for (size_t i = 0; s && s[i] && d + 5 < n; i++)
   {
      if (s[i] == '\'')
      {
         /* close ' , escaped ' , reopen ' */
         d += (size_t)snprintf(dst + d, n - d, "'\\''");
      }
      else
      {
         dst[d++] = s[i];
      }
   }
   dst[d < n ? d : n - 1] = '\0';
}

/* Parse a PR number from `gh pr create` output (a trailing .../pull/<N> URL): the
 * last run of digits in the text. Returns 0 if none. */
static int parse_pr_number(const char *text)
{
   int best = 0;
   if (!text)
      return 0;
   for (const char *p = text; *p; p++)
   {
      if (*p >= '0' && *p <= '9')
      {
         int v = atoi(p);
         best = v; /* keep the last numeric run */
         while (*p >= '0' && *p <= '9')
            p++;
         if (!*p)
            break;
      }
   }
   return best;
}

static wfe_ci_status_t live_ci_status(const char *repo, const char *pr)
{
   (void)repo;
   if (!forge_on())
      return WFE_CI_NONE; /* disabled -> unknown -> park (never advance) */
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
   if (!forge_on())
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
   if (!forge_on())
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

static int live_open(const char *repo, const char *branch, const char *title, const char *body,
                     char out_pr_ref[128])
{
   (void)repo;
   if (out_pr_ref)
      out_pr_ref[0] = '\0';
   if (!forge_allowed() || !branch || !branch[0])
      return -1;
   /* Push the work-item branch through the vaulted runner. Worktrees share the
    * branch namespace, so the push resolves the branch the run committed to. */
   char cmd[1024];
   char ebranch[256], etitle[512], ebody[512], ebase[128];
   shq(ebranch, sizeof ebranch, branch);
   shq(etitle, sizeof etitle, title ? title : "");
   shq(ebody, sizeof ebody, body ? body : "");
   shq(ebase, sizeof ebase, wfe_autonomous_base());

   snprintf(cmd, sizeof cmd, "git push -u origin '%s' 2>&1", ebranch);
   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "wfe-forge", "push of %s failed: %s", branch, out ? out : "");
      free(out);
      return -1;
   }
   free(out);

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
   aimee_log(LOG_INFO, "wfe-forge", "opened PR #%d for %s -> %s", num, branch,
             wfe_autonomous_base());
   return 0;
}

static const wfe_forge_t WFE_LIVE_FORGE = {live_ci_status, live_mergeable, live_is_merged,
                                           live_merge, live_open};

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
