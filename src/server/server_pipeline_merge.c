/* server_pipeline_merge.c: split from server_pipeline.c into a real translation unit
 * (was server_pipeline_merge.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_pipeline_internal.h"
#include "server_pipeline.h"
#include "cJSON.h"
#include "config.h"
#include "git_pr_api.h" /* in-process GitHub REST: the CI verdict for the merge gate */
#include "json_fluent.h"
#include "log.h"
#include "mcp_git.h"
#include "openai_runs_store.h"
#include "roundtable_pipeline.h"
#include "roundtable_pipeline_chunk.h"
#include "roundtable_pipeline_eval.h"
#include "server_http.h"
#include "agent_config.h"
#include "aimee_home.h"        /* aimee_home() for the origin working dir */
#include "delegate_ensemble.h" /* ENSEMBLE_MAX_REFS */
#include "local_operator.h"
#include "model_registry.h"
#include "platform_path.h" /* platform_mkdir_p */
#include "util.h"          /* shell_escape */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Revalidate the PR + worktree before honoring a gate verdict (proposal §5):
 * the PR still exists and is OPEN, its base is still the intended branch, its
 * head matches the approved SHA (drift #56), it is not conflicting, and the
 * worktree is clean. On any mismatch fills `resp` (merge_pending) and returns
 * -1; the verdict is preserved and the merge is not attempted. */
int validate_pr_for_merge(rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *resp)
{
   const char *mp = gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
   char cmd[RTP_PATH_LEN + 160];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + p, sizeof(cmd) - p,
            "gh pr view %d --json state,baseRefName,mergeable,headRefOid 2>/dev/null",
            gate->pr_number);
   int rc = 0;
   char *out = mcp_git_run(cmd, &rc);
   cJSON *j = (out && rc == 0 && out[0]) ? cJSON_Parse(out) : NULL;
   free(out);
   if (!j)
   {
      cJSON_AddBoolToObject(resp, "merged", 0);
      cJSON_AddStringToObject(resp, "state", mp);
      cJSON_AddStringToObject(
          resp, "note",
          "PR not found / forge unavailable / unparseable; verdict preserved, retry later.");
      return -1;
   }
   const char *state = jo_str(j, "state", "");
   const char *base = jo_str(j, "baseRefName", "");
   const char *mergeable = jo_str(j, "mergeable", "");
   const char *head = jo_str(j, "headRefOid", "");
   const char *why = NULL;
   if (state[0] && strcmp(state, "OPEN") != 0)
      why = "PR is no longer OPEN (closed or merged elsewhere)";
   else if (base[0] && run->base_branch[0] && strcmp(base, run->base_branch) != 0)
      why = "PR base branch changed from the approved target";
   else if (gate->expected_head_sha[0] && head[0] && strcmp(head, gate->expected_head_sha) != 0)
   {
      cJSON_AddBoolToObject(resp, "head_drift", 1);
      cJSON_AddStringToObject(resp, "current_head_sha", head);
      cJSON_AddStringToObject(resp, "expected_head_sha", gate->expected_head_sha);
      why = "PR head drifted from the approved SHA; the gate verdict is stale";
   }
   else if (mergeable[0] && strcmp(mergeable, "CONFLICTING") == 0)
      why = "PR is not mergeable (conflicting)";
   else if (mergeable[0] && strcmp(mergeable, "MERGEABLE") != 0)
      /* Anything that is neither MERGEABLE nor CONFLICTING (notably UNKNOWN,
       * which GitHub returns while it is still computing mergeability) is not
       * "assume green" (§5): park in *_merge_pending and re-check, rather than
       * attempting a merge against an unknown state. */
      why = "PR mergeability not yet known (UNKNOWN); verdict preserved, re-check shortly";
   cJSON_Delete(j);

   /* CI must be fully green before a merge (operator ruling 2026-07-15). Read the
    * verdict in-process from the Checks API rather than shelling `gh pr checks`, so
    * the forge token stays in aimee-server's memory. A PR with zero reported checks
    * merges (nothing to fail); PENDING and an undetermined verdict both park —
    * "unknown" is never "pass", consistent with the UNKNOWN-mergeability rule above. */
   if (!why)
   {
      char cierr[160];
      switch (git_pr_ci_via_api(NULL, rtp_git_cwd(run), gate->pr_number, cierr, sizeof(cierr)))
      {
      case GIT_PR_CI_SUCCESS:
      case GIT_PR_CI_NONE:
         break;
      case GIT_PR_CI_PENDING:
         why = "CI has not finished; verdict preserved, re-check once checks settle";
         break;
      case GIT_PR_CI_FAILURE:
         why = "CI is not green; a merge requires fully green CI";
         break;
      default: /* GIT_PR_CI_ERROR */
         why = "CI status could not be determined; verdict preserved, retry later";
         break;
      }
   }

   if (why)
   {
      cJSON_AddBoolToObject(resp, "merged", 0);
      cJSON_AddStringToObject(resp, "state", mp);
      cJSON_AddStringToObject(resp, "note", why);
      return -1;
   }
   /* explain base movement since open (#3): the base SHA recorded at PR-open vs
    * the current base SHA — surfaced (the base advancing as other PRs merge is
    * normal; a changed base BRANCH was already hard-rejected above). */
   if (run->base_sha[0] && run->base_branch[0])
   {
      char curbase[RTP_HASH_LEN] = {0};
      if (git_rev_parse(run, run->base_branch, curbase, sizeof(curbase)) != 0 || !curbase[0])
      {
         char originref[RTP_NAME_LEN + 8];
         snprintf(originref, sizeof(originref), "origin/%s", run->base_branch);
         git_rev_parse(run, originref, curbase, sizeof(curbase));
      }
      if (curbase[0] && strcmp(curbase, run->base_sha) != 0)
      {
         cJSON_AddBoolToObject(resp, "base_moved", 1);
         cJSON_AddStringToObject(resp, "base_sha_at_open", run->base_sha);
         cJSON_AddStringToObject(resp, "base_sha_now", curbase);
      }
   }
   /* worktree cleanliness (§5): a dirty dedicated worktree blocks the merge — the
    * verdict is preserved in *_merge_pending until the tree is clean. */
   if (rtp_git_cwd(run)[0])
   {
      char wc[RTP_PATH_LEN + 64];
      size_t wp = rtp_cd_prefix(run, wc, sizeof(wc));
      snprintf(wc + wp, sizeof(wc) - wp, "git status --porcelain 2>/dev/null");
      int wrc = 0;
      char *wo = mcp_git_run(wc, &wrc);
      int dirty = (wo && wo[0]) ? 1 : 0;
      free(wo);
      if (dirty)
      {
         cJSON_AddBoolToObject(resp, "merged", 0);
         cJSON_AddBoolToObject(resp, "worktree_dirty", 1);
         cJSON_AddStringToObject(resp, "state", mp);
         cJSON_AddStringToObject(resp, "note",
                                 "the dedicated worktree is dirty; commit/clean it before merging "
                                 "— verdict preserved.");
         return -1;
      }
   }
   return 0;
}

/* Record the merge SHA + advance after a successful merge: gate1 -> implementing
 * with a FRESH dedicated implementation branch/worktree (#2), gate2 -> done,
 * reclaiming the admission slot. */
void advance_after_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no,
                         const char *merge_sha, cJSON *resp)
{
   /* Never advance without a durable merge SHA (§ recovery): the merge landed but
    * if its commit SHA wasn't recovered (gh lookup raced/failed), stay in
    * *_merge_pending so the next advance reconciles (recovers mergeCommit.oid)
    * and advances then. */
   if (!merge_sha || !merge_sha[0])
   {
      const char *mp = gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
      rtp_run_set_state(id, mp, NULL);
      cJSON_AddBoolToObject(resp, "merged", 1);
      cJSON_AddBoolToObject(resp, "advanced", 0);
      cJSON_AddStringToObject(resp, "state", mp);
      cJSON_AddStringToObject(
          resp, "note",
          "merge landed but its SHA was not recovered; not advancing without a durable merge SHA — "
          "call pipeline.advance to reconcile.");
      return;
   }
   snprintf(gate->merge_sha, sizeof(gate->merge_sha), "%s", merge_sha);
   rtp_gate_update(gate);
   const char *next = gate_no == 2 ? RTP_STATE_DONE : RTP_STATE_IMPLEMENTING;
   rtp_run_set_state(id, next, RTP_PHASE_IMPL);
   /* the merge itself succeeded and is recorded — always report that truthfully. */
   cJSON_AddBoolToObject(resp, "merged", 1);
   cJSON_AddBoolToObject(resp, "advanced", 1);
   cJSON_AddStringToObject(resp, "merge_sha", gate->merge_sha);
   if (gate_no == 1)
   {
      rtp_run_t r2;
      if (rtp_run_get(id, &r2) == 0)
      {
         if (strcmp(r2.admission_class, RTP_ADMIT_ACTIVE) != 0)
         {
            snprintf(r2.admission_class, sizeof(r2.admission_class), RTP_ADMIT_ACTIVE);
            rtp_run_update(&r2);
         }
         /* the implementation phase gets its own dedicated branch/worktree off the
          * merge commit (#2). If that fails we must NOT silently continue with the
          * proposal branch: clear the proposal head/worktree so nothing reuses
          * them, and escalate — the merge stands but the impl workspace needs
          * operator attention. */
         if (prepare_impl_workspace(&r2, gate->merge_sha) != 0)
         {
            r2.head_branch[0] = '\0';
            r2.worktree_path[0] = '\0';
            r2.head_sha[0] = '\0';
            r2.base_sha[0] = '\0';
            r2.impl_pr_number = 0;
            r2.impl_pr_url[0] = '\0';
            rtp_run_update(&r2);
            *run = r2;
            cJSON_AddStringToObject(resp, "action", "escalate");
            cJSON_AddStringToObject(resp, "state", RTP_STATE_IMPLEMENTING);
            cJSON_AddBoolToObject(resp, "impl_workspace_error", 1);
            cJSON_AddStringToObject(
                resp, "note",
                "gate-1 merge landed, but the dedicated implementation worktree could not be "
                "created off the merge commit; the proposal branch was NOT reused — set up the "
                "impl "
                "workspace (repo_root/remote) and resume.");
            return;
         }
         *run = r2;
      }
   }
   cJSON_AddStringToObject(resp, "state", next);
}

/* Crash-recovery reconcile (#56): if the PR already merged (e.g. a crash after
 * the remote merge but before merge_sha/advance was recorded), record the merge
 * SHA + advance instead of treating the non-OPEN PR as an error. Returns 1 if
 * handled (merged-and-advanced, or merged-at-a-different-head -> stale stop); 0
 * if the PR is not merged and the caller should proceed to validate + merge. */
static int reconcile_if_merged(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *resp)
{
   char cmd[RTP_PATH_LEN + 128];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + p, sizeof(cmd) - p,
            "gh pr view %d --json state,mergeCommit,headRefOid 2>/dev/null", gate->pr_number);
   int rc = 0;
   char *out = mcp_git_run(cmd, &rc);
   cJSON *j = (out && rc == 0 && out[0]) ? cJSON_Parse(out) : NULL;
   free(out);
   if (!j)
      return 0; /* unknown -> let validate_pr_for_merge handle it */
   int is_merged = strcmp(jo_str(j, "state", ""), "MERGED") == 0;
   char head[RTP_HASH_LEN];
   snprintf(head, sizeof(head), "%s", jo_str(j, "headRefOid", ""));
   cJSON *mc = cJSON_GetObjectItemCaseSensitive(j, "mergeCommit");
   char msha[RTP_HASH_LEN];
   snprintf(msha, sizeof(msha), "%s", (mc && cJSON_IsObject(mc)) ? jo_str(mc, "oid", "") : "");
   cJSON_Delete(j);
   if (!is_merged)
      return 0;
   /* merged at a DIFFERENT head than approved -> stale, stop for operator (#56). */
   if (gate->expected_head_sha[0] && head[0] && strcmp(head, gate->expected_head_sha) != 0)
   {
      const char *mp = gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
      cJSON_AddBoolToObject(resp, "merged", 0);
      cJSON_AddBoolToObject(resp, "head_drift", 1);
      cJSON_AddStringToObject(resp, "current_head_sha", head);
      cJSON_AddStringToObject(resp, "expected_head_sha", gate->expected_head_sha);
      cJSON_AddStringToObject(resp, "state", mp);
      cJSON_AddStringToObject(
          resp, "note",
          "PR already merged at a DIFFERENT head than approved; evidence stale, "
          "stopping for operator review");
      return 1;
   }
   /* already merged at the approved head -> record + advance exactly once. */
   snprintf(gate->merge_executor, sizeof(gate->merge_executor), "gh pr merge");
   snprintf(gate->merge_command, sizeof(gate->merge_command), "gh pr merge %d (already merged)",
            gate->pr_number);
   gate->merge_exit_code = 0;
   advance_after_merge(id, run, gate, gate_no, msha, resp);
   cJSON_AddBoolToObject(resp, "reconciled", 1);
   return 1;
}

void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
                        cJSON *resp)
{
   /* Crash recovery (#56): an already-merged PR is reconciled (merge_sha
    * recorded + advanced), not rejected by the OPEN-state check below. */
   if (reconcile_if_merged(id, run, gate, gate_no, resp))
      return;

   /* Revalidate the PR + worktree before merging (§5): existence, OPEN state,
    * base branch, head drift (#56), mergeability, worktree cleanliness. On any
    * mismatch the verdict is preserved in *_merge_pending and we do not merge. */
   if (validate_pr_for_merge(run, gate, gate_no, resp) != 0)
      return;

   /* Policy-aware merge executor (#50), pinned to the recorded checkout (#3) and
    * keyed by the approved head SHA (--match-head-commit) so a drift between the
    * pre-check and the merge still refuses (idempotent for #55).
    *
    * No admin/bypass path exists: a merge that requires an admin override of
    * branch protection is HUMAN-ONLY (operator ruling 2026-07-15). When
    * protection refuses, the merge fails and the verdict is preserved in
    * *_merge_pending for a human — never forced through. */
   char match[160] = {0};
   if (gate->expected_head_sha[0])
   {
      char *e = shell_escape(gate->expected_head_sha);
      snprintf(match, sizeof(match), " --match-head-commit '%s'", e ? e : gate->expected_head_sha);
      free(e);
   }
   char cmd[RTP_PATH_LEN + 256];
   size_t pfx = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + pfx, sizeof(cmd) - pfx, "gh pr merge %d --merge%s 2>&1", gate->pr_number, match);
   int exit_code = 0;
   char *out = mcp_git_run(cmd, &exit_code);
   int merged = (exit_code == 0);
   char merge_sha[RTP_HASH_LEN] = {0};
   if (out)
      snprintf(gate->merge_output, sizeof(gate->merge_output), "%s", out);
   free(out);
   if (merged)
   {
      /* recover the merge commit SHA from the recorded checkout. */
      char vcmd[RTP_PATH_LEN + 96];
      size_t vp = rtp_cd_prefix(run, vcmd, sizeof(vcmd));
      snprintf(vcmd + vp, sizeof(vcmd) - vp,
               "gh pr view %d --json mergeCommit -q .mergeCommit.oid 2>/dev/null", gate->pr_number);
      int vrc = 0;
      char *vout = mcp_git_run(vcmd, &vrc);
      if (vout)
      {
         char *nl = strchr(vout, '\n');
         if (nl)
            *nl = '\0';
         if (vrc == 0)
            snprintf(merge_sha, sizeof(merge_sha), "%s", vout);
         free(vout);
      }
   }
   snprintf(gate->merge_executor, sizeof(gate->merge_executor), "gh pr merge");
   snprintf(gate->merge_command, sizeof(gate->merge_command), "gh pr merge %d --merge%s",
            gate->pr_number, match);
   gate->merge_exit_code = exit_code;
   rtp_gate_update(gate);

   if (merged)
   {
      /* record merge SHA + advance (gate1 -> implementing with a fresh dedicated
       * impl branch/worktree #2; gate2 -> done), reclaiming the slot (#47/#48). */
      advance_after_merge(id, run, gate, gate_no, merge_sha, resp);
   }
   else
   {
      const char *mp = gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
      cJSON_AddBoolToObject(resp, "merged", 0);
      cJSON_AddStringToObject(resp, "state", mp);
      cJSON_AddStringToObject(
          resp, "note",
          "merge did not land (auth / protected branch / drift / hung checks). Verdict preserved; "
          "call pipeline.advance to reconcile/retry, or pipeline.cancel.");
   }
   (void)run;
}
