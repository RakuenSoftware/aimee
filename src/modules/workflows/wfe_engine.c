/* wfe_engine.c: the workflow execution engine (server-side, DB1-backed). */
#include "wfe_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_home.h"
#include "artifact_trust.h"
#include "db1_client/wfe_store.h"

struct wfe_ctx
{
   const char *work_item_id;
   const wfe_def_t *def;
   const wfe_node_t *node;
   const db1_work_item_t *wi;
};

const char *wfe_ctx_work_item(const wfe_ctx *c)
{
   return c ? c->work_item_id : NULL;
}
const wfe_def_t *wfe_ctx_def(const wfe_ctx *c)
{
   return c ? c->def : NULL;
}
const wfe_node_t *wfe_ctx_node(const wfe_ctx *c)
{
   return c ? c->node : NULL;
}
const char *wfe_ctx_repo(const wfe_ctx *c)
{
   return (c && c->wi) ? c->wi->repo : NULL;
}
const char *wfe_ctx_proposal_path(const wfe_ctx *c)
{
   return (c && c->wi) ? c->wi->proposal_path : NULL;
}
const char *wfe_ctx_pr_ref(const wfe_ctx *c)
{
   /* "" (not NULL) when unloaded, mirroring how the resolver tolerates an absent
    * work item; wi is loaded by the engine driver before any executor runs. */
   return (c && c->wi) ? c->wi->pr_ref : "";
}
const char *wfe_ctx_worktree(const wfe_ctx *c)
{
   /* "" until the per-work-item worktree has been created (F2); executors fall
    * back to the shared repo dir while empty. */
   return (c && c->wi) ? c->wi->worktree : "";
}

wfe_def_t *wfe_load_workflow(const char *name, char *err, size_t errlen)
{
   if (!name || !name[0])
   {
      snprintf(err, errlen, "empty workflow name");
      return NULL;
   }
   char path[1100];
   snprintf(path, sizeof path, "%s/workflows/%s.yaml", aimee_home(), name);
   char *verified = NULL;
   size_t verified_len = 0;
   if (artifact_trust_read_file("workflow", name, path, 1024u * 1024u, &verified, &verified_len,
                                NULL, err, errlen) != 0)
      return NULL;
   (void)verified_len;
   wfe_def_t *def = wfe_def_parse(verified, err, errlen);
   free(verified);
   return def;
}

/* Server-minted, non-forgeable id: wi_ + 16 random bytes (hex). */
static int mint_work_item_id(char out[80])
{
   unsigned char rnd[16];
   FILE *f = fopen("/dev/urandom", "rb");
   if (f)
   {
      size_t got = fread(rnd, 1, sizeof rnd, f);
      fclose(f);
      if (got != sizeof rnd)
         f = NULL;
   }
   if (!f)
      return -1;
   static const char hx[] = "0123456789abcdef";
   char hex[33];
   for (int i = 0; i < 16; i++)
   {
      hex[i * 2] = hx[rnd[i] >> 4];
      hex[i * 2 + 1] = hx[rnd[i] & 0xf];
   }
   hex[32] = '\0';
   snprintf(out, 80, "wi_%s", hex);
   return 0;
}

/* Minimal repo URL normalization: drop trailing '/', strip a trailing ".git". */
static void normalize_repo(const char *in, char *out, size_t cap)
{
   if (!in)
   {
      out[0] = '\0';
      return;
   }
   snprintf(out, cap, "%s", in);
   size_t n = strlen(out);
   while (n > 0 && out[n - 1] == '/')
      out[--n] = '\0';
   if (n >= 4 && strcmp(out + n - 4, ".git") == 0)
      out[n - 4] = '\0';
}

int wfe_work_item_resolve(const char *workflow_name, const char *repo, char out_name[64],
                          char out_ver[65], char out_start[64], char out_repo[512], char out_id[80],
                          char *err, size_t errlen)
{
   char ferr[256];
   wfe_def_t *def = wfe_load_workflow(workflow_name, ferr, sizeof ferr);
   if (!def)
   {
      snprintf(err, errlen, "load workflow '%s': %s", workflow_name ? workflow_name : "", ferr);
      return -1;
   }
   if (wfe_def_validate(def, ferr, sizeof ferr) != 0)
   {
      snprintf(err, errlen, "workflow invalid: %s", ferr);
      wfe_def_free(def);
      return -1;
   }
   /* Reject (don't silently truncate) a name/start that won't fit the DB columns —
    * the row's workflow_name/current_stage must round-trip exactly. */
   if (strlen(def->name) >= 64 || strlen(def->start) >= 64)
   {
      snprintf(err, errlen, "workflow name/start stage too long");
      wfe_def_free(def);
      return -1;
   }
   out_ver[0] = '\0';
   wfe_def_compute_version(def, out_ver);
   snprintf(out_name, 64, "%s", def->name);
   snprintf(out_start, 64, "%s", def->start);
   normalize_repo(repo, out_repo, 512);
   wfe_def_free(def);
   if (mint_work_item_id(out_id) != 0)
   {
      snprintf(err, errlen, "could not mint work-item id");
      return -1;
   }
   return 0;
}

int wfe_work_item_create(const char *workflow_name, const char *repo, const char *proposal_path,
                         const char *mode, char out_id[80], char *err, size_t errlen)
{
   char name[64], ver[65], start[64], norm[512];
   if (wfe_work_item_resolve(workflow_name, repo, name, ver, start, norm, out_id, err, errlen) != 0)
      return -1;
   if (db1_work_item_create(out_id, norm, proposal_path, name, ver, start, mode) != 0)
   {
      snprintf(err, errlen, "create work item failed (duplicate repo+path?)");
      return -1;
   }
   db1_lifecycle_event_add(out_id, start, "create", "user", workflow_name, ver, 0);
   return 0;
}

/* Choose the next stage from a node + the executor's status. */
static const char *next_stage_for(const wfe_node_t *n, wfe_step_status_t st)
{
   if (st == WFE_STEP_ADVANCED)
      return n->on_pass[0] ? n->on_pass : n->next;
   if (st == WFE_STEP_LOOPED)
      return n->on_fail;
   return "";
}

/* Start an outcome with the two fields every one of them carries. The rest are
   set by whichever branch the engine decides on, and an unset field is empty --
   which is what the store reads as "no cost", "no PR ref", "did not park". */
static void wfe_outcome_init(db1_work_item_outcome_t *o, const char *work_item_id,
                             const char *node_id)
{
   memset(o, 0, sizeof(*o));
   snprintf(o->work_item_id, sizeof o->work_item_id, "%s", work_item_id ? work_item_id : "");
   snprintf(o->node_id, sizeof o->node_id, "%s", node_id ? node_id : "");
}

static const char *pause_name(wfe_pause_reason_t r)
{
   switch (r)
   {
   case WFE_PAUSE_PENDING_HUMAN:
      return "pending_human";
   case WFE_PAUSE_PANEL_DEGRADED:
      return "panel_degraded";
   case WFE_PAUSE_BUDGET_EXCEEDED:
      return "budget_exceeded";
   case WFE_PAUSE_PANEL_UNREACHABLE:
      return "panel_unreachable";
   case WFE_PAUSE_CI_PENDING:
      return "ci_pending";
   case WFE_PAUSE_MERGE_PENDING:
      return "merge_pending";
   case WFE_PAUSE_TURN_CAP:
      return "turn_cap_exceeded";
   case WFE_PAUSE_WALL_CAP:
      return "wall_cap_exceeded";
   case WFE_PAUSE_SLICES_RUNNING:
      return "slices_running";
   default:
      return "";
   }
}

static wfe_pause_reason_t pause_from_name(const char *s)
{
   if (!s)
      return WFE_PAUSE_NONE;
   if (strcmp(s, "pending_human") == 0)
      return WFE_PAUSE_PENDING_HUMAN;
   if (strcmp(s, "panel_degraded") == 0)
      return WFE_PAUSE_PANEL_DEGRADED;
   if (strcmp(s, "budget_exceeded") == 0)
      return WFE_PAUSE_BUDGET_EXCEEDED;
   if (strcmp(s, "panel_unreachable") == 0)
      return WFE_PAUSE_PANEL_UNREACHABLE;
   if (strcmp(s, "ci_pending") == 0)
      return WFE_PAUSE_CI_PENDING;
   if (strcmp(s, "merge_pending") == 0)
      return WFE_PAUSE_MERGE_PENDING;
   if (strcmp(s, "turn_cap_exceeded") == 0)
      return WFE_PAUSE_TURN_CAP;
   if (strcmp(s, "wall_cap_exceeded") == 0)
      return WFE_PAUSE_WALL_CAP;
   if (strcmp(s, "slices_running") == 0)
      return WFE_PAUSE_SLICES_RUNNING;
   return WFE_PAUSE_NONE;
}

int wfe_engine_advance(const char *work_item_id, wfe_advance_result_t *out, char *err,
                       size_t errlen)
{
   wfe_advance_result_t local;
   if (!out)
      out = &local;
   memset(out, 0, sizeof *out);
   if (err && errlen)
      err[0] = '\0';

   db1_work_item_t wi;
   int found = db1_work_item_get(work_item_id, &wi);
   if (found != 1)
   {
      snprintf(err, errlen, "unknown work item '%s'", work_item_id ? work_item_id : "");
      return -1;
   }
   snprintf(out->state, sizeof out->state, "%s", wi.state);
   if (strcmp(wi.state, "active") != 0)
   {
      out->terminal = 1; /* already terminal */
      return 0;
   }
   if (wi.pause_reason[0])
   {
      /* parked; caller must resume explicitly */
      out->last_status = WFE_STEP_PENDING;
      out->pause_reason = pause_from_name(wi.pause_reason);
      snprintf(out->stage, sizeof out->stage, "%s", wi.current_stage);
      snprintf(out->next_stage, sizeof out->next_stage, "%s", wi.current_stage);
      return 0;
   }

   char ferr[256];
   wfe_def_t *def = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
   if (!def)
   {
      snprintf(err, errlen, "load workflow: %s", ferr);
      return -1;
   }
   const wfe_node_t *node = wfe_def_node(def, wi.current_stage);
   if (!node)
   {
      snprintf(err, errlen, "current stage '%s' not in workflow", wi.current_stage);
      wfe_def_free(def);
      return -1;
   }
   snprintf(out->stage, sizeof out->stage, "%s", node->id);

   wfe_block_exec_fn fn = wfe_lookup_block_executor(node->block);
   if (!fn)
   {
      snprintf(err, errlen, "no executor registered for block '%s'", wfe_block_name(node->block));
      wfe_def_free(def);
      return -1;
   }

   /* --- atomic critical section: cost reservation + step + state update.
    * Fail closed: if we cannot open the transaction we must NOT mutate the
    * work item outside it (a half-applied step would corrupt the ledger).
    *
    * SCOPE: the transaction covers ONLY the post-executor state writes — it
    * must NEVER span the executor itself. A live executor runs delegates for
    * MINUTES; holding BEGIN IMMEDIATE on the shared db1 connection for that
    * long makes every concurrent write on the connection (operator
    * pause/resume, other items' events, trigger state) silently JOIN the open
    * transaction: invisible to other readers until this step commits, and
    * DISCARDED WHOLESALE if it rolls back (observed live: operator resumes
    * that returned 200 yet never persisted, and a WAL frozen for the length
    * of an implement stage). --- */

   /* cost pre-flight (estimate is 0 for stubs; executors report actuals).
    * Its pause+event pair rides a short transaction of its own. */
   if (wi.work_item_max_cost_usd > 0 && wi.cum_cost_usd >= wi.work_item_max_cost_usd)
   {
      db1_work_item_outcome_t pre;
      wfe_outcome_init(&pre, work_item_id, node->id);
      pre.disposition = DB1_WORK_ITEM_OUTCOME_PAUSE;
      snprintf(pre.pause_reason, sizeof pre.pause_reason, "%s", "budget_exceeded");
      snprintf(pre.pause_stage, sizeof pre.pause_stage, "%s", node->id);
      snprintf(pre.event_kind, sizeof pre.event_kind, "%s", "pause");
      snprintf(pre.event_detail, sizeof pre.event_detail, "%s", "budget_exceeded");
      if (db1_work_item_record_outcome(&pre) != 0)
      {
         snprintf(err, errlen, "could not record the budget pause");
         wfe_def_free(def);
         return -1;
      }
      out->last_status = WFE_STEP_PENDING;
      out->pause_reason = WFE_PAUSE_BUDGET_EXCEEDED;
      snprintf(out->next_stage, sizeof out->next_stage, "%s", node->id);
      wfe_def_free(def);
      return 0;
   }

   /* The executor runs OUTSIDE any transaction (see scope note above). Its own
    * incidental db1 writes (attempt counters, loop audit events) auto-commit
    * individually, which is strictly better than riding a step txn that a later
    * write failure would erase. */
   wfe_ctx ctx = {work_item_id, def, node, &wi};
   wfe_step_result_t r = fn(&ctx, node);
   out->last_status = r.status;
   out->failure_class = r.failure_class;
   out->failure_has_new_input = r.failure_has_new_input;

   /* --- the step's outcome, decided here and applied in one call. ---
    *
    * This used to be BEGIN, up to five writes, COMMIT, with a goto rolling the
    * lot back on any failure. That cannot cross a module boundary: the store
    * would have to hold an open transaction between requests, on a shared
    * connection behind the gate mutex, and a caller that died mid-section would
    * block every other writer. The decision below is unchanged and still the
    * engine's; only the applying moved into db1_work_item_record_outcome, which
    * keeps the same all-or-nothing guarantee on the far side.
    *
    * One thing did change. db1_stage_attempt_inc is a counter whose NEW value
    * picks the branch, so it cannot be part of an outcome the engine has not
    * decided yet -- it now runs before, on its own. If the outcome write then
    * fails, the attempt stays counted where it used to roll back. That is
    * bounded and one-directional: a node's loop budget is at most one smaller
    * on the retry, so an exhausted cap escalates sooner and never later, which
    * is the safe side of a loop cap to be wrong on. */
   db1_work_item_outcome_t oc;
   wfe_outcome_init(&oc, work_item_id, node->id);
   if (r.cost_usd > 0)
      oc.cost_usd = r.cost_usd;

   /* post-executor budget re-check: a single step's cost can push over the cap,
    * which the pre-flight (before the executor) cannot see. Bound overshoot to
    * one step by parking now. */
   int over_budget =
       (wi.work_item_max_cost_usd > 0 &&
        wi.cum_cost_usd + (r.cost_usd > 0 ? r.cost_usd : 0) >= wi.work_item_max_cost_usd);

   if (r.status == WFE_STEP_PENDING)
   {
      /* a PENDING step MUST carry a reason; never write an empty pause_reason
       * (an empty reason reads as "not parked" on the next advance -> infinite
       * re-run of a gate). Fail closed to "unspecified". */
      const char *pr = pause_name(r.pause_reason);
      if (!pr[0])
         pr = "unspecified";
      /* An AUTONOMOUS run has no operator to satisfy a pending_human wait, so a
       * block parking pending_human at anything OTHER than a real human gate
       * (gate.human) is a terminal dead-end — a failed slice, an unrecoverable
       * escalation. Parking would leave the run 'active' forever (the stale-park
       * reaper never reaps a human wait), piling up zombie runs. Terminate it
       * (abandoned) with an audit trail instead. A real gate.human still parks (its
       * whole purpose is the operator decision); interactive runs are unchanged. */
      if (r.pause_reason == WFE_PAUSE_PENDING_HUMAN && strcmp(wi.mode, "autonomous") == 0 &&
          node->block != WFE_BLK_GATE_HUMAN)
      {
         oc.disposition = DB1_WORK_ITEM_OUTCOME_TERMINAL;
         snprintf(oc.state, sizeof oc.state, "%s", "abandoned");
         oc.abandon_children = 1;
         snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "terminal");
         snprintf(oc.event_detail, sizeof oc.event_detail, "%s",
                  "abandoned: autonomous dead-end (no human to escalate to)");
         out->terminal = 1;
         snprintf(out->state, sizeof out->state, "abandoned");
      }
      else
      {
         oc.disposition = DB1_WORK_ITEM_OUTCOME_PAUSE;
         snprintf(oc.pause_reason, sizeof oc.pause_reason, "%s", pr);
         snprintf(oc.pause_stage, sizeof oc.pause_stage, "%s", node->id);
         snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "pause");
         snprintf(oc.event_detail, sizeof oc.event_detail, "%s", pr);
         snprintf(oc.event_hash, sizeof oc.event_hash, "%s", r.content_hash);
         out->pause_reason = r.pause_reason;
         snprintf(out->next_stage, sizeof out->next_stage, "%s", node->id);
      }
   }
   else if (r.status == WFE_STEP_FAILED)
   {
      /* Phase-C failure taxonomy: derive the pause reason from the failure class,
       * REUSING the existing reason strings (budget_exceeded / panel_degraded /
       * stuck / pending_human / failed) so the scheduler + UI need no new vocabulary
       * (the run loop already skips a 'stuck' item; park_budget already uses
       * 'budget_exceeded'). The lifecycle-event detail stays EMPTY for the legacy
       * terminal case (byte-inert for any consumer of the old wfe_step_failed()
       * events); only the new dispositions carry a tag. A retryable failure must be
       * returned as LOOPED, never FAILED — a FAILED that claims retry can't loop from
       * here, so it parks 'stuck' (visible, no silent terminal-stop). */
      const char *reason = "failed"; /* REFUSAL / PERMANENT / CORRUPTION / NONE -> terminal */
      const char *detail = "";       /* legacy-inert for terminal */
      switch (r.failure_class)
      {
      case WFE_FAIL_BUDGET:
         reason = "budget_exceeded";
         detail = "budget";
         break;
      case WFE_FAIL_DEGRADED:
         reason = "panel_degraded";
         detail = "degraded";
         break;
      case WFE_FAIL_FORGE:
         reason = "pending_human";
         detail = "forge";
         break;
      case WFE_FAIL_TRANSIENT:
         reason = "stuck";
         detail = r.failure_has_new_input ? "retry_expected_looped" : "park_stuck";
         break;
      default:
         break; /* terminal-reject: reason "failed", detail "" (inert) */
      }
      oc.disposition = DB1_WORK_ITEM_OUTCOME_PAUSE;
      snprintf(oc.pause_reason, sizeof oc.pause_reason, "%s", reason);
      snprintf(oc.pause_stage, sizeof oc.pause_stage, "%s", node->id);
      snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "failed");
      snprintf(oc.event_detail, sizeof oc.event_detail, "%s", detail);
   }
   else
   {
      const char *next = next_stage_for(node, r.status);
      if (r.status == WFE_STEP_ADVANCED && (!next || !next[0]))
      {
         /* terminal node reached */
         oc.disposition = DB1_WORK_ITEM_OUTCOME_TERMINAL;
         snprintf(oc.state, sizeof oc.state, "%s", "accepted");
         snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "terminal");
         snprintf(oc.event_detail, sizeof oc.event_detail, "%s", "accepted");
         snprintf(oc.event_hash, sizeof oc.event_hash, "%s", r.content_hash);
         out->terminal = 1;
         snprintf(out->state, sizeof out->state, "accepted");
      }
      else if (!next || !next[0])
      {
         snprintf(err, errlen, "node '%s' looped with no on_fail edge", node->id);
         wfe_def_free(def);
         return -1;
      }
      else
      {
         int capped = 0;
         if (r.status == WFE_STEP_LOOPED)
         {
            /* Generic per-node loop cap. Keyed on the GATE node (node->id) so
             * distinct gates looping to a shared target keep independent budgets
             * and the review block's own read (also node->id) agrees. */
            int att = db1_stage_attempt_inc(work_item_id, node->id);
            if (att >= wfe_node_max_iters(node))
            {
               wfe_on_max_t pol = wfe_node_on_max(node);
               capped = 1;
               if (pol == WFE_ON_MAX_FAIL)
               {
                  oc.disposition = DB1_WORK_ITEM_OUTCOME_TERMINAL;
                  snprintf(oc.state, sizeof oc.state, "%s", "rejected");
                  snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "terminal");
                  snprintf(oc.event_detail, sizeof oc.event_detail, "%s",
                           "rejected: max_iters reached");
                  out->terminal = 1;
                  snprintf(out->state, sizeof out->state, "rejected");
               }
               else if (pol == WFE_ON_MAX_PASS)
               {
                  /* Proceed forward as if the node advanced; the validator
                   * guarantees on_max:pass carries an on_pass/next forward edge. */
                  const char *fwd = node->on_pass[0] ? node->on_pass : node->next;
                  oc.disposition = DB1_WORK_ITEM_OUTCOME_ADVANCE;
                  snprintf(oc.next_stage, sizeof oc.next_stage, "%s", fwd);
                  snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "forced_pass");
                  snprintf(oc.event_detail, sizeof oc.event_detail, "%s", fwd);
                  snprintf(oc.event_hash, sizeof oc.event_hash, "%s", r.content_hash);
                  out->last_status = WFE_STEP_ADVANCED;
                  snprintf(out->next_stage, sizeof out->next_stage, "%s", fwd);
               }
               /* WFE_ON_MAX_HUMAN (default): pause for a human — but an AUTONOMOUS
                * run has no human to escalate to, so an exhausted loop cap is a
                * terminal dead-end, not a wait. Parking pending_human would leave it
                * 'active' forever (the reaper never reaps a human wait) — the very
                * zombie-accumulation this avoids. Terminate it (abandoned) with an
                * audit trail; an interactive run still parks for its operator. */
               else if (strcmp(wi.mode, "autonomous") == 0)
               {
                  oc.disposition = DB1_WORK_ITEM_OUTCOME_TERMINAL;
                  snprintf(oc.state, sizeof oc.state, "%s", "abandoned");
                  oc.abandon_children = 1;
                  snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "terminal");
                  snprintf(oc.event_detail, sizeof oc.event_detail, "%s",
                           "abandoned: max_iters reached (autonomous, no human to escalate to)");
                  out->terminal = 1;
                  snprintf(out->state, sizeof out->state, "abandoned");
               }
               else
               {
                  oc.disposition = DB1_WORK_ITEM_OUTCOME_PAUSE;
                  snprintf(oc.pause_reason, sizeof oc.pause_reason, "%s", "pending_human");
                  snprintf(oc.pause_stage, sizeof oc.pause_stage, "%s", next);
                  snprintf(oc.event_kind, sizeof oc.event_kind, "%s", "pause");
                  snprintf(oc.event_detail, sizeof oc.event_detail, "%s", "max_iters");
                  out->last_status = WFE_STEP_PENDING;
                  out->pause_reason = WFE_PAUSE_PENDING_HUMAN;
                  snprintf(out->next_stage, sizeof out->next_stage, "%s", next);
               }
            }
         }
         if (!capped)
         {
            oc.disposition = DB1_WORK_ITEM_OUTCOME_ADVANCE;
            snprintf(oc.next_stage, sizeof oc.next_stage, "%s", next);
            /* pr.open carries the opened PR ref in content_hash; persist it durably so
             * the later gate.ci / check.mergeable / merge blocks resolve the real PR
             * (full-autonomous-development Phase A). Rides this atomic outcome. */
            if (node->block == WFE_BLK_PR_OPEN && r.status == WFE_STEP_ADVANCED &&
                r.content_hash[0])
               snprintf(oc.pr_ref, sizeof oc.pr_ref, "%s", r.content_hash);
            snprintf(oc.event_kind, sizeof oc.event_kind, "%s",
                     r.status == WFE_STEP_LOOPED ? "loop" : "advance");
            snprintf(oc.event_detail, sizeof oc.event_detail, "%s", next);
            snprintf(oc.event_hash, sizeof oc.event_hash, "%s", r.content_hash);
            snprintf(out->next_stage, sizeof out->next_stage, "%s", next);
            if (over_budget)
            {
               /* the step advanced but its cost crossed the cap: park before the
                * next stage so a human must resume --budget-bump. */
               snprintf(oc.park_reason, sizeof oc.park_reason, "%s", "budget_exceeded");
               out->last_status = WFE_STEP_PENDING;
               out->pause_reason = WFE_PAUSE_BUDGET_EXCEEDED;
            }
         }
      }
   }

   if (db1_work_item_record_outcome(&oc) != 0)
   {
      snprintf(err, errlen, "advance: the step outcome was not recorded; nothing was applied");
      wfe_def_free(def);
      return -1;
   }
   wfe_def_free(def);
   return 0;
}

int wfe_engine_run(const char *work_item_id, char *err, size_t errlen)
{
   for (int i = 0; i < 10000; i++)
   {
      wfe_advance_result_t r;
      if (wfe_engine_advance(work_item_id, &r, err, errlen) != 0)
         return -1;
      if (r.terminal)
         return 0;
      if (r.last_status == WFE_STEP_PENDING || r.last_status == WFE_STEP_FAILED)
         return 0;
   }
   snprintf(err, errlen, "engine run exceeded step bound (cycle without terminal?)");
   return -1;
}

/* ---- built-in stub executors (W2): every block advances ---- */
static wfe_step_result_t stub_exec(wfe_ctx *ctx, const wfe_node_t *node)
{
   (void)ctx;
   char handle[80], hash[65];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   wfe_sha256_hex(node->id, strlen(node->id), hash);
   return wfe_step_advanced(handle, hash, 0.0);
}

void wfe_register_stub_executors(void)
{
   for (wfe_block_type_t t = WFE_BLK_UNKNOWN + 1; t < WFE_BLK__COUNT; t++)
      wfe_register_block_executor(t, stub_exec);
}
