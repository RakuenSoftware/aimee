/* server_pipeline.c: the `pipeline.*` control surface for the roundtable
 * authoring pipeline. Drives the section-1 state machine over the durable DB1
 * ledger, enforces the two human gates with operator authority (#53), and runs
 * the policy-aware merge executor (#50/#56). The outer-loop decisions come from
 * roundtable_pipeline_eval; result capture is server-worker-owned
 * (roundtable_pipeline_capture). See
 * docs/proposals/accepted/agent-roundtable-authoring-pipeline.md. */

#include "server_pipeline.h"

#include "cJSON.h"
#include "config.h"
#include "json_fluent.h"
#include "log.h"
#include "mcp_git.h"
#include "openai_runs_store.h"
#include "roundtable_pipeline.h"
#include "roundtable_pipeline_eval.h"
#include "server_http.h"

#include "local_operator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers -- */

static const char *phase_for_state(const char *state)
{
   if (strncmp(state, "gate2", 5) == 0 || strcmp(state, RTP_STATE_IMPLEMENTING) == 0 ||
       strcmp(state, RTP_STATE_PR_REVIEW) == 0)
      return RTP_PHASE_IMPL;
   return RTP_PHASE_PROPOSAL;
}

static const char *review_state_for_phase(const char *phase)
{
   return strcmp(phase, RTP_PHASE_IMPL) == 0 ? RTP_STATE_PR_REVIEW : RTP_STATE_PROPOSAL_REVIEW;
}

static int is_terminal_state(const char *s)
{
   return strcmp(s, RTP_STATE_DONE) == 0 || strcmp(s, RTP_STATE_FAILED) == 0 ||
          strcmp(s, RTP_STATE_ABANDONED) == 0;
}

/* Rebuild the captured terminal envelope from the persisted pass aggregate plus
 * the current attempt's flags, so the loop decision uses the same predicate as
 * the worker capture path (#41). */
static void env_from_ledger(const rtp_pass_t *p, const rtp_attempt_t *a, rtp_envelope_t *e)
{
   memset(e, 0, sizeof(*e));
   e->is_draft = strcmp(p->mode, RTP_MODE_DRAFT) == 0;
   if (a)
   {
      e->present = (a->lost_result || strcmp(a->capture_status, RTP_CAP_PENDING) == 0) ? 0 : 1;
      e->parse_ok = strcmp(a->parse_status, "malformed") == 0 ? 0 : 1;
      e->lost_result = a->lost_result;
      e->truncated = a->truncated;
      e->items_truncated = a->items_truncated;
      e->degraded = a->degraded;
      e->cost_capped = a->cost_capped;
      e->deadline_hit = a->deadline_hit;
      e->cancelled = a->cancelled;
      e->has_error = (strcmp(a->capture_status, RTP_CAP_FAILED) == 0 && !a->truncated &&
                      !a->degraded)
                         ? 1
                         : 0;
      e->cost_usd = a->cost_usd;
      e->cost_known = a->cost_known;
   }
   else
   {
      e->present = 0;
   }
   e->converged = p->converged;
   e->blocking_count = p->blocking_count;
   e->suggestion_count = p->suggestion_count;
   e->nit_count = p->nit_count;
   e->coverage_gap_count = p->coverage_gaps;
   e->answered_count = 0;
   e->items_round = p->items_round;
   e->artifact_round = p->artifact_round;
   e->best_round = p->best_round;
   e->rounds_run = p->rounds_run;
   e->artifact_present = e->is_draft ? 1 : 0; /* aggregate doesn't store artifact text */
}

static void load_loop_cfg(const config_t *cfg, const rtp_run_t *run, rtp_loop_cfg_t *out)
{
   out->done_bar = run->done_bar[0] ? run->done_bar : cfg->roundtable_pipeline_done_bar;
   out->max_passes = cfg->roundtable_pipeline_max_passes;
   out->max_attempts_per_pass = cfg->roundtable_pipeline_max_attempts_per_pass > 0
                                    ? cfg->roundtable_pipeline_max_attempts_per_pass
                                    : 2;
   out->max_phase_cost_usd = cfg->roundtable_pipeline_max_cost_usd;
}

static double phase_cost(const rtp_run_t *run, const char *phase)
{
   return strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->impl_phase_cost_usd
                                             : run->proposal_phase_cost_usd;
}

/* Compact converged-review digest for the human gate (section 5). */
static cJSON *build_digest(const rtp_run_t *run, const rtp_pass_t *latest, int have_latest)
{
   cJSON *d = cJSON_CreateObject();
   cJSON_AddNumberToObject(d, "pipeline_id", run->id);
   cJSON_AddStringToObject(d, "state", run->state);
   cJSON_AddStringToObject(d, "phase", run->phase);
   const char *phase = phase_for_state(run->state);
   int pr = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->impl_pr_number : run->proposal_pr_number;
   const char *url = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->impl_pr_url : run->proposal_pr_url;
   cJSON_AddNumberToObject(d, "pr_number", pr);
   cJSON_AddStringToObject(d, "pr_url", url);
   if (have_latest)
   {
      cJSON *rv = cJSON_AddObjectToObject(d, "review");
      cJSON_AddNumberToObject(rv, "pass_no", latest->pass_no);
      cJSON_AddBoolToObject(rv, "converged", latest->converged ? 1 : 0);
      cJSON_AddBoolToObject(rv, "envelope_valid", latest->envelope_valid ? 1 : 0);
      cJSON_AddNumberToObject(rv, "blocking", latest->blocking_count);
      cJSON_AddNumberToObject(rv, "suggestions", latest->suggestion_count);
      cJSON_AddNumberToObject(rv, "nits", latest->nit_count);
      cJSON_AddNumberToObject(rv, "coverage_gaps", latest->coverage_gaps);
      cJSON_AddNumberToObject(rv, "best_round", latest->best_round);
   }
   cJSON *ec = cJSON_AddObjectToObject(d, "economics");
   cJSON_AddNumberToObject(ec, "proposal_phase_cost_usd", run->proposal_phase_cost_usd);
   cJSON_AddNumberToObject(ec, "impl_phase_cost_usd", run->impl_phase_cost_usd);
   cJSON_AddNumberToObject(ec, "total_cost_usd", run->total_cost_usd);
   cJSON_AddStringToObject(ec, "cost_scope", run->cost_scope);
   cJSON_AddStringToObject(ec, "cost_source", run->cost_source);
   return d;
}

/* forward decl: the shared merge executor (defined in the gate section). */
static void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
                               cJSON *resp);

/* ------------------------------------------------------------- handlers ---- */

int handle_pipeline_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *idea = jo_str(req, "idea", NULL);
   if (!idea || !idea[0])
      idea = jo_str(req, "task", NULL);
   if (!idea || !idea[0])
      return server_send_error(conn, "usage: aimee pipeline start <idea>", NULL);

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "pipeline: could not load configuration", NULL);

   /* v1 admission control: at most one active pipeline (section 1). Parked gates
    * may release the slot per roundtable.pipeline_parked_releases_slot. */
   int active = rtp_run_count_active();
   if (active > 0)
      return server_send_error(
          conn, "pipeline: another pipeline is already active (one active run at a time)", NULL);

   const char *done_bar = jo_str(req, "done_bar", cfg.roundtable_pipeline_done_bar);
   if (strcmp(done_bar, RTP_DONEBAR_ZERO_BLOCKING) != 0 &&
       strcmp(done_bar, RTP_DONEBAR_ZERO_BLOCKING_SUGGESTIONS) != 0 &&
       strcmp(done_bar, RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS) != 0)
      return server_send_error(conn, "pipeline: invalid done_bar", NULL);

   const char *repo_root = jo_str(req, "repo_root", "");
   const char *base = jo_str(req, "base_branch", "testing");

   int id = 0;
   if (rtp_run_create(idea, done_bar, repo_root, base, &id) != 0 || id <= 0)
      return server_send_error(conn, "pipeline: could not create pipeline", NULL);

   /* seed the brief from the idea + any seed questions. */
   rtp_run_t run;
   if (rtp_run_get(id, &run) == 0)
   {
      snprintf(run.brief, sizeof(run.brief), "goal: %s", idea);
      const char *seed = jo_str(req, "brief", NULL);
      if (seed && seed[0])
         snprintf(run.brief, sizeof(run.brief), "%s", seed);
      rtp_run_update(&run);
   }

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", id);
   cJSON_AddStringToObject(resp, "state", RTP_STATE_DRAFTING);
   cJSON_AddStringToObject(resp, "done_bar", done_bar);
   return server_send_ok(conn, resp);
}

int handle_pipeline_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   if (id <= 0)
      return server_send_error(conn, "usage: aimee pipeline status <id>", NULL);

   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);

   const char *phase = phase_for_state(run.state);
   rtp_pass_t latest;
   int have = rtp_pass_latest(id, phase, &latest) == 0;

   cJSON *resp = jo_ok();
   cJSON *dig = build_digest(&run, &latest, have);
   cJSON_AddItemToObject(resp, "pipeline", dig);
   cJSON_AddStringToObject(resp, "admission_class", run.admission_class);
   cJSON_AddStringToObject(resp, "brief", run.brief);
   if (have)
   {
      rtp_attempt_t a;
      int hav_a = rtp_attempt_current(latest.id, &a) == 0;
      cJSON *p = cJSON_AddObjectToObject(resp, "latest_pass");
      cJSON_AddStringToObject(p, "status", latest.status);
      cJSON_AddStringToObject(p, "mode", latest.mode);
      cJSON_AddNumberToObject(p, "pass_no", latest.pass_no);
      if (hav_a)
      {
         cJSON_AddStringToObject(p, "capture_status", a.capture_status);
         cJSON_AddNumberToObject(p, "attempt_no", a.attempt_no);
         cJSON_AddStringToObject(p, "run_id", a.run_id);
      }
   }
   /* surface any open gate's recorded verdict / merge intent for recovery. */
   if (strncmp(run.state, "gate", 4) == 0)
   {
      int gate_no = (run.state[4] == '2') ? 2 : 1;
      rtp_gate_t g;
      if (rtp_gate_get(id, gate_no, &g) == 0)
      {
         cJSON *gj = cJSON_AddObjectToObject(resp, "gate");
         cJSON_AddNumberToObject(gj, "gate_no", g.gate_no);
         cJSON_AddStringToObject(gj, "verdict", g.verdict);
         cJSON_AddNumberToObject(gj, "pr_number", g.pr_number);
         cJSON_AddStringToObject(gj, "expected_head_sha", g.expected_head_sha);
         cJSON_AddStringToObject(gj, "merge_sha", g.merge_sha);
      }
   }
   return server_send_ok(conn, resp);
}

int handle_pipeline_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *filter = jo_str(req, "state", NULL);
   rtp_run_t rows[64];
   int n = rtp_run_list(filter, rows, 64);
   if (n < 0)
      n = 0;
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "pipelines");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "pipeline_id", rows[i].id);
      cJSON_AddStringToObject(o, "state", rows[i].state);
      cJSON_AddStringToObject(o, "phase", rows[i].phase);
      cJSON_AddStringToObject(o, "admission_class", rows[i].admission_class);
      cJSON_AddStringToObject(o, "idea", rows[i].idea);
      cJSON_AddNumberToObject(o, "total_cost_usd", rows[i].total_cost_usd);
      cJSON_AddItemToArray(arr, o);
   }
   return server_send_ok(conn, resp);
}

/* request child-run cancellation for any in-flight attempt (#31). */
static void stop_inflight(int pipeline_id, const char *phase)
{
   rtp_pass_t p;
   if (rtp_pass_latest(pipeline_id, phase, &p) != 0)
      return;
   rtp_attempt_t a;
   if (rtp_attempt_current(p.id, &a) != 0)
      return;
   if (strcmp(a.capture_status, RTP_CAP_PENDING) == 0 && a.run_id[0])
      openai_runs_store_request_cancel(a.run_id);
}

int handle_pipeline_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   if (id <= 0)
      return server_send_error(conn, "usage: aimee pipeline cancel <id>", NULL);
   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);
   if (is_terminal_state(run.state))
      return server_send_error(conn, "pipeline: already terminal", NULL);

   stop_inflight(id, RTP_PHASE_PROPOSAL);
   stop_inflight(id, RTP_PHASE_IMPL);
   rtp_run_set_state(id, RTP_STATE_ABANDONED, NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", id);
   cJSON_AddStringToObject(resp, "state", RTP_STATE_ABANDONED);
   return server_send_ok(conn, resp);
}

int handle_pipeline_resume(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   if (id <= 0)
      return server_send_error(conn, "usage: aimee pipeline resume <id>", NULL);
   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);
   if (is_terminal_state(run.state))
      return server_send_error(conn, "pipeline: terminal, cannot resume", NULL);

   /* a parked run can re-claim the active slot only if none is taken. */
   if (strcmp(run.admission_class, RTP_ADMIT_ACTIVE) != 0)
   {
      if (rtp_run_count_active() > 0)
         return server_send_error(conn, "pipeline: another pipeline holds the active slot", NULL);
      snprintf(run.admission_class, sizeof(run.admission_class), RTP_ADMIT_ACTIVE);
      rtp_run_update(&run);
   }
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", id);
   cJSON_AddStringToObject(resp, "state", run.state);
   cJSON_AddStringToObject(resp, "next",
                           "call pipeline.advance to continue the loop or resolve the gate");
   return server_send_ok(conn, resp);
}

/* Submit a roundtable pass (draft or review) as a pipeline-owned op-run. The
 * worker captures the terminal envelope into the ledger (#18). */
static int submit_pass(server_conn_t *conn, rtp_run_t *run, const char *phase, const char *mode,
                       const char *artifact, const char *artifact_hash)
{
   int pass_no = rtp_pass_max_no(run->id, phase) + 1;
   int pass_id = 0;
   if (rtp_pass_create(run->id, phase, mode, pass_no, artifact_hash, &pass_id) != 0)
      return server_send_error(conn, "pipeline: could not create pass", NULL);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "task", artifact ? artifact : "");
   cJSON_AddStringToObject(body, "mode", strcmp(mode, RTP_MODE_DRAFT) == 0 ? "draft" : "review");
   if (run->brief[0])
      cJSON_AddStringToObject(body, "brief", run->brief);
   cJSON_AddNumberToObject(body, "pipeline_pass_id", pass_id);
   char *bj = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   char runresp[4096];
   int rc = server_http_submit_op_run("delegate.roundtable", bj ? bj : "{}", conn->capabilities,
                                      runresp, (int)sizeof(runresp));
   free(bj);
   if (rc < 200 || rc >= 300)
   {
      /* mark the pass failed so the loop doesn't wait forever on a non-submit. */
      rtp_pass_t p;
      if (rtp_pass_get(pass_id, &p) == 0)
      {
         snprintf(p.status, sizeof(p.status), RTP_PASS_FAILED);
         rtp_pass_update(&p);
      }
      return server_send_error(conn, "pipeline: roundtable submission failed (ensemble enabled?)",
                               NULL);
   }

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", run->id);
   cJSON_AddStringToObject(resp, "action", "submitted");
   cJSON_AddStringToObject(resp, "phase", phase);
   cJSON_AddStringToObject(resp, "mode", mode);
   cJSON_AddNumberToObject(resp, "pass_id", pass_id);
   cJSON *runobj = cJSON_Parse(runresp);
   if (runobj)
      cJSON_AddItemToObject(resp, "run", runobj);
   return server_send_ok(conn, resp);
}

int handle_pipeline_advance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   if (id <= 0)
      return server_send_error(conn, "usage: aimee pipeline advance <id> [--artifact <text>]", NULL);
   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);
   if (is_terminal_state(run.state))
      return server_send_error(conn, "pipeline: terminal", NULL);

   /* *_merge_pending is a post-pass recovery state (#56): reconcile the recorded
    * merge intent rather than re-asking the human. The verdict is preserved; the
    * TTL never abandons it (#57). */
   if (strcmp(run.state, RTP_STATE_GATE1_MERGE_PENDING) == 0 ||
       strcmp(run.state, RTP_STATE_GATE2_MERGE_PENDING) == 0)
   {
      int gate_no = strcmp(run.state, RTP_STATE_GATE2_MERGE_PENDING) == 0 ? 2 : 1;
      rtp_gate_t gate;
      if (rtp_gate_get(id, gate_no, &gate) != 0)
         return server_send_error(conn, "pipeline: merge-pending but gate record missing", NULL);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "action", "merge_reconcile");
      execute_gate_merge(id, &run, &gate, gate_no, req, resp);
      return server_send_ok(conn, resp);
   }
   if (strncmp(run.state, "gate", 4) == 0)
      return server_send_error(conn, "pipeline: at a gate; use pipeline.gate to resolve", NULL);

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "pipeline: could not load configuration", NULL);

   const char *phase = phase_for_state(run.state);
   const char *artifact = jo_str(req, "artifact", NULL);
   char artifact_hash[RTP_HASH_LEN] = {0};
   const char *prov_hash = jo_str(req, "artifact_hash", NULL);
   if (prov_hash && prov_hash[0])
      snprintf(artifact_hash, sizeof(artifact_hash), "%s", prov_hash);

   rtp_pass_t latest;
   int have = rtp_pass_latest(id, phase, &latest) == 0;

   /* If a pass is in flight (open + a pending current attempt), wait. */
   if (have && strcmp(latest.status, RTP_PASS_OPEN) == 0)
   {
      rtp_attempt_t a;
      if (rtp_attempt_current(latest.id, &a) == 0 &&
          strcmp(a.capture_status, RTP_CAP_PENDING) == 0)
      {
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", "waiting");
         cJSON_AddStringToObject(resp, "run_id", a.run_id);
         cJSON_AddNumberToObject(resp, "pass_id", latest.id);
         return server_send_ok(conn, resp);
      }
   }

   /* A captured pass that hasn't yet been acted on -> decide. */
   if (have && strcmp(latest.status, RTP_PASS_CAPTURED) == 0 &&
       !(artifact && artifact[0] && artifact_hash[0] &&
         strcmp(artifact_hash, latest.artifact_hash) != 0))
   {
      rtp_attempt_t a;
      int hav_a = rtp_attempt_current(latest.id, &a) == 0;
      rtp_envelope_t env;
      env_from_ledger(&latest, hav_a ? &a : NULL, &env);

      rtp_loop_cfg_t lc;
      load_loop_cfg(&cfg, &run, &lc);
      rtp_loop_state_t ls = {latest.pass_no, hav_a ? a.attempt_no : 1, phase_cost(&run, phase), 0};
      rtp_action_t act = rtp_loop_decide(&lc, &ls, &env);

      if (act == RTP_ACT_PASS)
      {
         /* mark pass done; open the gate (needs the PR opened first). */
         snprintf(latest.status, sizeof(latest.status), RTP_PASS_DONE);
         rtp_pass_update(&latest);
         int pr = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run.impl_pr_number : run.proposal_pr_number;
         if (pr <= 0)
         {
            cJSON *resp = jo_ok();
            cJSON_AddStringToObject(resp, "action", "open_pr");
            cJSON_AddStringToObject(
                resp, "note",
                "done-bar met; open the PR (git_pr create) then call advance to surface the gate");
            return server_send_ok(conn, resp);
         }
         int gate_no = strcmp(phase, RTP_PHASE_IMPL) == 0 ? 2 : 1;
         rtp_gate_create(id, gate_no, pr, run.head_sha, NULL);
         rtp_run_set_state(id, gate_no == 2 ? RTP_STATE_GATE2_PENDING : RTP_STATE_GATE1_PENDING,
                           NULL);
         rtp_run_get(id, &run);
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", "gate_pending");
         cJSON_AddItemToObject(resp, "digest", build_digest(&run, &latest, 1));
         return server_send_ok(conn, resp);
      }
      if (act == RTP_ACT_ESCALATE)
      {
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", "escalate");
         cJSON_AddStringToObject(resp, "note",
                                 "pass ceiling / cost cap / invalid evidence reached without the "
                                 "done-bar; human attention required (never auto-passed)");
         cJSON_AddItemToObject(resp, "digest", build_digest(&run, &latest, 1));
         return server_send_ok(conn, resp);
      }
      if (act == RTP_ACT_RETRY)
      {
         if (!artifact || !artifact[0])
         {
            cJSON *resp = jo_ok();
            cJSON_AddStringToObject(resp, "action", "retry");
            cJSON_AddStringToObject(
                resp, "note", "capture fault; re-call advance with the same artifact to re-run");
            return server_send_ok(conn, resp);
         }
         /* new attempt under the same pass id. */
         int attempt_no = rtp_attempt_max_no(latest.id) + 1;
         (void)attempt_no; /* the seam allocates + supersedes on submit */
         snprintf(latest.status, sizeof(latest.status), RTP_PASS_OPEN);
         rtp_pass_update(&latest);
         cJSON *body = cJSON_CreateObject();
         cJSON_AddStringToObject(body, "task", artifact);
         cJSON_AddStringToObject(body, "mode", strcmp(latest.mode, RTP_MODE_DRAFT) == 0 ? "draft"
                                                                                       : "review");
         if (run.brief[0])
            cJSON_AddStringToObject(body, "brief", run.brief);
         cJSON_AddNumberToObject(body, "pipeline_pass_id", latest.id);
         char *bj = cJSON_PrintUnformatted(body);
         cJSON_Delete(body);
         char rr[4096];
         int rc = server_http_submit_op_run("delegate.roundtable", bj ? bj : "{}",
                                            conn->capabilities, rr, (int)sizeof(rr));
         free(bj);
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", (rc >= 200 && rc < 300) ? "submitted" : "error");
         cJSON_AddNumberToObject(resp, "pass_id", latest.id);
         return server_send_ok(conn, resp);
      }
      /* RTP_ACT_REVISE */
      if (!(artifact && artifact[0]))
      {
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", "revise");
         cJSON_AddNumberToObject(resp, "blocking", latest.blocking_count);
         cJSON_AddNumberToObject(resp, "suggestions", latest.suggestion_count);
         cJSON_AddStringToObject(
             resp, "note",
             "blocking items remain; revise the artifact and call advance with the new --artifact");
         return server_send_ok(conn, resp);
      }
      /* a new artifact was supplied -> fall through to submit a fresh pass. */
   }

   /* Otherwise: submit the next pass. drafting -> DRAFT, review states -> REVIEW. */
   const char *mode = strcmp(run.state, RTP_STATE_DRAFTING) == 0 ? RTP_MODE_DRAFT : RTP_MODE_REVIEW;
   if (strcmp(run.state, RTP_STATE_DRAFTING) != 0 &&
       strcmp(run.state, review_state_for_phase(phase)) != 0)
   {
      /* nudge the state into the review phase if we're implementing and have a
       * diff to review. */
      if (strcmp(run.state, RTP_STATE_IMPLEMENTING) == 0 && artifact && artifact[0])
         rtp_run_set_state(id, RTP_STATE_PR_REVIEW, RTP_PHASE_IMPL);
      else
         return server_send_error(conn,
                                  "pipeline: nothing to advance in this state without an artifact",
                                  NULL);
      rtp_run_get(id, &run);
   }
   if ((!artifact || !artifact[0]) && strcmp(mode, RTP_MODE_REVIEW) == 0)
      return server_send_error(
          conn, "pipeline: review needs the artifact (--artifact <diff|proposal>)", NULL);
   return submit_pass(conn, &run, phase, mode, artifact, artifact_hash);
}

/* ----------------------------------------------------------------- gate ---- */

/* Run the policy-aware merge for an approved gate, record full evidence into the
 * gate row, and advance on success. Shared by gate-pass and the *_merge_pending
 * crash-recovery reconcile (#56). Fills `resp`. */
static void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
                               cJSON *resp)
{
   cJSON *margs = cJSON_CreateObject();
   cJSON_AddStringToObject(margs, "action", "merge");
   cJSON_AddNumberToObject(margs, "number", gate->pr_number);
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "admin")))
      cJSON_AddBoolToObject(margs, "admin", 1);
   if (gate->expected_head_sha[0])
      cJSON_AddStringToObject(margs, "expected_head_sha", gate->expected_head_sha);
   cJSON *mres = handle_git_pr(margs);
   cJSON_Delete(margs);

   int merged = 0;
   char merge_sha[RTP_HASH_LEN] = {0};
   int exit_code = -1;
   if (mres)
   {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(mres, "content");
      cJSON *first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
      cJSON *txt = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;
      if (cJSON_IsString(txt))
      {
         cJSON *mj = cJSON_Parse(txt->valuestring);
         if (mj)
         {
            merged = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mj, "merged"));
            cJSON *es = cJSON_GetObjectItemCaseSensitive(mj, "exit_code");
            if (cJSON_IsNumber(es))
               exit_code = (int)es->valuedouble;
            cJSON *ms = cJSON_GetObjectItemCaseSensitive(mj, "merge_sha");
            if (cJSON_IsString(ms))
               snprintf(merge_sha, sizeof(merge_sha), "%s", ms->valuestring);
            cJSON *out = cJSON_GetObjectItemCaseSensitive(mj, "output");
            if (cJSON_IsString(out))
               snprintf(gate->merge_output, sizeof(gate->merge_output), "%s", out->valuestring);
            cJSON_Delete(mj);
         }
      }
      cJSON_Delete(mres);
   }
   snprintf(gate->merge_executor, sizeof(gate->merge_executor), "git_pr");
   snprintf(gate->merge_command, sizeof(gate->merge_command), "gh pr merge %d", gate->pr_number);
   gate->merge_exit_code = exit_code;
   if (merged && merge_sha[0])
      snprintf(gate->merge_sha, sizeof(gate->merge_sha), "%s", merge_sha);
   rtp_gate_update(gate);

   if (merged)
   {
      const char *next = gate_no == 2 ? RTP_STATE_DONE : RTP_STATE_IMPLEMENTING;
      rtp_run_set_state(id, next, RTP_PHASE_IMPL);
      cJSON_AddBoolToObject(resp, "merged", 1);
      cJSON_AddStringToObject(resp, "merge_sha", gate->merge_sha);
      cJSON_AddStringToObject(resp, "state", next);
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

static int gate_authorized(cJSON *req)
{
   const char *principal = jo_str(req, "operator_principal", NULL);
   db1_local_operator_t op;
   int active = (db1_local_operator_get_active(&op) == 0 && op.active);
   return rtp_gate_authority_ok(principal, active ? op.operator_uuid : "", active);
}

int handle_pipeline_gate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   const char *verdict = jo_str(req, "verdict", NULL);
   if (id <= 0 || !verdict || (strcmp(verdict, "pass") != 0 && strcmp(verdict, "fail") != 0))
      return server_send_error(conn, "usage: aimee pipeline gate <id> pass|fail [--reason ...]",
                               NULL);

   /* Authority separation (#53): the driving agent (CAP_DELEGATE) must not be
    * able to resolve its own gate. v1 requires an enrolled local operator
    * principal that a delegate-driving session does not possess. */
   if (!gate_authorized(req))
      return server_send_error(
          conn, "pipeline: gate resolution requires an enrolled local operator principal", NULL);

   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);

   /* Exactly-once (#55): act only from the matching *_pending state. */
   int gate_no;
   const char *review_back;
   if (strcmp(run.state, RTP_STATE_GATE1_PENDING) == 0)
   {
      gate_no = 1;
      review_back = RTP_STATE_PROPOSAL_REVIEW;
   }
   else if (strcmp(run.state, RTP_STATE_GATE2_PENDING) == 0)
   {
      gate_no = 2;
      review_back = RTP_STATE_PR_REVIEW;
   }
   else
      return server_send_error(conn, "pipeline: not awaiting a gate verdict (already resolving?)",
                               NULL);

   rtp_gate_t gate;
   if (rtp_gate_get(id, gate_no, &gate) != 0)
      return server_send_error(conn, "pipeline: gate record missing", NULL);

   const char *reason = jo_str(req, "reason", "");
   const char *actor = jo_str(req, "operator_principal", "operator");
   snprintf(gate.verdict, sizeof(gate.verdict), "%s", verdict);
   snprintf(gate.reason, sizeof(gate.reason), "%s", reason ? reason : "");
   snprintf(gate.actor, sizeof(gate.actor), "%s", actor ? actor : "operator");
   snprintf(gate.resolved_at, sizeof(gate.resolved_at), "resolved");

   if (strcmp(verdict, "fail") == 0)
   {
      rtp_gate_update(&gate);
      /* fail reason -> brief, return to the review phase (#43 same PR). */
      if (reason && reason[0])
      {
         char nb[RTP_BRIEF_LEN];
         snprintf(nb, sizeof(nb), "%s\nhuman-gate-%d fail: %s", run.brief, gate_no, reason);
         snprintf(run.brief, sizeof(run.brief), "%s", nb);
      }
      rtp_run_update(&run);
      rtp_run_set_state(id, review_back, NULL);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "verdict", "fail");
      cJSON_AddStringToObject(resp, "state", review_back);
      cJSON_AddStringToObject(resp, "note",
                              "fail reason recorded in the brief; re-review pushes to the SAME PR");
      return server_send_ok(conn, resp);
   }

   /* pass: atomically write the merge intent + move to *_merge_pending (#56),
    * then run the policy-aware merge and advance on success. */
   const char *merge_state =
       gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
   rtp_gate_update(&gate);
   rtp_run_set_state(id, merge_state, NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "verdict", "pass");
   execute_gate_merge(id, &run, &gate, gate_no, req, resp);
   return server_send_ok(conn, resp);
}
