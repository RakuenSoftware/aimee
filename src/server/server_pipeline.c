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

/* ~4 bytes per token is a conservative budget conversion; the per-chunk byte
 * budget reserves headroom for the brief/role/peer-note framing (section 2). */
#define RTP_BYTES_PER_TOKEN  4
#define RTP_CHUNK_RESERVE_FR 4 /* reserve 1/4 of the budget for framing */

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
      e->has_error =
          (strcmp(a->capture_status, RTP_CAP_FAILED) == 0 && !a->truncated && !a->degraded) ? 1 : 0;
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
   e->answered_count = p->answered_count; /* persisted from capture (#3) */
   e->items_round = p->items_round;
   e->artifact_round = p->artifact_round;
   e->best_round = p->best_round;
   e->rounds_run = p->rounds_run;
   /* DRAFT validity is decided directly from the attempt (draft_complete), not
    * via this reconstruction; mirror the captured attempt's verdict so the
    * predicate stays consistent rather than assuming an artifact always exists. */
   e->artifact_present = (e->is_draft && a) ? a->envelope_valid : 0;
}

static void load_loop_cfg(const config_t *cfg, const rtp_run_t *run, rtp_loop_cfg_t *out)
{
   out->done_bar = run->done_bar[0] ? run->done_bar : cfg->roundtable_pipeline_done_bar;
   out->max_passes = cfg->roundtable_pipeline_max_passes;
   out->max_attempts_per_pass = cfg->roundtable_pipeline_max_attempts_per_pass > 0
                                    ? cfg->roundtable_pipeline_max_attempts_per_pass
                                    : 2;
   out->max_phase_cost_usd = cfg->roundtable_pipeline_max_cost_usd;
   out->max_total_cost_usd = cfg->roundtable_pipeline_max_total_cost_usd;
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

/* forward decls: defined later in the file. */
static void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
                               cJSON *resp);
static int resolve_panel(const config_t *cfg, rtp_panel_t *out);
static int maybe_ttl_abandon(int id, rtp_run_t *run, const config_t *cfg);

/* Open a human gate: record the gate (PR + expected head SHA as the merge-intent
 * key), move to *_pending, and release re-creatable resources (#47) — the
 * review-scoped chunk index is dropped (it re-derives from the retained origin
 * hash on resume) and, if configured, the parked gate releases the single active
 * admission slot so another pipeline can run while a human is away. */
static void enter_gate(int id, int gate_no, int pr)
{
   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return;
   rtp_gate_create(id, gate_no, pr, run.head_sha, NULL);
   rtp_run_set_state(id, gate_no == 2 ? RTP_STATE_GATE2_PENDING : RTP_STATE_GATE1_PENDING, NULL);

   config_t cfg;
   int parked_releases =
       (config_load(&cfg) == 0) ? cfg.roundtable_pipeline_parked_releases_slot : 1;
   if (rtp_run_get(id, &run) == 0)
   {
      /* the chunk index is re-creatable from the retained origin, so dropping it
       * while parked leaks nothing (#34/#47); it re-derives on the next pass. */
      run.chunk_index_ref[0] = '\0';
      if (parked_releases)
         snprintf(run.admission_class, sizeof(run.admission_class), RTP_ADMIT_WAITING_HUMAN);
      rtp_run_update(&run);
   }
}

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
   const char *head_branch = jo_str(req, "head_branch", NULL);

   /* Reject (never silently truncate) an over-long question set before it can be
    * mistaken as fully answered (#14). */
   cJSON *qreq = cJSON_GetObjectItemCaseSensitive(req, "questions");
   if (cJSON_IsArray(qreq) && cJSON_GetArraySize(qreq) > RTP_MAX_BRIEF_QUESTIONS)
      return server_send_error(
          conn, "pipeline: too many questions (max 16); split into multiple review passes", NULL);

   /* Branch/PR ownership guard (#48): even with the active slot free (parked
    * gate), a second run may not target a branch another non-terminal run owns. */
   if (head_branch && head_branch[0] && rtp_run_branch_owner(repo_root, head_branch, 0) > 0)
      return server_send_error(
          conn, "pipeline: head_branch is already owned by another active pipeline", NULL);

   int id = 0;
   if (rtp_run_create(idea, done_bar, repo_root, base, &id) != 0 || id <= 0)
      return server_send_error(conn, "pipeline: could not create pipeline", NULL);

   /* seed the brief from the idea + any seed questions. When `questions` are
    * supplied the brief is stored as a JSON object so they reach the panel and
    * the strict questions bar can be enforced (#3/#14); accepted_question_count
    * is recorded (capped at RTP_MAX_BRIEF_QUESTIONS). Also capture repo/branch
    * state for the PR lifecycle (#2). */
   rtp_run_t run;
   if (rtp_run_get(id, &run) == 0)
   {
      const char *seed = jo_str(req, "brief", NULL);
      cJSON *questions = cJSON_GetObjectItemCaseSensitive(req, "questions");
      if (cJSON_IsArray(questions) && cJSON_GetArraySize(questions) > 0)
      {
         cJSON *bo = cJSON_CreateObject();
         cJSON *focus = cJSON_AddArrayToObject(bo, "focus");
         char goal[RTP_IDEA_LEN + 16];
         snprintf(goal, sizeof(goal), "goal: %s", idea);
         cJSON_AddItemToArray(focus, cJSON_CreateString(goal));
         if (seed && seed[0])
            cJSON_AddItemToArray(focus, cJSON_CreateString(seed));
         cJSON *qarr = cJSON_AddArrayToObject(bo, "questions");
         int qc = 0;
         cJSON *q = NULL;
         cJSON_ArrayForEach(q, questions)
         {
            if (cJSON_IsString(q) && q->valuestring[0] && qc < RTP_MAX_BRIEF_QUESTIONS)
            {
               cJSON_AddItemToArray(qarr, cJSON_CreateString(q->valuestring));
               qc++;
            }
         }
         char *bs = cJSON_PrintUnformatted(bo);
         cJSON_Delete(bo);
         if (bs)
         {
            snprintf(run.brief, sizeof(run.brief), "%s", bs);
            free(bs);
         }
         run.accepted_question_count = qc;
      }
      else if (seed && seed[0])
         snprintf(run.brief, sizeof(run.brief), "%s", seed);
      else
         snprintf(run.brief, sizeof(run.brief), "goal: %s", idea);

      /* repo/branch capture for the PR lifecycle (#2). */
      const char *hb = jo_str(req, "head_branch", NULL);
      if (hb && hb[0])
         snprintf(run.head_branch, sizeof(run.head_branch), "%s", hb);
      const char *remote = jo_str(req, "remote", NULL);
      if (remote && remote[0])
         snprintf(run.remote, sizeof(run.remote), "%s", remote);
      const char *wt = jo_str(req, "worktree_path", NULL);
      if (wt && wt[0])
         snprintf(run.worktree_path, sizeof(run.worktree_path), "%s", wt);
      rtp_run_update(&run);
   }

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", id);
   cJSON_AddStringToObject(resp, "state", RTP_STATE_DRAFTING);
   cJSON_AddStringToObject(resp, "done_bar", done_bar);

   /* Surface panel diversity now so a single-provider panel is flagged before
    * the loop runs (section 7): a mono-provider panel can converge on its own
    * blind spots, so its "converged" is weaker evidence. */
   rtp_panel_t panel;
   if (resolve_panel(&cfg, &panel) == 0)
   {
      cJSON *pj = cJSON_AddObjectToObject(resp, "panel");
      cJSON_AddNumberToObject(pj, "requested", panel.requested);
      cJSON_AddNumberToObject(pj, "resolved", panel.resolved);
      cJSON_AddNumberToObject(pj, "distinct_providers", panel.distinct_providers);
      cJSON_AddNumberToObject(pj, "min_context_tokens", panel.min_context_tokens);
      cJSON_AddBoolToObject(pj, "diverse", rtp_panel_diverse(&panel) ? 1 : 0);
      if (!rtp_panel_diverse(&panel))
         cJSON_AddStringToObject(
             pj, "warning",
             "single-provider panel — convergence may reflect shared blind spots (section 7)");
   }
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

   /* lazily enforce the unanswered-gate TTL when the run is observed (#47). */
   config_t scfg;
   if (config_load(&scfg) == 0)
      maybe_ttl_abandon(id, &run, &scfg);

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
   config_t lcfg;
   int have_cfg = (config_load(&lcfg) == 0);
   rtp_run_t rows[64];
   int n = rtp_run_list(filter, rows, 64);
   if (n < 0)
      n = 0;
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "pipelines");
   for (int i = 0; i < n; i++)
   {
      /* lazily enforce the unanswered-gate TTL on each observed run (#47). */
      if (have_cfg)
         maybe_ttl_abandon(rows[i].id, &rows[i], &lcfg);
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

/* Attach the artifact as `prompt` (handle_delegate_roundtable reads "prompt",
 * min 20 chars — #1) and the brief — as a JSON object when run.brief holds one
 * (so questions/invariants/fixes reach the panel and the questions bar can be
 * enforced, #3), otherwise as a focus string. */
static void attach_prompt_brief(cJSON *body, const char *artifact, const char *brief_str)
{
   cJSON_AddStringToObject(body, "prompt", artifact ? artifact : "");
   if (brief_str && brief_str[0])
   {
      cJSON *bo = cJSON_Parse(brief_str);
      if (bo && cJSON_IsObject(bo))
         cJSON_AddItemToObject(body, "brief", bo);
      else
      {
         cJSON_Delete(bo);
         cJSON_AddStringToObject(body, "brief", brief_str);
      }
   }
}

/* ---- PR / git lifecycle (#2), pinned to the pipeline's recorded checkout (#3) -- */

/* The directory the pipeline's git/gh operations must run in: the recorded
 * dedicated worktree, else the repo root. Empty -> the server's active
 * workspace (back-compat). Every git/gh command below is anchored to it so the
 * pipeline never operates on the wrong checkout/branch. */
static const char *rtp_git_cwd(const rtp_run_t *run)
{
   if (run->worktree_path[0])
      return run->worktree_path;
   if (run->repo_root[0])
      return run->repo_root;
   return "";
}

/* Prepend `cd '<cwd>' && ` (escaped) when a checkout is recorded. */
static size_t rtp_cd_prefix(const rtp_run_t *run, char *buf, size_t cap)
{
   const char *cwd = rtp_git_cwd(run);
   if (!cwd[0])
   {
      buf[0] = '\0';
      return 0;
   }
   char *e = shell_escape(cwd);
   int n = snprintf(buf, cap, "cd '%s' && ", e ? e : cwd);
   free(e);
   return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static int git_head_sha(const rtp_run_t *run, char *out, size_t cap)
{
   char cmd[RTP_PATH_LEN + 64];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + p, sizeof(cmd) - p, "git rev-parse HEAD 2>/dev/null");
   int rc = 0;
   char *o = mcp_git_run(cmd, &rc);
   if (!o)
      return -1;
   char *nl = strchr(o, '\n');
   if (nl)
      *nl = '\0';
   int ok = (rc == 0 && o[0]) ? 0 : -1;
   snprintf(out, cap, "%s", ok == 0 ? o : "");
   free(o);
   return ok;
}

/* `git diff <base>...HEAD` in the recorded checkout; caller frees. */
static char *capture_diff(const rtp_run_t *run)
{
   const char *base = run->base_branch[0] ? run->base_branch : "HEAD~1";
   char *eb = shell_escape(base);
   char cmd[RTP_PATH_LEN + 128];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + p, sizeof(cmd) - p, "git diff '%s'...HEAD 2>/dev/null", eb ? eb : base);
   free(eb);
   int rc = 0;
   char *o = mcp_git_run(cmd, &rc);
   if (o && rc == 0 && o[0])
      return o;
   free(o);
   return NULL;
}

/* The last run of digits in a PR URL (".../pull/<n>") is the PR number. */
static int parse_pr_number(const char *text)
{
   int best = 0;
   for (const char *p = text ? text : ""; *p;)
   {
      if (*p >= '0' && *p <= '9')
      {
         int v = 0;
         while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
         best = v;
      }
      else
         p++;
   }
   return best;
}

/* Open the phase PR with `gh pr create` in the recorded checkout, on the
 * recorded head_branch + base, and record number/url + current head SHA (#2/#3).
 * The agent must have pushed its commits first. Returns 0 on success. */
static int open_and_record_pr(rtp_run_t *run, const char *phase)
{
   char title[256];
   snprintf(title, sizeof(title), "%s: %.180s",
            strcmp(phase, RTP_PHASE_IMPL) == 0 ? "impl" : "proposal", run->idea);
   char body[512];
   snprintf(body, sizeof(body),
            "Roundtable authoring pipeline #%d (%s phase). Done-bar met; PR opened by the pipeline "
            "controller.",
            run->id, phase);
   char *et = shell_escape(title);
   char *eb = shell_escape(body);
   char *ebase = shell_escape(run->base_branch[0] ? run->base_branch : "testing");
   char *ehead = run->head_branch[0] ? shell_escape(run->head_branch) : NULL;

   char cmd[RTP_PATH_LEN + 1280];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   if (ehead)
      snprintf(cmd + p, sizeof(cmd) - p,
               "gh pr create --title '%s' --body '%s' --base '%s' --head '%s' 2>&1", et ? et : "",
               eb ? eb : "", ebase ? ebase : "testing", ehead);
   else
      snprintf(cmd + p, sizeof(cmd) - p, "gh pr create --title '%s' --body '%s' --base '%s' 2>&1",
               et ? et : "", eb ? eb : "", ebase ? ebase : "testing");
   free(et);
   free(eb);
   free(ebase);
   free(ehead);

   int rc = 0;
   char *out = mcp_git_run(cmd, &rc);
   int pr = 0;
   char url[RTP_PATH_LEN] = {0};
   if (out && rc == 0)
   {
      const char *u = strstr(out, "http");
      if (u)
      {
         snprintf(url, sizeof(url), "%s", u);
         char *cut = strpbrk(url, " \n\"");
         if (cut)
            *cut = '\0';
      }
      pr = parse_pr_number(url[0] ? url : out);
   }
   free(out);
   if (pr <= 0)
      return -1;

   char sha[RTP_HASH_LEN] = {0};
   git_head_sha(run, sha, sizeof(sha));
   if (strcmp(phase, RTP_PHASE_IMPL) == 0)
   {
      run->impl_pr_number = pr;
      snprintf(run->impl_pr_url, sizeof(run->impl_pr_url), "%s", url);
   }
   else
   {
      run->proposal_pr_number = pr;
      snprintf(run->proposal_pr_url, sizeof(run->proposal_pr_url), "%s", url);
   }
   if (sha[0])
      snprintf(run->head_sha, sizeof(run->head_sha), "%s", sha);
   rtp_run_update(run);
   return 0;
}

/* The PR's current head SHA via the forge (in the recorded checkout), for drift
 * detection (#56). Empty on failure. */
static void pr_current_head(const rtp_run_t *run, int pr_number, char *out, size_t cap)
{
   out[0] = '\0';
   if (pr_number <= 0)
      return;
   char cmd[RTP_PATH_LEN + 128];
   size_t p = rtp_cd_prefix(run, cmd, sizeof(cmd));
   snprintf(cmd + p, sizeof(cmd) - p, "gh pr view %d --json headRefOid -q .headRefOid 2>/dev/null",
            pr_number);
   int rc = 0;
   char *o = mcp_git_run(cmd, &rc);
   if (o && rc == 0)
   {
      char *nl = strchr(o, '\n');
      if (nl)
         *nl = '\0';
      snprintf(out, cap, "%s", o);
   }
   free(o);
}

/* Per-pass artifact is persisted to a durable working file so a lost-result
 * attempt can be auto re-run under the same pass id without the caller resending
 * it (#19), and the content hash pins the replay (#24). */
static void pass_file_path(const rtp_run_t *run, int pass_id, char *buf, size_t cap)
{
   snprintf(buf, cap, "%s/roundtable_pipeline/%d/pass-%d.txt", aimee_home(), run->id, pass_id);
}

static void persist_pass_artifact(const rtp_run_t *run, int pass_id, const char *artifact)
{
   char dir[RTP_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s/roundtable_pipeline/%d", aimee_home(), run->id);
   platform_mkdir_p(dir, 0700);
   char path[RTP_PATH_LEN];
   pass_file_path(run, pass_id, path, sizeof(path));
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   if (artifact)
      fwrite(artifact, 1, strlen(artifact), f);
   fclose(f);
}

static char *read_text_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n < 0)
   {
      fclose(f);
      return NULL;
   }
   char *b = (char *)malloc((size_t)n + 1);
   if (!b)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(b, 1, (size_t)n, f);
   fclose(f);
   b[rd] = '\0';
   return b;
}

static char *read_pass_artifact(const rtp_run_t *run, int pass_id)
{
   char path[RTP_PATH_LEN];
   pass_file_path(run, pass_id, path, sizeof(path));
   return read_text_file(path);
}

static int submit_pass(server_conn_t *conn, rtp_run_t *run, const char *phase, const char *mode,
                       const char *artifact, const char *artifact_hash)
{
   int pass_no = rtp_pass_max_no(run->id, phase) + 1;
   int pass_id = 0;
   if (rtp_pass_create(run->id, phase, mode, pass_no, artifact_hash, &pass_id) != 0)
      return server_send_error(conn, "pipeline: could not create pass", NULL);

   /* pin the pass to the content hash + persist the artifact for auto-retry. */
   char ahash[RTP_CHUNK_HASH_LEN];
   rtp_chunk_hash(artifact ? artifact : "", artifact ? (int)strlen(artifact) : 0, ahash,
                  sizeof(ahash));
   rtp_pass_t pp;
   if (rtp_pass_get(pass_id, &pp) == 0)
   {
      snprintf(pp.artifact_hash, sizeof(pp.artifact_hash), "%s", ahash);
      rtp_pass_update(&pp);
   }
   persist_pass_artifact(run, pass_id, artifact);

   cJSON *body = cJSON_CreateObject();
   attach_prompt_brief(body, artifact, run->brief);
   cJSON_AddStringToObject(body, "mode", strcmp(mode, RTP_MODE_DRAFT) == 0 ? "draft" : "review");
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

/* Resolve the ensemble panel to provider identity + min context budget (section
 * 7/#36). Returns 0 on success (fills out), -1 if a participant has an unknown
 * context window and no fallback is configured. */
static int resolve_panel(const config_t *cfg, rtp_panel_t *out)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      memset(out, 0, sizeof(*out));
      return 0; /* no registry -> treat as unresolved, caller decides */
   }
   rtp_participant_t parts[ENSEMBLE_MAX_REFS];
   int n = cfg->ensemble_reference_count;
   if (n > ENSEMBLE_MAX_REFS)
      n = ENSEMBLE_MAX_REFS;
   for (int i = 0; i < n; i++)
   {
      agent_t *ag = agent_find(&acfg, cfg->ensemble_reference_models[i]);
      if (ag)
      {
         parts[i].provider = ag->provider;
         parts[i].context_tokens = ag->middleware.context_window > 0
                                       ? ag->middleware.context_window
                                       : model_context_window(ag->model);
      }
      else
      {
         parts[i].provider = NULL;
         parts[i].context_tokens = 0;
      }
   }
   int fallback = cfg->roundtable_pipeline_unknown_context_tokens;
   int rc = rtp_panel_summarize(parts, n, fallback, out);
   /* providers point into acfg, which is about to free; copy what we need first.
    * out only stores counts/min, not provider strings, so this is safe. */
   return rc;
}

/* Per-chunk byte budget from the panel's smallest participant (#33), reserving
 * framing headroom. 0 = no budget resolved (single-shot review). */
static int chunk_budget_bytes(const rtp_panel_t *panel)
{
   if (!panel || panel->min_context_tokens <= 0)
      return 0;
   long bytes = (long)panel->min_context_tokens * RTP_BYTES_PER_TOKEN;
   bytes -= bytes / RTP_CHUNK_RESERVE_FR;
   if (bytes < 1024)
      bytes = 1024;
   if (bytes > 1 << 20)
      bytes = 1 << 20;
   return (int)bytes;
}

/* Submit one chunk-pass of a chunked review group. For a chunk member, span =
 * {offset,len}; for the synthesis member (chunk_index == -1), omitted/over_budget
 * record the assembly manifest so the aggregate blocks on incomplete coverage. */
static int submit_chunk_pass(server_conn_t *conn, rtp_run_t *run, const char *phase,
                             const char *artifact, const char *artifact_hash, int group,
                             int chunk_index, int chunk_total, int span_offset, int span_len,
                             int omitted, int over_budget)
{
   int pass_no = rtp_pass_max_no(run->id, phase) + 1;
   int pass_id = 0;
   if (rtp_pass_create(run->id, phase, RTP_MODE_REVIEW, pass_no, artifact_hash, &pass_id) != 0)
      return -1;
   rtp_pass_t p;
   if (rtp_pass_get(pass_id, &p) == 0)
   {
      p.is_chunked = 1;
      p.chunk_group = group;
      p.chunk_index = chunk_index;
      p.chunk_total = chunk_total;
      p.chunk_offset = span_offset;
      p.chunk_len = span_len;
      p.chunk_omitted = omitted;
      p.chunk_over_budget = over_budget;
      rtp_pass_update(&p);
   }
   cJSON *body = cJSON_CreateObject();
   attach_prompt_brief(body, artifact, run->brief);
   cJSON_AddStringToObject(body, "mode", "review");
   cJSON_AddNumberToObject(body, "pipeline_pass_id", pass_id);
   char *bj = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   char rr[2048];
   int rc = server_http_submit_op_run("delegate.roundtable", bj ? bj : "{}", conn->capabilities, rr,
                                      (int)sizeof(rr));
   free(bj);
   if (rc < 200 || rc >= 300)
   {
      if (rtp_pass_get(pass_id, &p) == 0)
      {
         snprintf(p.status, sizeof(p.status), RTP_PASS_FAILED);
         rtp_pass_update(&p);
      }
      return -1;
   }
   return pass_id;
}

/* Persist the whole origin to a durable working file and record its ref+hash on
 * the run (#34: origin always recoverable; chunks re-derive from it, #42/#47).
 * Returns 0 on success. The path goes into proposal_ref/diff_ref. */
static int persist_origin(rtp_run_t *run, const char *phase, const char *artifact,
                          const char *origin_hash)
{
   char dir[RTP_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s/roundtable_pipeline/%d", aimee_home(), run->id);
   platform_mkdir_p(dir, 0700);
   char path[RTP_PATH_LEN];
   snprintf(path, sizeof(path), "%s/origin-%s.txt", dir, phase);
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t len = strlen(artifact);
   size_t wrote = fwrite(artifact, 1, len, f);
   fclose(f);
   if (wrote != len)
      return -1;
   if (strcmp(phase, RTP_PHASE_IMPL) == 0)
   {
      snprintf(run->diff_ref, sizeof(run->diff_ref), "%s", path);
      snprintf(run->diff_origin_hash, sizeof(run->diff_origin_hash), "%s", origin_hash);
   }
   else
   {
      snprintf(run->proposal_ref, sizeof(run->proposal_ref), "%s", path);
      snprintf(run->proposal_origin_hash, sizeof(run->proposal_origin_hash), "%s", origin_hash);
   }
   snprintf(run->chunk_index_ref, sizeof(run->chunk_index_ref), "origin:%s", origin_hash);
   rtp_run_update(run);
   return 0;
}

/* Auto re-run a lost-result pass under the SAME pass id + a new attempt (#19),
 * pinned to the persisted artifact's hash (#24). Escalates if the artifact is
 * unavailable or its hash drifted. Driven by the controller, not the caller. */
static int resubmit_same_pass(server_conn_t *conn, rtp_run_t *run, rtp_pass_t *latest)
{
   char *a = read_pass_artifact(run, latest->id);
   if (!a)
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "action", "escalate");
      cJSON_AddStringToObject(resp, "note",
                              "capture fault and the pass artifact is unavailable to re-run; human "
                              "attention required");
      return server_send_ok(conn, resp);
   }
   char h[RTP_CHUNK_HASH_LEN];
   rtp_chunk_hash(a, (int)strlen(a), h, sizeof(h));
   if (latest->artifact_hash[0] && strcmp(h, latest->artifact_hash) != 0)
   {
      free(a);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "action", "escalate");
      cJSON_AddStringToObject(
          resp, "note", "artifact changed since the lost attempt; not reusing the pass id (#24)");
      return server_send_ok(conn, resp);
   }
   snprintf(latest->status, sizeof(latest->status), RTP_PASS_OPEN);
   rtp_pass_update(latest);
   cJSON *body = cJSON_CreateObject();
   attach_prompt_brief(body, a, run->brief);
   cJSON_AddStringToObject(body, "mode",
                           strcmp(latest->mode, RTP_MODE_DRAFT) == 0 ? "draft" : "review");
   cJSON_AddNumberToObject(body, "pipeline_pass_id", latest->id);
   char *bj = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   free(a);
   char rr[4096];
   int rc = server_http_submit_op_run("delegate.roundtable", bj ? bj : "{}", conn->capabilities, rr,
                                      (int)sizeof(rr));
   free(bj);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "action", (rc >= 200 && rc < 300) ? "retrying" : "error");
   cJSON_AddNumberToObject(resp, "pass_id", latest->id);
   cJSON_AddStringToObject(resp, "note",
                           "lost-result auto re-run under the same pass id, new attempt (#19)");
   return server_send_ok(conn, resp);
}

/* DRAFT completion (#1/#9): a valid DRAFT pass produced a skeleton (in the
 * attempt result's `artifact`). Store it as the proposal working artifact
 * (ref + hash, #34), mark the pass done, and transition drafting ->
 * proposal_review — NOT to a PR/gate. */
static int draft_complete(server_conn_t *conn, rtp_run_t *run, rtp_pass_t *latest,
                          const rtp_attempt_t *att)
{
   char *skeleton = NULL;
   cJSON *snap = att->result_snapshot[0] ? cJSON_Parse(att->result_snapshot) : NULL;
   if (snap)
   {
      cJSON *a = cJSON_GetObjectItemCaseSensitive(snap, "artifact");
      if (cJSON_IsString(a) && a->valuestring[0])
         skeleton = strdup(a->valuestring);
      cJSON_Delete(snap);
   }
   if (!skeleton || !skeleton[0])
   {
      free(skeleton);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "action", "escalate");
      cJSON_AddStringToObject(resp, "note",
                              "DRAFT produced no skeleton artifact; human attention required");
      return server_send_ok(conn, resp);
   }
   char hash[RTP_CHUNK_HASH_LEN];
   rtp_chunk_hash(skeleton, (int)strlen(skeleton), hash, sizeof(hash));
   persist_origin(run, RTP_PHASE_PROPOSAL, skeleton, hash);
   free(skeleton);
   snprintf(latest->status, sizeof(latest->status), RTP_PASS_DONE);
   rtp_pass_update(latest);
   rtp_run_set_state(run->id, RTP_STATE_PROPOSAL_REVIEW, RTP_PHASE_PROPOSAL);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "action", "drafted");
   cJSON_AddStringToObject(resp, "state", RTP_STATE_PROPOSAL_REVIEW);
   cJSON_AddStringToObject(resp, "proposal_ref", run->proposal_ref);
   cJSON_AddStringToObject(
       resp, "note",
       "DRAFT skeleton stored as the proposal working artifact; entering proposal_review");
   return server_send_ok(conn, resp);
}

/* Unanswered-gate TTL (#47/#57): an awaiting-human *_pending gate older than the
 * configured TTL moves to abandoned with child-run stop — never an auto-pass,
 * and never applied to *_merge_pending. Returns 1 if it abandoned the run. */
static int maybe_ttl_abandon(int id, rtp_run_t *run, const config_t *cfg)
{
   if (cfg->roundtable_pipeline_gate_ttl_h <= 0)
      return 0;
   int gate_no = 0;
   if (strcmp(run->state, RTP_STATE_GATE1_PENDING) == 0)
      gate_no = 1;
   else if (strcmp(run->state, RTP_STATE_GATE2_PENDING) == 0)
      gate_no = 2;
   else
      return 0;
   if (!rtp_gate_age_exceeds_hours(id, gate_no, cfg->roundtable_pipeline_gate_ttl_h))
      return 0;
   stop_inflight(id, RTP_PHASE_PROPOSAL);
   stop_inflight(id, RTP_PHASE_IMPL);
   rtp_run_set_state(id, RTP_STATE_ABANDONED, NULL);
   rtp_run_get(id, run);
   return 1;
}

/* Chunked review submission (#28/#37): persist the whole origin, split it into
 * budget-sized chunks, submit a review pass per chunk (recording each span's
 * offset/len/hash, #39), then submit a whole-artifact synthesis pass whose
 * prompt is the actual concatenated selected-span text — a self-contained inline
 * unit the panel reviews without needing tools (#37). Omitted/over-budget spans
 * are recorded on the synthesis member so the aggregate blocks on them (#39). */
static int submit_chunked_review(server_conn_t *conn, rtp_run_t *run, const char *phase,
                                 const char *artifact, int budget_bytes)
{
   rtp_chunk_plan_t plan;
   rtp_chunk_plan(artifact, budget_bytes, &plan);
   int group = rtp_pass_max_group(run->id, phase) + 1;

   persist_origin(run, phase, artifact, plan.origin_hash);

   int submitted = 0;
   for (int i = 0; i < plan.count; i++)
   {
      char *chunk = (char *)malloc((size_t)plan.chunks[i].len + 1);
      if (!chunk)
         continue;
      memcpy(chunk, artifact + plan.chunks[i].offset, (size_t)plan.chunks[i].len);
      chunk[plan.chunks[i].len] = '\0';
      if (submit_chunk_pass(conn, run, phase, chunk, plan.chunks[i].hash, group, i, plan.count,
                            plan.chunks[i].offset, plan.chunks[i].len, 0, 0) > 0)
         submitted++;
      free(chunk);
   }

   /* whole-artifact synthesis member (chunk_index = -1): the orchestrator
    * RETRIEVES the selected spans from the retained origin and concatenates them
    * into a self-contained inline unit (#32/#37), bounded by the smallest panel
    * budget. */
   rtp_assembly_t asm_unit;
   rtp_assembly_build(&plan, budget_bytes, &asm_unit);
   size_t cap = (size_t)(budget_bytes > 0 ? budget_bytes : 4096) + 1024;
   char *synth = (char *)malloc(cap);
   int spass = -1;
   if (synth)
   {
      size_t pos = 0;
      pos += (size_t)snprintf(
          synth + pos, cap - pos,
          "Whole-artifact synthesis review (origin %s, %d chunks, %d span(s) included, %d "
          "omitted). Check cross-chunk invariants — a definition in one span used in another, a "
          "renamed reference, a missing section. Report any blocking issue spanning chunk "
          "boundaries.\n\n",
          plan.origin_hash, plan.count, asm_unit.selected_count, asm_unit.omitted_count);
      for (int i = 0; i < asm_unit.selected_count && pos < cap - 64; i++)
      {
         const rtp_chunk_t *c = &plan.chunks[asm_unit.selected[i]];
         pos += (size_t)snprintf(synth + pos, cap - pos, "--- span %d [%d..%d] ---\n", c->index,
                                 c->offset, c->offset + c->len);
         int n = c->len;
         if (pos + (size_t)n >= cap - 32)
            n = (int)(cap - 32 - pos);
         if (n > 0)
         {
            memcpy(synth + pos, artifact + c->offset, (size_t)n);
            pos += (size_t)n;
            synth[pos++] = '\n';
         }
      }
      synth[pos < cap ? pos : cap - 1] = '\0';
      spass = submit_chunk_pass(conn, run, phase, synth, plan.origin_hash, group, -1, plan.count, 0,
                                0, asm_unit.omitted_count,
                                (plan.over_budget || asm_unit.over_budget) ? 1 : 0);
      free(synth);
   }

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "pipeline_id", run->id);
   cJSON_AddStringToObject(resp, "action", "chunked_submitted");
   cJSON_AddStringToObject(resp, "phase", phase);
   cJSON_AddNumberToObject(resp, "chunk_group", group);
   cJSON_AddNumberToObject(resp, "chunks", plan.count);
   cJSON_AddNumberToObject(resp, "chunks_submitted", submitted);
   cJSON_AddBoolToObject(resp, "synthesis_submitted", spass > 0 ? 1 : 0);
   cJSON_AddNumberToObject(resp, "budget_bytes", budget_bytes);
   cJSON_AddBoolToObject(resp, "over_budget", plan.over_budget ? 1 : 0);
   cJSON_AddBoolToObject(resp, "truncated", plan.truncated ? 1 : 0);
   cJSON_AddNumberToObject(resp, "omitted_spans", asm_unit.omitted_count);
   cJSON_AddStringToObject(resp, "origin_ref",
                           strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->diff_ref : run->proposal_ref);
   return server_send_ok(conn, resp);
}

/* Decide the next action for an in-flight / captured chunked-review group. */
static int decide_chunked_group(server_conn_t *conn, rtp_run_t *run, const char *phase, int group)
{
   /* any member still in flight -> wait. */
   /* (a member pass is open with a pending current attempt) */
   rtp_group_agg_t agg;
   if (rtp_pass_group_agg(run->id, phase, group, &agg) != 0)
      return server_send_error(conn, "pipeline: could not aggregate chunk group", NULL);

   /* Each member is terminal once it is valid (done/synthesis_done) OR invalid;
    * `invalid` is a COUNT so two-or-more invalid members are all accounted for
    * and the group never gets stuck "waiting" (#4). */
   int members = agg.total + (agg.synthesis_present ? 1 : 0);
   int captured = agg.done + (agg.synthesis_done ? 1 : 0) + agg.invalid;
   if (captured < members)
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "action", "waiting");
      cJSON_AddStringToObject(resp, "note", "chunked review in flight");
      cJSON_AddNumberToObject(resp, "chunk_group", group);
      cJSON_AddNumberToObject(resp, "members", members);
      cJSON_AddNumberToObject(resp, "captured", captured);
      return server_send_ok(conn, resp);
   }

   int aggregate_done =
       rtp_chunk_aggregate_done(agg.total, agg.done, agg.synthesis_done, agg.invalid > 0);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "chunk_group", group);
   cJSON_AddNumberToObject(resp, "blocking", agg.blocking_count);
   cJSON_AddNumberToObject(resp, "invalid_members", agg.invalid);
   if (agg.invalid > 0)
   {
      cJSON_AddStringToObject(resp, "action", "escalate");
      cJSON_AddStringToObject(resp, "note",
                              "one or more chunk members produced invalid evidence; human "
                              "attention required");
      return server_send_ok(conn, resp);
   }
   if (aggregate_done && agg.blocking_count == 0)
   {
      int pr = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->impl_pr_number : run->proposal_pr_number;
      if (pr <= 0)
      {
         /* controller opens + records the PR when a head branch is known (#2). */
         if (run->head_branch[0] && open_and_record_pr(run, phase) == 0)
            pr = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run->impl_pr_number : run->proposal_pr_number;
         else
         {
            cJSON_AddStringToObject(resp, "action", "open_pr");
            cJSON_AddStringToObject(
                resp, "note",
                "chunked done-bar met; set head_branch + push, then advance to open the gate");
            return server_send_ok(conn, resp);
         }
      }
      int gate_no = strcmp(phase, RTP_PHASE_IMPL) == 0 ? 2 : 1;
      enter_gate(run->id, gate_no, pr);
      cJSON_AddStringToObject(resp, "action", "gate_pending");
      return server_send_ok(conn, resp);
   }
   if (!aggregate_done)
   {
      cJSON_AddStringToObject(resp, "action", "escalate");
      cJSON_AddStringToObject(
          resp, "note", "chunk coverage incomplete (a member failed/omitted); human review needed");
      return server_send_ok(conn, resp);
   }
   /* aggregate complete but blocking findings remain -> revise. */
   cJSON_AddStringToObject(resp, "action", "revise");
   cJSON_AddStringToObject(resp, "note",
                           "cross-chunk blocking findings remain; revise the artifact and advance "
                           "with the new version");
   return server_send_ok(conn, resp);
}

int handle_pipeline_advance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int id = (int)jo_num(req, "pipeline_id", 0);
   if (id <= 0)
      return server_send_error(conn, "usage: aimee pipeline advance <id> [--artifact <text>]",
                               NULL);
   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);
   if (is_terminal_state(run.state))
      return server_send_error(conn, "pipeline: terminal", NULL);

   /* unanswered-gate TTL (#47): abandon an over-age awaiting-human gate before
    * doing anything else. Never applies to *_merge_pending (#57). */
   {
      config_t tcfg;
      if (config_load(&tcfg) == 0 && maybe_ttl_abandon(id, &run, &tcfg))
         return server_send_error(conn, "pipeline: gate TTL exceeded; pipeline abandoned", NULL);
   }

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

   /* A chunked-review group is the active review unit: aggregate it, unless the
    * caller supplied a new artifact (a revise -> re-chunk into a fresh group). */
   if (have && latest.chunk_group > 0 && !(artifact && artifact[0]))
      return decide_chunked_group(conn, &run, phase, latest.chunk_group);

   /* If a pass is in flight (open + a pending current attempt), wait. */
   if (have && strcmp(latest.status, RTP_PASS_OPEN) == 0)
   {
      rtp_attempt_t a;
      if (rtp_attempt_current(latest.id, &a) == 0 && strcmp(a.capture_status, RTP_CAP_PENDING) == 0)
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

      /* DRAFT completion is NOT a review done-bar (#1): a valid skeleton is
       * stored as the proposal working artifact and transitions drafting ->
       * proposal_review; it never opens a PR/gate. */
      if (strcmp(latest.mode, RTP_MODE_DRAFT) == 0)
      {
         int maxa = cfg.roundtable_pipeline_max_attempts_per_pass > 0
                        ? cfg.roundtable_pipeline_max_attempts_per_pass
                        : 2;
         if (hav_a && a.envelope_valid)
            return draft_complete(conn, &run, &latest, &a);
         if (hav_a && a.attempt_no < maxa)
            return resubmit_same_pass(conn, &run, &latest);
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "action", "escalate");
         cJSON_AddStringToObject(
             resp, "note",
             "DRAFT did not produce a valid skeleton within the retry ceiling; human attention "
             "required");
         return server_send_ok(conn, resp);
      }

      rtp_envelope_t env;
      env_from_ledger(&latest, hav_a ? &a : NULL, &env);

      rtp_loop_cfg_t lc;
      load_loop_cfg(&cfg, &run, &lc);
      rtp_loop_state_t ls = {latest.pass_no, hav_a ? a.attempt_no : 1, phase_cost(&run, phase),
                             run.total_cost_usd, run.accepted_question_count};
      rtp_action_t act = rtp_loop_decide(&lc, &ls, &env);

      if (act == RTP_ACT_PASS)
      {
         /* mark pass done; open the gate (needs the PR opened first). */
         snprintf(latest.status, sizeof(latest.status), RTP_PASS_DONE);
         rtp_pass_update(&latest);
         int pr = strcmp(phase, RTP_PHASE_IMPL) == 0 ? run.impl_pr_number : run.proposal_pr_number;
         if (pr <= 0)
         {
            /* controller opens + records the PR via git_pr when a head branch is
             * known; otherwise it asks the agent to push a branch first (#2). */
            if (run.head_branch[0] && open_and_record_pr(&run, phase) == 0)
               pr =
                   strcmp(phase, RTP_PHASE_IMPL) == 0 ? run.impl_pr_number : run.proposal_pr_number;
            else
            {
               cJSON *resp = jo_ok();
               cJSON_AddStringToObject(resp, "action", "open_pr");
               cJSON_AddStringToObject(
                   resp, "note",
                   "done-bar met; set head_branch (pipeline.start) + push commits, then advance to "
                   "open the gate");
               return server_send_ok(conn, resp);
            }
         }
         int gate_no = strcmp(phase, RTP_PHASE_IMPL) == 0 ? 2 : 1;
         enter_gate(id, gate_no, pr);
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
         /* automatic: re-run the same pass id from the persisted artifact (#19). */
         return resubmit_same_pass(conn, &run, &latest);
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
         return server_send_error(
             conn, "pipeline: nothing to advance in this state without an artifact", NULL);
      rtp_run_get(id, &run);
   }
   /* REVIEW with no supplied artifact: the controller obtains it itself rather
    * than requiring the caller to re-provide it. For the proposal phase that is
    * the stored DRAFT skeleton / working proposal file (#1); for the PR phase it
    * is the captured diff via the workspace-aware git surface (#2). */
   char *captured_diff = NULL;
   if ((!artifact || !artifact[0]) && strcmp(mode, RTP_MODE_REVIEW) == 0)
   {
      if (strcmp(phase, RTP_PHASE_IMPL) == 0 && run.head_branch[0])
         captured_diff = capture_diff(&run);
      else if (strcmp(phase, RTP_PHASE_PROPOSAL) == 0 && run.proposal_ref[0])
         captured_diff = read_text_file(run.proposal_ref);
      if (captured_diff)
         artifact = captured_diff;
   }

   /* DRAFT with no artifact: build the prompt from the stored human idea + brief
    * focus, so the "idea -> DRAFT skeleton" flow works without the caller
    * supplying anything (#1/#9). The skeleton lands in the working file via the
    * author-revise loop; DRAFT is never expected to emit a full proposal. */
   char *draft_prompt = NULL;
   if ((!artifact || !artifact[0]) && strcmp(mode, RTP_MODE_DRAFT) == 0)
   {
      size_t cap = RTP_IDEA_LEN + RTP_BRIEF_LEN + 512;
      draft_prompt = (char *)malloc(cap);
      if (draft_prompt)
      {
         snprintf(draft_prompt, cap,
                  "Draft a concise PROPOSAL SKELETON (section outline + goal/scope, NOT a finished "
                  "proposal; the author-revise loop expands it) for this idea:\n\n%s\n\n%s",
                  run.idea, run.brief[0] ? run.brief : "");
         artifact = draft_prompt;
      }
   }

   if ((!artifact || !artifact[0]) && strcmp(mode, RTP_MODE_REVIEW) == 0)
   {
      free(captured_diff);
      return server_send_error(
          conn, "pipeline: review needs the artifact (--artifact <diff|proposal>)", NULL);
   }

   /* For a REVIEW pass, resolve the panel and chunk the artifact when it exceeds
    * the smallest participant's context budget (#28/#33/#37). DRAFT is always a
    * single skeleton pass (#9). */
   if (strcmp(mode, RTP_MODE_REVIEW) == 0 && artifact && artifact[0])
   {
      rtp_panel_t panel;
      int prc = resolve_panel(&cfg, &panel);
      if (prc < 0)
      {
         free(captured_diff);
         free(draft_prompt);
         return server_send_error(
             conn,
             "pipeline: a panel participant has an unknown context window and no fallback is "
             "set (roundtable.pipeline_unknown_context_tokens)",
             NULL);
      }
      int budget = chunk_budget_bytes(&panel);
      if (budget > 0 && rtp_chunk_needed(artifact, budget))
      {
         int r = submit_chunked_review(conn, &run, phase, artifact, budget);
         free(captured_diff);
         free(draft_prompt);
         return r;
      }
   }
   int r = submit_pass(conn, &run, phase, mode, artifact, artifact_hash);
   free(captured_diff);
   free(draft_prompt);
   return r;
}

/* ----------------------------------------------------------------- gate ---- */

/* Run the policy-aware merge for an approved gate, record full evidence into the
 * gate row, and advance on success. Shared by gate-pass and the *_merge_pending
 * crash-recovery reconcile (#56). Fills `resp`. */
static void execute_gate_merge(int id, rtp_run_t *run, rtp_gate_t *gate, int gate_no, cJSON *req,
                               cJSON *resp)
{
   /* Drift guard (#56): if the PR head moved away from the SHA the human
    * approved, the verdict no longer applies — stop for a fresh review rather
    * than merging unreviewed commits. (gh's --match-head-commit also refuses,
    * but we surface it explicitly and mark the evidence stale.) */
   if (gate->expected_head_sha[0])
   {
      char cur[RTP_HASH_LEN];
      pr_current_head(run, gate->pr_number, cur, sizeof(cur));
      if (cur[0] && strcmp(cur, gate->expected_head_sha) != 0)
      {
         const char *mp =
             gate_no == 2 ? RTP_STATE_GATE2_MERGE_PENDING : RTP_STATE_GATE1_MERGE_PENDING;
         cJSON_AddBoolToObject(resp, "merged", 0);
         cJSON_AddBoolToObject(resp, "head_drift", 1);
         cJSON_AddStringToObject(resp, "expected_head_sha", gate->expected_head_sha);
         cJSON_AddStringToObject(resp, "current_head_sha", cur);
         cJSON_AddStringToObject(resp, "state", mp);
         cJSON_AddStringToObject(
             resp, "note",
             "PR head drifted from the approved SHA; the gate verdict is stale. Re-review the new "
             "head (pipeline fail->re-review) before merging.");
         (void)run;
         return;
      }
   }

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
   if (merged && merge_sha[0])
      snprintf(gate->merge_sha, sizeof(gate->merge_sha), "%s", merge_sha);
   rtp_gate_update(gate);

   if (merged)
   {
      const char *next = gate_no == 2 ? RTP_STATE_DONE : RTP_STATE_IMPLEMENTING;
      rtp_run_set_state(id, next, RTP_PHASE_IMPL);
      /* reclaim the active admission slot for the implementing phase (the gate
       * may have parked it, #47/#48). gate2 -> done is terminal, no reclaim. */
      if (gate_no == 1)
      {
         rtp_run_t r2;
         if (rtp_run_get(id, &r2) == 0 && strcmp(r2.admission_class, RTP_ADMIT_ACTIVE) != 0)
         {
            snprintf(r2.admission_class, sizeof(r2.admission_class), RTP_ADMIT_ACTIVE);
            rtp_run_update(&r2);
         }
      }
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

   rtp_run_t run;
   if (rtp_run_get(id, &run) != 0)
      return server_send_error(conn, "pipeline: not found", NULL);

   /* Enforce the unanswered-gate TTL FIRST (#47), before authority/verdict: an
    * expired *_pending gate is abandoned and can never be passed/merged, even by
    * an authorized caller invoking pipeline.gate directly. */
   config_t gcfg;
   if (config_load(&gcfg) == 0 && maybe_ttl_abandon(id, &run, &gcfg))
      return server_send_error(conn, "pipeline: gate TTL exceeded; pipeline abandoned (not passed)",
                               NULL);

   /* Authority separation (#53): the driving agent (CAP_DELEGATE) must not be
    * able to resolve its own gate. v1 requires an enrolled local operator
    * principal that a delegate-driving session does not possess. */
   if (!gate_authorized(req))
      return server_send_error(
          conn, "pipeline: gate resolution requires an enrolled local operator principal", NULL);

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
