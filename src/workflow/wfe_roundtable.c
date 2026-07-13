/* wfe_roundtable.c: the gate.roundtable executor. Reads the panel spec from the
 * node params, runs the (pluggable) panel provider, scores the verdicts with
 * the fail-closed rule, and maps the decision to a wfe_step_result. */
#include "wfe_roundtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "util.h" /* safe_exec_capture — compute the change-under-review diff */
#include "wfe_store.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

#define WFE_PANEL_MAX 16
/* Cap the diff pushed to the panel. A change larger than this is truncated (the
 * panel still sees the head of the diff + can navigate the rest via the index). */
#define WFE_REVIEW_DIFF_MAX 200000

/* Default (live) provider: calls the ensemble engine. Gated on
 * roundtable-panel-composition (§0) — until that ships the engine passes no
 * per-participant persona and codex cannot review, so we cannot compose a
 * diverse panel and MUST fail closed (park) rather than approve. */
static int live_panel_run(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                          const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)pkt;
   (void)required;
   (void)nreq;
   (void)eligible;
   (void)nelig;
   (void)out;
   (void)max;
   return -1; /* §0: panel not composable yet -> DEGRADED (fail closed) */
}

static wfe_panel_run_fn g_provider = live_panel_run;

void wfe_set_panel_provider(wfe_panel_run_fn fn)
{
   g_provider = fn ? fn : live_panel_run;
}

/* Pull a string array out of params.panel.<key> into a fixed buffer set. */
static int names_from(const cJSON *panel, const char *key, char buf[][32], int cap)
{
   const cJSON *arr = panel ? cJSON_GetObjectItemCaseSensitive(panel, key) : NULL;
   int n = 0;
   if (arr && cJSON_IsArray(arr))
   {
      const cJSON *it = NULL;
      cJSON_ArrayForEach(it, arr)
      {
         if (n >= cap)
            break;
         if (cJSON_IsString(it))
            snprintf(buf[n++], 32, "%s", it->valuestring);
      }
   }
   return n;
}

/* Read up to `cap-1` bytes of a text file into a NUL-terminated malloc'd buffer.
 * Returns NULL if `path` is empty/unreadable. Bounded so a pathological proposal
 * file cannot balloon the review packet; truncation is silent (the panel sees the
 * leading, most-relevant portion). Caller frees. */
#define WFE_PROPOSAL_MAX 262144 /* 256 KiB cap on proposal text handed to the panel */
static char *read_text_capped(const char *path, size_t cap)
{
   if (!path || !path[0])
      return NULL;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   char *buf = malloc(cap);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, cap - 1, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

static wfe_step_result_t exec_roundtable(wfe_ctx *ctx, const wfe_node_t *node)
{
   const cJSON *panel =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "panel") : NULL;
   char reqbuf[WFE_PANEL_MAX][32], eligbuf[WFE_PANEL_MAX][32];
   int nreq = names_from(panel, "required", reqbuf, WFE_PANEL_MAX);
   int nelig = names_from(panel, "eligible", eligbuf, WFE_PANEL_MAX);
   const char *req[WFE_PANEL_MAX], *elig[WFE_PANEL_MAX];
   for (int i = 0; i < nreq; i++)
      req[i] = reqbuf[i];
   for (int i = 0; i < nelig; i++)
      elig[i] = eligbuf[i];

   int quorum_req = 0;
   const cJSON *q = node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "quorum") : NULL;
   if (q && cJSON_IsNumber(q))
      quorum_req = q->valueint;
   int quorum = wfe_gate_effective_quorum(quorum_req, nreq);

   /* artifact under review = the work item's current content hash */
   char artifact_hash[72] = "";
   const char *wi = wfe_ctx_work_item(ctx);
   db1_work_item_t row;
   if (wi && db1_work_item_get(wi, &row) == 1)
      snprintf(artifact_hash, sizeof artifact_hash, "%s", row.content_hash);

   /* Compose the review packet: the panel sees the artifact together with the
    * originating proposal/request (so it validates the plan/diff AGAINST the ask,
    * not in isolation) and the optional review lens from params.focus. */
   char *proposal = read_text_capped(wfe_ctx_proposal_path(ctx), WFE_PROPOSAL_MAX);
   const cJSON *focus_j =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "focus") : NULL;
   /* Optional: this gate may convene a NAMED roundtable preset (its seat
    * persona->model bindings) instead of the configured default. */
   const cJSON *rt_j =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "roundtable") : NULL;
   /* The directory the panel reviews the change in: the per-work-item worktree, or —
    * when worktree isolation is unavailable and a run fell back to the shared checkout
    * (wfe_worktree_ensure failed) — that shared repo dir. Mirrors the producing blocks'
    * resolve_workdir fallback (the work item's own repo, else AIMEE_WORKFLOW_REPO, else
    * "."), so the panel convenes in the run's repository rather than fail closed to
    * panel_unreachable when the worktree is empty. */
   const char *worktree = wfe_ctx_worktree(ctx);
   const char *workdir = (worktree && worktree[0]) ? worktree : wfe_repo_local(wfe_ctx_repo(ctx));

   /* Push the change under review to the panel: the branch diff vs the autonomous
    * base ("what changed from the default repo"). Computed once here so panelists
    * read the delta directly and lean on the aimee index for surrounding context,
    * instead of each re-running git and sweeping the worktree. Empty at a plan gate
    * (HEAD == base -> no diff) or when the base ref is absent; capped for size. */
   char *diff = NULL;
   {
      char range[128];
      snprintf(range, sizeof range, "%s...HEAD", wfe_autonomous_base());
      const char *dargv[] = {"git", "-C", workdir, "--no-pager", "diff", range, NULL};
      (void)safe_exec_capture(dargv, &diff, WFE_REVIEW_DIFF_MAX);
   }

   wfe_review_packet_t pkt = {
       .artifact_hash = artifact_hash,
       .proposal = proposal ? proposal : "",
       .focus =
           (focus_j && cJSON_IsString(focus_j) && focus_j->valuestring) ? focus_j->valuestring : "",
       .workdir = workdir,
       .diff = diff ? diff : "",
       .roundtable = (rt_j && cJSON_IsString(rt_j) && rt_j->valuestring) ? rt_j->valuestring : "",
   };

   wfe_verdict_t verdicts[WFE_PANEL_MAX];
   int nv = g_provider(&pkt, req, nreq, elig, nelig, verdicts, WFE_PANEL_MAX);
   free(proposal);
   free(diff);
   if (nv == WFE_PANEL_PINNED_FAIL)
      /* A user-pinned seat model could not be fulfilled. A pinned model is a hard
       * requirement — never silently swapped — so the run FAILS terminally rather
       * than degrading to a different panel. */
      return wfe_step_failed_class(WFE_FAIL_PERMANENT, 0);
   if (nv < 0)
      return wfe_step_pending(WFE_PAUSE_PANEL_UNREACHABLE);

   char reason[160];
   wfe_gate_decision_t d =
       wfe_gate_decide(verdicts, nv, req, nreq, quorum, artifact_hash, reason, sizeof reason);
   switch (d)
   {
   case WFE_GATE_APPROVE:
   {
      /* Passed: drop any stale review feedback so it can't bleed into a later
       * author step for this work item. */
      wfe_feedback_clear(wi);
      char handle[80];
      snprintf(handle, sizeof handle, "%s.out", node->id);
      return wfe_step_advanced(handle, artifact_hash, 0.0);
   }
   case WFE_GATE_CHANGES:
   {
      /* Persist the panel's blockers so the re-authoring delegate (via on_fail)
       * refines against the actual objections instead of re-authoring blind. */
      const cJSON *fj =
          node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "focus") : NULL;
      const char *focus = (fj && cJSON_IsString(fj)) ? fj->valuestring : "";
      char agg[4096];
      int off = snprintf(agg, sizeof agg,
                         "The review roundtable requested changes%s%s. Revise to resolve the "
                         "reviewers' blockers below, then commit:\n",
                         focus[0] ? " — focus: " : "", focus);
      for (int i = 0; i < nv && off > 0 && (size_t)off < sizeof agg; i++)
         if ((verdicts[i].kind == WFE_V_REQUEST_CHANGES || verdicts[i].kind == WFE_V_MALFORMED) &&
             verdicts[i].feedback[0])
            off += snprintf(agg + off, sizeof agg - (size_t)off, "\n## %s\n%s\n",
                            verdicts[i].persona[0] ? verdicts[i].persona : "reviewer",
                            verdicts[i].feedback);
      wfe_feedback_write(wi, agg);
      return wfe_step_looped();
   }
   case WFE_GATE_DEGRADED:
   default:
      return wfe_step_pending(WFE_PAUSE_PANEL_DEGRADED);
   }
}

void wfe_register_roundtable_gate(void)
{
   wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE, exec_roundtable);
}
