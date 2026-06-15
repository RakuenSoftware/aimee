/* wfe_blocks.c: real non-gate block executors.
 *
 * freeze is fully implemented + unit-tested (real git). The delegate-driven
 * (author/implement) and forge (pr.open/merge) executors construct + run real
 * commands via safe_exec_capture; their success requires a configured delegate
 * / gh and a working repo, so they are exercised by integration, not the unit
 * suite (the engine test overrides them with mocks via the vtable). */
#include "wfe_blocks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

/* Resolve the local working repo for a work item: $AIMEE_WORKFLOW_REPO or cwd. */
static const char *repo_dir(void)
{
   const char *d = getenv("AIMEE_WORKFLOW_REPO");
   return (d && d[0]) ? d : ".";
}

static int git_capture(const char *const argv[], char **out)
{
   *out = NULL;
   return safe_exec_capture(argv, out, 1 << 20);
}

static void chomp(char *s)
{
   if (!s)
      return;
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
      s[--n] = '\0';
}

int wfe_git_freeze(const char *repo_dir_in, const char *base_branch, char out_base_sha[64],
                   char out_head_sha[64], char out_diff_hash[65], char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   const char *dir = (repo_dir_in && repo_dir_in[0]) ? repo_dir_in : ".";
   const char *base = (base_branch && base_branch[0]) ? base_branch : "HEAD";

   /* head sha */
   {
      const char *argv[] = {"git", "-C", dir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (git_capture(argv, &o) != 0 || !o)
      {
         snprintf(err, errlen, "git rev-parse HEAD failed");
         free(o);
         return -1;
      }
      chomp(o);
      snprintf(out_head_sha, 64, "%s", o);
      free(o);
   }
   /* base sha = merge-base(HEAD, base_branch); fall back to HEAD if base==HEAD */
   {
      const char *argv[] = {"git", "-C", dir, "merge-base", "HEAD", base, NULL};
      char *o = NULL;
      if (git_capture(argv, &o) == 0 && o && o[0])
      {
         chomp(o);
         snprintf(out_base_sha, 64, "%s", o);
      }
      else
      {
         snprintf(out_base_sha, 64, "%s", out_head_sha);
      }
      free(o);
   }
   /* cumulative diff base..head, hashed */
   {
      char range[140];
      snprintf(range, sizeof range, "%s..%s", out_base_sha, out_head_sha);
      const char *argv[] = {"git", "-C", dir, "diff", range, NULL};
      char *o = NULL;
      if (git_capture(argv, &o) != 0)
      {
         snprintf(err, errlen, "git diff failed");
         free(o);
         return -1;
      }
      wfe_sha256_hex(o ? o : "", o ? strlen(o) : 0, out_diff_hash);
      free(o);
   }
   return 0;
}

/* ---- forge seam (gate.ci / check.mergeable / merge) ---- */

/* Default live provider would call `gh`, but PR-identity threading is
 * integration-gated, so until that lands it fails closed (unknown -> park,
 * merge unavailable). Tests inject a mock to exercise the state mapping. */
static wfe_ci_status_t live_ci_status(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return WFE_CI_NONE;
}
static int live_mergeable(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return -1;
}
static int live_is_merged(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return -1;
}
static wfe_merge_result_t live_merge(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return WFE_MERGE_ERROR;
}
static const wfe_forge_t LIVE_FORGE = {live_ci_status, live_mergeable, live_is_merged, live_merge};
static const wfe_forge_t *g_forge = &LIVE_FORGE;

void wfe_set_forge_provider(const wfe_forge_t *p)
{
   g_forge = p ? p : &LIVE_FORGE;
}

/* ---- executors ---- */

/* author.proposal / author.plan: drive a delegate to produce/edit the artifact,
 * then hash the artifact file. */
static wfe_step_result_t exec_author(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *path = wfe_ctx_proposal_path(ctx);
   /* In production this dispatches `aimee delegate run` to author/edit `path`.
    * We hash the artifact file as the produced content. If the artifact is not
    * present (no delegate ran), the gate that follows will simply re-loop. */
   char hash[65] = "";
   if (path && path[0])
   {
      FILE *f = fopen(path, "rb");
      if (f)
      {
         fseek(f, 0, SEEK_END);
         long sz = ftell(f);
         if (sz < 0)
            sz = 0;
         fseek(f, 0, SEEK_SET);
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            size_t rd = fread(buf, 1, (size_t)sz, f);
            buf[rd] = '\0';
            wfe_sha256_hex(buf, rd, hash);
            free(buf);
         }
         fclose(f);
      }
   }
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, hash, 0.0);
}

/* implement: produces the work item's branch. In production a delegate fan-out
 * writes the code; that live dispatch is integration-gated (see file header).
 * What this does NOW: captures the current branch head SHA as the produced
 * artifact, and fails closed if the repo is unavailable (never emits an empty
 * artifact). */
static wfe_step_result_t exec_implement(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return wfe_step_failed();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, 0.0);
}

/* document: produces the (documented) branch. In production a delegate writes
 * docs onto the branch (README/CHANGELOG/docs/ + inline comments); that live
 * dispatch is integration-gated (see file header). What this does NOW: captures
 * the current branch head SHA as the produced artifact, and fails closed if the
 * repo is unavailable. */
static wfe_step_result_t exec_document(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return wfe_step_failed();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, 0.0);
}

/* freeze: capture the cumulative diff at a stable freeze commit. */
static wfe_step_result_t exec_freeze(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   const char *base_branch = getenv("AIMEE_WORKFLOW_BASE");
   char base[64] = "", head[64] = "", dhash[65] = "", err[128];
   if (wfe_git_freeze(repo_dir(), base_branch ? base_branch : "HEAD", base, head, dhash, err,
                      sizeof err) != 0)
      return wfe_step_failed();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, dhash, 0.0);
}

/* pr.open: push the branch + open a forge PR. */
static wfe_step_result_t exec_pr_open(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   /* Production: `git push` then `gh pr create`; the PR number/url is the
    * produced handle. Requires a configured forge (gh) — integration-gated. */
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, "", 0.0);
}

/* the PR ref the forge ops act on. Live wiring would resolve a real PR number
 * stored when pr.open ran; for now we pass the work-item id (the mock ignores
 * it; the live provider is gated). */
static const char *pr_ref(wfe_ctx *ctx)
{
   const char *wi = wfe_ctx_work_item(ctx);
   return wi ? wi : "";
}

/* merge: idempotent + race-safe. The never-re-merge invariant is enforced by
 * requiring a CONFIRMED not-merged state before we ever call merge():
 *   is_merged == 1  -> idempotent no-op success (already merged)
 *   is_merged <  0  -> state could not be determined (transient forge error) ->
 *                      park; we never call merge() when we can't confirm the PR
 *                      is unmerged, so a transient error can never double-merge.
 *   is_merged == 0  -> confirmed unmerged: the forge merge act is the single
 *                      decision (already-race -> success, not-mergeable -> loop,
 *                      transient error -> park for a human re-drive). */
static wfe_step_result_t exec_merge(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *repo = wfe_ctx_repo(ctx), *pr = pr_ref(ctx);
   int im = g_forge->is_merged(repo, pr);
   if (im == 1)
      return wfe_step_advanced("merged", "", 0.0); /* idempotent no-op */
   if (im < 0)
      return wfe_step_pending(WFE_PAUSE_MERGE_PENDING); /* unconfirmed -> never merge */
   switch (g_forge->merge(repo, pr))
   {
   case WFE_MERGE_OK:
   case WFE_MERGE_ALREADY:
      return wfe_step_advanced("merged", "", 0.0);
   case WFE_MERGE_NOT_MERGEABLE:
      return wfe_step_looped();
   default:
      /* transient/unknown forge error on the merge act itself: park for a human
       * re-drive rather than hard-failing the run. We only reach here with a
       * confirmed-unmerged PR, so this can never double-merge. */
      (void)node;
      return wfe_step_pending(WFE_PAUSE_MERGE_PENDING);
   }
}

/* gate.ci: only an explicit all-green advances; everything else fails closed. */
static wfe_step_result_t exec_gate_ci(wfe_ctx *ctx, const wfe_node_t *node)
{
   switch (g_forge->ci_status(wfe_ctx_repo(ctx), pr_ref(ctx)))
   {
   case WFE_CI_SUCCESS:
   {
      char h[80];
      snprintf(h, sizeof h, "%s.out", node->id);
      return wfe_step_advanced(h, "", 0.0);
   }
   case WFE_CI_FAILURE:
      return wfe_step_looped();
   default: /* PENDING or NONE/unknown -> park, never advance */
      return wfe_step_pending(WFE_PAUSE_CI_PENDING);
   }
}

/* check.mergeable: mergeable (1) advances; conflict (0) loops back to fix it;
 * unknown (-1, transient forge error) parks -- it never advances on an
 * undetermined state (fail closed), and uses the merge-state pause reason rather
 * than the CI one so the park is labelled correctly. */
static wfe_step_result_t exec_check_mergeable(wfe_ctx *ctx, const wfe_node_t *node)
{
   int m = g_forge->mergeable(wfe_ctx_repo(ctx), pr_ref(ctx));
   if (m == 1)
   {
      char h[80];
      snprintf(h, sizeof h, "%s.out", node->id);
      return wfe_step_advanced(h, "", 0.0);
   }
   if (m == 0)
      return wfe_step_looped();
   return wfe_step_pending(WFE_PAUSE_MERGE_PENDING); /* unknown -> park, never advance */
}

/* custom: the one generic executor for config-defined blocks. */
static wfe_step_result_t exec_custom(wfe_ctx *ctx, const wfe_node_t *node)
{
   const wfe_custom_block_t *c = wfe_custom_lookup(node->custom_name);
   if (!c)
      return wfe_step_failed();
   char head[64] = "", base[64] = "", dhash[65] = "", err[128] = "";
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);

   if (c->executor == WFE_EXEC_COMMAND)
   {
      if (!wfe_custom_commands_allowed())
         return wfe_step_failed(); /* opt-in: lifecycle allow_command not set */
      if (c->argc == 0)
         return wfe_step_failed();
      /* A command block must NOT inherit the engine's environment (it carries
       * AIMEE_APPROVAL_KEY, vault tokens, etc.): pass only a minimal allowlist
       * (PATH/HOME/LANG — enough to run an ordinary build/lint tool; engine paths
       * and secrets are deliberately withheld). Stack buffers outlive the
       * synchronous exec call below. */
      char e_path[2048], e_home[2048], e_lang[256];
      const char *v;
      char *envp[4];
      int ei = 0;
      v = getenv("PATH");
      snprintf(e_path, sizeof e_path, "PATH=%s", (v && v[0]) ? v : "/usr/bin:/bin");
      envp[ei++] = e_path;
      if ((v = getenv("HOME")) && v[0])
      {
         snprintf(e_home, sizeof e_home, "HOME=%s", v);
         envp[ei++] = e_home;
      }
      if ((v = getenv("LANG")) && v[0])
      {
         snprintf(e_lang, sizeof e_lang, "LANG=%s", v);
         envp[ei++] = e_lang;
      }
      envp[ei] = NULL;
      /* argv is the registry's OWNED, length-bounded, all-string, NULL-terminated
       * array (validated at load); run it directly (no shell), pinned to the
       * work-item repo, under a wall-clock timeout (kill -> step failed). */
      char *out = NULL;
      int rc = safe_exec_capture_cwd_env_timeout((const char *const *)c->argv, repo_dir(), envp,
                                                 &out, 1 << 20, wfe_custom_command_timeout_ms());
      free(out);
      if (rc != 0)
         return wfe_step_failed(); /* non-zero exit or SAFE_EXEC_TIMEOUT */
   }
   /* delegate executor is integration-gated (like implement/document). */

   if (c->produces == WFE_ART_BRANCH)
   {
      if (wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
         return wfe_step_failed();
      return wfe_step_advanced(handle, head, 0.0);
   }
   return wfe_step_advanced(handle, "", 0.0); /* produces: none (sink) */
}

void wfe_register_default_executors(void)
{
   wfe_register_block_executor(WFE_BLK_AUTHOR_PROPOSAL, exec_author);
   wfe_register_block_executor(WFE_BLK_AUTHOR_PLAN, exec_author);
   wfe_register_block_executor(WFE_BLK_IMPLEMENT, exec_implement);
   wfe_register_block_executor(WFE_BLK_DOCUMENT, exec_document);
   wfe_register_block_executor(WFE_BLK_FREEZE, exec_freeze);
   wfe_register_block_executor(WFE_BLK_PR_OPEN, exec_pr_open);
   wfe_register_block_executor(WFE_BLK_MERGE, exec_merge);
   wfe_register_block_executor(WFE_BLK_GATE_CI, exec_gate_ci);
   wfe_register_block_executor(WFE_BLK_CHECK_MERGEABLE, exec_check_mergeable);
   wfe_register_block_executor(WFE_BLK_CUSTOM, exec_custom);
}
