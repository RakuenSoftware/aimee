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
#include <time.h>

#include "cJSON.h"
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
/* Default forge open: integration-gated (the live provider — git push + gh pr
 * create — is registered by the server). Fail closed here so pr.open re-loops. */
static int live_open(const char *repo, const char *branch, const char *title, const char *body,
                     char out_pr_ref[128])
{
   (void)repo;
   (void)branch;
   (void)title;
   (void)body;
   if (out_pr_ref)
      out_pr_ref[0] = '\0';
   return -1;
}
static const wfe_forge_t LIVE_FORGE = {live_ci_status, live_mergeable, live_is_merged, live_merge,
                                       live_open};
static const wfe_forge_t *g_forge = &LIVE_FORGE;

void wfe_set_forge_provider(const wfe_forge_t *p)
{
   g_forge = p ? p : &LIVE_FORGE;
}

/* Delegate seam (see wfe_blocks.h). Default NULL: producing blocks fail closed
 * until the server registers a live provider, preserving the integration-gated
 * behavior the stubs had before. */
static const wfe_delegate_provider_t *g_delegate = NULL;

void wfe_set_delegate_provider(const wfe_delegate_provider_t *p)
{
   g_delegate = p;
}

/* Verify seam (see wfe_blocks.h). Default NULL: implement does not gate on
 * verification (pre-WP-1b behavior). */
static const wfe_verify_provider_t *g_verify = NULL;

void wfe_set_verify_provider(const wfe_verify_provider_t *p)
{
   g_verify = p;
}

/* Run the mechanical verify gate on the implemented worktree. Returns 1 to ADVANCE
 * (only when the top-level verdict is an explicit "passed"); 0 to BLOCK in every
 * other case — FAIL CLOSED. Blocking cases: no provider installed (a missing safety
 * gate must never let unverified work advance), the gate could not run, an
 * unparseable verdict, or a verdict other than "passed". We parse the git_verify
 * format=json document and read its TOP-LEVEL "verdict" field, so a nested or echoed
 * verdict token cannot spoof the gate. Exposed (non-static) for the unit test. */
int wfe_implement_verify_ok(const char *workdir)
{
   if (!g_verify || !g_verify->verify)
      return 0; /* no gate -> fail closed (unverified work never advances) */
   char verdict[4096] = "";
   if (g_verify->verify(workdir, verdict, sizeof verdict) != 0)
      return 0; /* gate could not run -> fail closed */
   cJSON *doc = cJSON_Parse(verdict);
   if (!doc)
      return 0; /* unparseable -> fail closed */
   const cJSON *vd = cJSON_GetObjectItemCaseSensitive(doc, "verdict");
   int passed = cJSON_IsString(vd) && vd->valuestring && strcmp(vd->valuestring, "passed") == 0;
   cJSON_Delete(doc);
   return passed ? 1 : 0;
}

/* The step's assigned delegate from node params ("delegate"), or "" for none.
 * May be the sentinel "$random" — the provider resolves it to a random agent. */
static const char *node_delegate(const wfe_node_t *node)
{
   if (!node || !node->params)
      return "";
   const cJSON *d = cJSON_GetObjectItemCaseSensitive(node->params, "delegate");
   return (d && cJSON_IsString(d) && d->valuestring) ? d->valuestring : "";
}

/* Dispatch one block's delegate work, if a provider is installed. `delegate` is
 * the step's assigned agent (or "$random", or "" to route by role). out_cost (may
 * be NULL) receives the server-side wall-clock USD estimate for the turn (WP-5
 * budget). Returns:
 *   1  provider ran and succeeded,
 *   0  no provider installed (caller falls back to its fail-closed path),
 *  -1  provider ran and failed (caller should loop/retry). */
static int wfe_delegate_dispatch(const char *role, const char *delegate, const char *prompt,
                                 const char *artifact_path, char out_commit_sha[64],
                                 double *out_cost)
{
   if (out_commit_sha)
      out_commit_sha[0] = '\0';
   if (out_cost)
      *out_cost = 0.0;
   if (!g_delegate || !g_delegate->run)
      return 0;
   char err[256] = "";
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int rc = g_delegate->run(repo_dir(), role, delegate ? delegate : "", prompt, artifact_path,
                            out_commit_sha, err, sizeof err);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   if (out_cost)
      *out_cost = wfe_autonomy_cost_estimate((double)(t1.tv_sec - t0.tv_sec) +
                                             (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
   return rc == 0 ? 1 : -1;
}

/* ---- executors ---- */

/* author.proposal / author.plan: drive a delegate to produce/edit the artifact,
 * then hash the artifact file. */
static wfe_step_result_t exec_author(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *path = wfe_ctx_proposal_path(ctx);
   /* Dispatch a delegate to author/edit `path` (no-op if no provider installed;
    * a failed run loops). Then hash the artifact file as the produced content; if
    * it is absent (no provider ran) the gate that follows simply re-loops. */
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch("architect", node_delegate(node),
                             "Author or revise the workflow artifact at the given path "
                             "per the work item, then commit it.",
                             path, commit, &cost) < 0)
      return wfe_step_looped();
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
   return wfe_step_advanced(handle, hash, cost);
}

/* implement: the manager loop. The live delegate provider owns decompose -> fan
 * out -> verify -> re-delegate (see the full-autonomous-development plan); this
 * executor dispatches it, then captures the branch head SHA as the produced
 * artifact. A failed dispatch loops (retry); fails closed if the repo is
 * unavailable. With no provider installed it preserves the prior freeze-only
 * behavior so the engine remains drivable without a live delegate. */
static wfe_step_result_t exec_implement(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch(
           "engineer", node_delegate(node),
           "Implement the approved plan on the work-item branch: split into units, delegate each, "
           "VERIFY each with `aimee git verify` and fix any failures, then commit the accepted "
           "work.",
           NULL, commit, &cost) < 0)
      return wfe_step_looped();
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return wfe_step_failed();
   /* Mechanical verify gate (WP-1b): a unit only advances if it PASSES. A failed
    * verdict loops back to implement (the engine bounds the retries via
    * stage_attempt and parks max_attempts on exhaustion); a re-dispatched fresh
    * engineer delegate has the verify tool to see + fix the findings. */
   if (!wfe_implement_verify_ok(repo_dir()))
      return wfe_step_looped();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, cost);
}

/* document: produces the (documented) branch. In production a delegate writes
 * docs onto the branch (README/CHANGELOG/docs/ + inline comments); that live
 * dispatch is integration-gated (see file header). What this does NOW: captures
 * the current branch head SHA as the produced artifact, and fails closed if the
 * repo is unavailable. */
static wfe_step_result_t exec_document(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch("engineer", node_delegate(node),
                             "Document the change on the work-item branch (README/CHANGELOG/docs "
                             "+ inline comments), then commit.",
                             NULL, commit, &cost) < 0)
      return wfe_step_looped();
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(repo_dir(), "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return wfe_step_failed();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, cost);
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

/* pr.open: push the branch + open a forge PR via the forge seam. The PR ref the
 * forge returns becomes the produced content. A failed open loops (retry). With
 * no live `open` installed (default/mocks) it preserves the prior advance so the
 * engine stays drivable without a forge. */
/* A forge PR ref must be non-empty, printable, and fit the wfe_step_result_t
 * content_hash transport (it rides that field to the engine, which persists it as
 * the work item's pr_ref). A live provider that signals success with a junk or
 * over-long ref is rejected so a bogus ref never reaches the merge gates. */
static int pr_ref_is_sane(const char *ref)
{
   size_t n = ref ? strlen(ref) : 0;
   if (n == 0 || n >= 64) /* < sizeof(((wfe_step_result_t*)0)->content_hash) (65) */
      return 0;
   for (size_t i = 0; i < n; i++)
      if ((unsigned char)ref[i] < 0x20 || (unsigned char)ref[i] > 0x7e)
         return 0;
   return 1;
}

static wfe_step_result_t exec_pr_open(wfe_ctx *ctx, const wfe_node_t *node)
{
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   /* Safety rail: an autonomous PR may only target the configured non-protected
    * base (default testing). Refuse a misconfiguration to main/master/release* —
    * fail closed so the run stops rather than opening a PR against a protected
    * branch. */
   if (!wfe_autonomous_target_ok())
      return wfe_step_failed();
   if (g_forge->open)
   {
      const char *branch = getenv("AIMEE_WORKFLOW_BRANCH");
      char pr_ref[128] = ""; /* the forge open() contract writes up to 128 bytes */
      if (g_forge->open(wfe_ctx_repo(ctx), branch ? branch : "HEAD", wfe_ctx_work_item(ctx), "",
                        pr_ref) != 0)
         return wfe_step_looped();
      /* Fail closed on a success-with-bad-ref: advancing with no resolvable PR would
       * silently push the merge gates onto the wrong target. */
      if (!pr_ref_is_sane(pr_ref))
         return wfe_step_looped();
      /* pr_ref rides content_hash; the engine persists it as the work item's pr_ref
       * when this pr.open block advances (see wfe_engine.c ADVANCED branch). */
      return wfe_step_advanced(handle, pr_ref, 0.0);
   }
   return wfe_step_advanced(handle, "", 0.0);
}

/* The PR ref the forge ops (gate.ci / check.mergeable / merge) act on. Resolution:
 *   1. the real forge ref persisted when pr.open ran -> use it;
 *   2. else, if a PR-opening provider IS registered, a missing ref is an anomaly
 *      (a gate ran without a recorded PR) -> return "" so the forge ops fail closed
 *      (NONE/unknown -> park), rather than masking it with a wrong ref;
 *   3. else (no `open` provider: the live default + the safety-test mock) fall back
 *      to the work-item id, preserving today's behavior for the gated/legacy path. */
static const char *pr_ref(wfe_ctx *ctx)
{
   const char *ref = wfe_ctx_pr_ref(ctx);
   if (ref && ref[0])
      return ref;
   if (g_forge->open)
      return ""; /* open provider present but no ref recorded -> fail closed */
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
   /* Safety rail (mirror pr.open): never merge an autonomous run into a protected
    * branch. Fail closed on a misconfigured base. */
   if (!wfe_autonomous_target_ok())
      return wfe_step_failed();
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
