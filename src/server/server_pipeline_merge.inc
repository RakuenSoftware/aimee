/* server_pipeline_merge.inc: PR revalidation + policy-aware merge + crash-recovery
 * reconcile for the roundtable authoring pipeline gates. Included by
 * server_pipeline.c (kept as an .inc to stay under the per-file line cap). */

/* Run the policy-aware merge for an approved gate, record full evidence into the
 * gate row, and advance on success. Shared by gate-pass and the *_merge_pending
 * crash-recovery reconcile (#56). Fills `resp`. */
/* Revalidate the PR + worktree before honoring a gate verdict (proposal §5):
 * the PR still exists and is OPEN, its base is still the intended branch, its
 * head matches the approved SHA (drift #56), it is not conflicting, and the
 * worktree is clean. On any mismatch fills `resp` (merge_pending) and returns
 * -1; the verdict is preserved and the merge is not attempted. */
static int validate_pr_for_merge(rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *resp)
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
   cJSON_Delete(j);
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
static void advance_after_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no,
                                const char *merge_sha, cJSON *resp)
{
   if (merge_sha && merge_sha[0])
   {
      snprintf(gate->merge_sha, sizeof(gate->merge_sha), "%s", merge_sha);
      rtp_gate_update(gate);
   }
   const char *next = gate_no == 2 ? RTP_STATE_DONE : RTP_STATE_IMPLEMENTING;
   rtp_run_set_state(id, next, RTP_PHASE_IMPL);
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
         /* the implementation phase gets its own dedicated branch/worktree (#2). */
         if (prepare_impl_workspace(&r2) != 0)
            cJSON_AddStringToObject(
                resp, "impl_workspace_warning",
                "could not create the dedicated implementation worktree; check repo_root");
         *run = r2;
      }
   }
   cJSON_AddBoolToObject(resp, "merged", 1);
   cJSON_AddStringToObject(resp, "merge_sha", gate->merge_sha);
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

static void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
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
    * pre-check and the merge still refuses (idempotent for #55). */
   const char *admin =
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "admin")) ? " --admin" : "";
   char match[160] = {0};
   if (gate->expected_head_sha[0])
   {
      char *e = shell_escape(gate->expected_head_sha);
      snprintf(match, sizeof(match), " --match-head-commit '%s'", e ? e : gate->expected_head_sha);
      free(e);
   }
   char cmd[RTP_PATH_LEN + 256];
   size_t pfx = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + pfx, sizeof(cmd) - pfx, "gh pr merge %d --merge%s%s 2>&1", gate->pr_number, admin,
            match);
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
   snprintf(gate->merge_command, sizeof(gate->merge_command), "gh pr merge %d --merge%s%s",
            gate->pr_number, admin, match);
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
