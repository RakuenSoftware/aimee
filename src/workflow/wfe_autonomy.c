/* wfe_autonomy.c: the autonomy driver + human-only gate-override. */
#include "wfe_autonomy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "wfe_store.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

/* On a terminal run, tear down the per-work-item worktree (F2) + clear its column,
 * so finished runs don't leak worktrees/branches. A non-autonomous terminal path
 * (e.g. an API gate reject) is caught by the orphan-sweep (GA). */
static void wfe_autonomy_cleanup_worktree(const char *work_item_id)
{
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1 || !wi.worktree[0])
      return;
   const char *rl = getenv("AIMEE_WORKFLOW_REPO");
   if (wfe_worktree_cleanup(wi.worktree, (rl && rl[0]) ? rl : ".") == 0)
      db1_work_item_set_worktree(work_item_id, "");
}

/* A positive long from an env var, else the default (a safety rail must never be
 * silently disabled by a malformed override). Rejects junk, non-positive, AND
 * strtol overflow (ERANGE -> LONG_MAX would otherwise pass the n>0 test). */
static long wfe_env_long(const char *name, long def)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return def;
   errno = 0;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (errno == ERANGE || !end || *end != '\0' || n <= 0)
      return def;
   return n;
}

/* Park an active, not-yet-parked autonomous work item with budget_exceeded and
 * audit the reason. Returns 0 on a clean park, 1 if it was already parked (still
 * audits the breach), -1 if the row could not be read (caller hard-stops — a
 * safety rail must not silently no-op on a read failure). */
static int wfe_autonomy_park_budget(const char *work_item_id, const char *why)
{
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1)
      return -1;
   if (wi.pause_reason[0])
   {
      /* already parked (e.g. at a gate): record the breach for audit, don't
       * overwrite the existing pause reason. */
      db1_lifecycle_event_add(work_item_id, wi.current_stage, "budget", "engine", why, "", 0);
      return 1;
   }
   db1_work_item_set_pause(work_item_id, "budget_exceeded", wi.current_stage);
   db1_lifecycle_event_add(work_item_id, wi.current_stage, "pause", "engine", why, "", 0);
   return 0;
}

/* Park an active autonomous work item 'stuck' after an UNRECOVERABLE advance
 * failure — its current_stage is not a node in the workflow (an orphaned stage
 * after a rename), the stage's block has no executor, or a looped node has no
 * on_fail edge to retry into. Such a failure never self-resolves, so without
 * parking the scheduler retry-fails the item on every backstop sweep while the
 * UI still shows it "running" (the symptom that surfaced this). Mirrors
 * wfe_autonomy_park_budget: audits the reason once and never overwrites an
 * existing pause. Returns 0 on a clean park, 1 if already parked (no re-audit, so
 * a re-sweep can't spam the event log), -1 if the row could not be read. */
static int wfe_autonomy_park_stuck(const char *work_item_id, const char *why)
{
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1)
      return -1;
   if (wi.pause_reason[0])
      return 1; /* already parked (incl. a prior 'stuck'): leave it, don't spam */
   db1_work_item_set_pause(work_item_id, "stuck", wi.current_stage);
   db1_lifecycle_event_add(work_item_id, wi.current_stage, "pause", "engine",
                           (why && why[0]) ? why : "unrecoverable advance failure", "", 0);
   return 0;
}

int wfe_autonomy_run(const char *work_item_id, char *err, size_t errlen)
{
   /* Per-run safety ceilings (WP-5). max_turns is a CUMULATIVE cap on the persisted
    * audit-event count: it bounds the WHOLE run across resumes (events persist), and
    * because one advance emits several events it is a deliberately CONSERVATIVE upper
    * bound on advances — fine for a runaway backstop. max_wall bounds THIS resume's
    * wall-clock (a single hung resume); total run time is bounded transitively by the
    * cumulative turn cap. On breach -> park budget_exceeded (never silently continue).
    * Env-overridable; a bad override falls back to the default. */
   long max_turns = wfe_env_long("AIMEE_AUTONOMY_MAX_TURNS", 300);
   long max_wall = wfe_env_long("AIMEE_AUTONOMY_MAX_WALL_SECS", 1800);
   struct timespec ts0;
   clock_gettime(CLOCK_MONOTONIC, &ts0); /* monotonic: immune to NTP/clock jumps */

   /* An item parked 'stuck' cannot advance without human intervention (its stage
    * is unresolvable / has no executor); an item parked 'operator_paused' was
    * deliberately halted by an operator. Skip both so the backstop sweep doesn't
    * re-attempt an advance every cycle; a human resume clears the pause and lets
    * the next sweep retry. */
   {
      db1_work_item_t wi0;
      if (db1_work_item_get(work_item_id, &wi0) == 1 &&
          (strcmp(wi0.pause_reason, "stuck") == 0 ||
           strcmp(wi0.pause_reason, "operator_paused") == 0))
         return 0;
   }

   for (int i = 0; i < 10000; i++)
   {
      db1_lifecycle_event_t *evs = NULL;
      int nev = db1_lifecycle_event_list(work_item_id, &evs);
      free(evs);
      const char *breach = NULL;
      if (nev < 0)
         breach = "turn cap: audit log unreadable"; /* can't evaluate -> fail closed */
      else if ((long)nev >= max_turns)
         breach = "turn cap reached";
      else
      {
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         if ((long)(ts.tv_sec - ts0.tv_sec) >= max_wall)
            breach = "wall-clock cap reached (this resume)";
      }
      if (breach)
      {
         if (wfe_autonomy_park_budget(work_item_id, breach) < 0)
         {
            snprintf(err, errlen, "autonomy: budget breach but work item unreadable to park");
            return -1; /* surface; never leave a breached run un-parked + claim success */
         }
         return 0;
      }

      wfe_advance_result_t r;
      if (wfe_engine_advance(work_item_id, &r, err, errlen) != 0)
      {
         /* A structural advance failure (current_stage not a workflow node, the
          * block has no executor, or a looped node with no on_fail edge) will
          * never self-resolve. Park it 'stuck' so it surfaces in the UI and the
          * scheduler stops retry-failing it every sweep; the error is preserved
          * in the audit event. */
         if (wfe_autonomy_park_stuck(work_item_id, err) < 0)
            return -1; /* row unreadable — surface the original advance error */
         return 0;
      }
      if (r.terminal)
      {
         wfe_autonomy_cleanup_worktree(work_item_id);
         return 0;
      }
      if (r.last_status == WFE_STEP_FAILED)
      {
         /* Phase-C taxonomy: only a TERMINAL disposition tears down the worktree; a
          * park-for-human or park-stuck keeps it so a human resume / new-input retry
          * can pick up where it left off. (Retryable-with-new-input uses the LOOPED
          * path, which never reaches here.) */
         if (wfe_failure_disposition(r.failure_class, r.failure_has_new_input) ==
             WFE_FDISP_TERMINAL)
            wfe_autonomy_cleanup_worktree(work_item_id);
         return 0;
      }
      if (r.last_status != WFE_STEP_PENDING)
         continue; /* ADVANCED / LOOPED: keep going */

      /* Parked at a gate. A human gate is an INVIOLABLE stop: autonomous mode
       * never self-approves it, regardless of any node params — only a human's
       * authenticated, signed approval (POST /v1/workflow/items/<id>/gate, which
       * attributes the decision to the caller's attested principal and records an
       * HMAC-signed approval bound to the content hash) can clear it. So a parked
       * step ends this autonomy pass; the run waits for the human. */
      return 0;
   }
   snprintf(err, errlen, "autonomy run exceeded step bound");
   return -1;
}

int wfe_gate_override(const char *work_item_id, const char *gate, const char *actor,
                      const char *reason, char *err, size_t errlen)
{
   /* the override is attributed to the authenticated caller; the call site (CLI /
    * v1) must pass the real principal — it is bound into the approval MAC. */
   const char *act = (actor && actor[0]) ? actor : "user";
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1)
   {
      snprintf(err, errlen, "unknown work item");
      return -1;
   }
   /* override is only meaningful on a PARKED, active work item — refuse to
    * record an override approval against an active/terminal item (otherwise a
    * caller could mint approvals + burn the cap on a work item not at a gate). */
   if (strcmp(wi.state, "active") != 0 || !wi.pause_reason[0])
   {
      snprintf(err, errlen, "work item is not parked at a gate (state=%s pause=%s)", wi.state,
               wi.pause_reason);
      return -1;
   }
   /* override only the gate the work item is actually parked at, and never a
    * roundtable gate (override is a human-gate affordance — fail closed). */
   if (gate && strcmp(gate, wi.current_stage) != 0)
   {
      snprintf(err, errlen, "gate '%s' is not the current stage '%s'", gate, wi.current_stage);
      return -1;
   }
   {
      char ferr[256];
      wfe_def_t *def = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
      const wfe_node_t *node = def ? wfe_def_node(def, wi.current_stage) : NULL;
      int is_roundtable = node && node->block == WFE_BLK_GATE_ROUNDTABLE;
      if (def)
         wfe_def_free(def);
      if (is_roundtable)
      {
         snprintf(err, errlen, "cannot override a roundtable gate ('%s')", wi.current_stage);
         return -1;
      }
   }
   if (wi.override_count >= WFE_MAX_OVERRIDES)
   {
      db1_work_item_set_terminal(work_item_id, "rejected");
      db1_lifecycle_event_add(work_item_id, gate, "rejected", act, "override cap reached", "", 0);
      return 1;
   }
   /* an override is a recorded human approval bound to the current artifact +
    * the authenticated actor (the MAC covers actor). */
   if (wfe_approval_record(work_item_id, gate, wi.content_hash, act) != 0)
   {
      snprintf(err, errlen, "could not sign override (approval key?)");
      return -1;
   }
   db1_work_item_inc_override(work_item_id);
   db1_lifecycle_event_add(work_item_id, gate, "override", act, reason ? reason : "",
                           wi.content_hash, 0);
   db1_work_item_clear_pause(work_item_id);
   return 0;
}
