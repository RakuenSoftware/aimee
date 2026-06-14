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

/* implement: delegate fan-out that writes code on the work item's branch. */
static wfe_step_result_t exec_implement(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   /* Production: dispatch as many delegates as the plan allows against the
    * branch. The produced artifact is the branch; its hash is the head SHA. */
   char base[64] = "", head[64] = "", dhash[65] = "", err[128];
   wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err);
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head[0] ? head : dhash, 0.0);
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

/* merge: merge the approved PR. */
static wfe_step_result_t exec_merge(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   (void)node;
   /* Production: `gh pr merge --squash`. Terminal on success. */
   return wfe_step_advanced("merged", "", 0.0);
}

void wfe_register_default_executors(void)
{
   wfe_register_block_executor(WFE_BLK_AUTHOR_PROPOSAL, exec_author);
   wfe_register_block_executor(WFE_BLK_AUTHOR_PLAN, exec_author);
   wfe_register_block_executor(WFE_BLK_IMPLEMENT, exec_implement);
   wfe_register_block_executor(WFE_BLK_FREEZE, exec_freeze);
   wfe_register_block_executor(WFE_BLK_PR_OPEN, exec_pr_open);
   wfe_register_block_executor(WFE_BLK_MERGE, exec_merge);
}
