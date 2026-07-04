/* wfe_engine.c: the workflow execution engine (server-side, DB1-backed). */
#include "wfe_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_home.h"
#include "wfe_store.h"

/* Loop-back safety bound (config-overridable in W6). */
#define WFE_MAX_ATTEMPTS 20

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
   return wfe_def_load_file(path, err, errlen);
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
    * work item outside it (a half-applied step would corrupt the ledger). --- */
   if (db1_lifecycle_txn_begin() != 0)
   {
      snprintf(err, errlen, "could not begin advance transaction");
      wfe_def_free(def);
      return -1;
   }

   /* Every state-mutating DB write below is checked; on ANY failure we roll the
    * whole step back (goto txn_fail) so the work item is never left advanced but
    * un-parked, or otherwise half-applied. (Audit/counter writes are
    * non-corrupting and left unchecked.) */
#define WFE_CKW(call)                                                                              \
   do                                                                                              \
   {                                                                                               \
      if ((call) != 0)                                                                             \
         goto txn_fail;                                                                            \
   } while (0)

   /* cost pre-flight (estimate is 0 for stubs; executors report actuals). */
   if (wi.work_item_max_cost_usd > 0 && wi.cum_cost_usd >= wi.work_item_max_cost_usd)
   {
      WFE_CKW(db1_work_item_set_pause(work_item_id, "budget_exceeded", node->id));
      db1_lifecycle_event_add(work_item_id, node->id, "pause", "engine", "budget_exceeded", "", 0);
      db1_lifecycle_txn_commit();
      out->last_status = WFE_STEP_PENDING;
      out->pause_reason = WFE_PAUSE_BUDGET_EXCEEDED;
      snprintf(out->next_stage, sizeof out->next_stage, "%s", node->id);
      wfe_def_free(def);
      return 0;
   }

   wfe_ctx ctx = {work_item_id, def, node, &wi};
   wfe_step_result_t r = fn(&ctx, node);
   out->last_status = r.status;
   out->failure_class = r.failure_class;
   out->failure_has_new_input = r.failure_has_new_input;
   if (r.cost_usd > 0)
      WFE_CKW(db1_work_item_add_cost(work_item_id, r.cost_usd));
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
      WFE_CKW(db1_work_item_set_pause(work_item_id, pr, node->id));
      db1_lifecycle_event_add(work_item_id, node->id, "pause", "engine", pr, r.content_hash,
                              r.cost_usd);
      out->pause_reason = r.pause_reason;
      snprintf(out->next_stage, sizeof out->next_stage, "%s", node->id);
   }
   else if (r.status == WFE_STEP_FAILED)
   {
      /* Phase-C failure taxonomy: derive the pause reason from the failure class so
       * the run loop + UI can distinguish a terminal reject from a park-for-human
       * from a stuck (transient without new input). Park with a NON-empty reason so
       * the next advance treats it as parked (empty reads as "not parked"). */
      wfe_failure_disposition_t disp =
          wfe_failure_disposition(r.failure_class, r.failure_has_new_input);
      const char *reason = "failed"; /* terminal-reject (refusal/permanent/corruption) */
      const char *detail = "terminal";
      if (disp == WFE_FDISP_PARK_HUMAN)
      {
         reason = "pending_human"; /* degraded / budget / forge -> human resumes */
         detail = "park_human";
      }
      else if (disp == WFE_FDISP_PARK_STUCK)
      {
         reason = "stuck"; /* transient without new input -> no spin (stuck-skip guard) */
         detail = "park_stuck";
      }
      WFE_CKW(db1_work_item_set_pause(work_item_id, reason, node->id));
      db1_lifecycle_event_add(work_item_id, node->id, "failed", "engine", detail, "", r.cost_usd);
   }
   else
   {
      const char *next = next_stage_for(node, r.status);
      if (r.status == WFE_STEP_ADVANCED && (!next || !next[0]))
      {
         /* terminal node reached */
         WFE_CKW(db1_work_item_set_terminal(work_item_id, "accepted"));
         db1_lifecycle_event_add(work_item_id, node->id, "terminal", "engine", "accepted",
                                 r.content_hash, r.cost_usd);
         out->terminal = 1;
         snprintf(out->state, sizeof out->state, "accepted");
      }
      else if (!next || !next[0])
      {
         snprintf(err, errlen, "node '%s' looped with no on_fail edge", node->id);
         db1_lifecycle_txn_rollback();
         wfe_def_free(def);
         return -1;
      }
      else
      {
         if (r.status == WFE_STEP_LOOPED)
         {
            int att = db1_stage_attempt_inc(work_item_id, next);
            if (att >= WFE_MAX_ATTEMPTS)
            {
               WFE_CKW(db1_work_item_set_pause(work_item_id, "pending_human", next));
               db1_lifecycle_event_add(work_item_id, node->id, "pause", "engine", "max_attempts",
                                       "", r.cost_usd);
               out->last_status = WFE_STEP_PENDING;
               out->pause_reason = WFE_PAUSE_PENDING_HUMAN;
               snprintf(out->next_stage, sizeof out->next_stage, "%s", next);
               db1_lifecycle_txn_commit();
               wfe_def_free(def);
               return 0;
            }
         }
         WFE_CKW(db1_work_item_set_stage(work_item_id, next, r.content_hash));
         /* pr.open carries the opened PR ref in content_hash; persist it durably so
          * the later gate.ci / check.mergeable / merge blocks resolve the real PR
          * (full-autonomous-development Phase A). Rides this atomic txn. */
         if (node->block == WFE_BLK_PR_OPEN && r.status == WFE_STEP_ADVANCED && r.content_hash[0])
            WFE_CKW(db1_work_item_set_pr_ref(work_item_id, r.content_hash));
         db1_lifecycle_event_add(work_item_id, node->id,
                                 r.status == WFE_STEP_LOOPED ? "loop" : "advance", "engine", next,
                                 r.content_hash, r.cost_usd);
         snprintf(out->next_stage, sizeof out->next_stage, "%s", next);
         if (over_budget)
         {
            /* the step advanced but its cost crossed the cap: park before the
             * next stage so a human must resume --budget-bump. */
            WFE_CKW(db1_work_item_set_pause(work_item_id, "budget_exceeded", next));
            db1_lifecycle_event_add(work_item_id, next, "pause", "engine", "budget_exceeded", "",
                                    0);
            out->last_status = WFE_STEP_PENDING;
            out->pause_reason = WFE_PAUSE_BUDGET_EXCEEDED;
         }
      }
   }

   db1_lifecycle_txn_commit();
   wfe_def_free(def);
   return 0;

txn_fail:
   db1_lifecycle_txn_rollback();
   snprintf(err, errlen, "advance: a state write failed; rolled back");
   wfe_def_free(def);
   return -1;
#undef WFE_CKW
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
