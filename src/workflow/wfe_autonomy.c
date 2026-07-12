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

/* A roundtable panel that could not be composed (panel_degraded) or reached
 * (panel_unreachable) is TRANSIENT provider flakiness, not a human decision:
 * these two pause reasons are auto-retried by the autonomy driver. */
static int autonomy_pause_is_transient(const char *reason)
{
   return strcmp(reason, "panel_degraded") == 0 || strcmp(reason, "panel_unreachable") == 0;
}

/* The per-(work item, stage) transient-park retry cap. Unlike wfe_env_long (which
 * floors junk/<=0 to the default so a safety rail can't be silently disabled by a
 * typo), this ACCEPTS an explicit 0 as a deliberate operator escape hatch: 0 means
 * "do not auto-retry transient panel parks — escalate to a human immediately" (the
 * pre-feature behavior), useful to set during a known provider incident. A
 * malformed or negative override still falls back to the default. */
static long autonomy_panel_retry_cap(void)
{
   const char *v = getenv("AIMEE_AUTONOMY_PANEL_RETRIES");
   if (!v || !v[0])
      return WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT;
   errno = 0;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (errno == ERANGE || !end || *end != '\0' || n < 0)
      return WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT;
   return n; /* n >= 0; an explicit 0 disables auto-retry (escalate immediately) */
}

/* Count how many times this run has already auto-retried a transient roundtable
 * park at `stage` — one "panel_retry" audit event is written per retry. The
 * caller compares this against the cap to bound retries, so a persistently-
 * degraded panel eventually escalates to a human instead of retrying forever.
 * Returns the count; on a read failure it
 * returns 0 (fail toward retrying — a transient DB fault must not permanently
 * strand an otherwise-healthy run). This fail-open is best-effort but bounded: a
 * PERSISTENT audit-log read failure is caught on the very next step by the
 * turn-cap check at the top of the advance loop, which fail-CLOSES to
 * turn_cap_exceeded when db1_lifecycle_event_list is unreadable — so an
 * unreadable log parks the run rather than looping. The turn cap is the hard
 * runaway backstop; the panel-retry cap is the precise-but-best-effort bound.
 *
 * Durability: the budget is derived from the audit log rather than a counter
 * column, which is safe because lifecycle events are never pruned/compacted/
 * rotated mid-run — the only DELETE on lifecycle_event is the terminal
 * whole-work-item purge (db1_work_item_delete). So while a run is active its
 * panel_retry history is complete and the derived count is authoritative. */
static int autonomy_panel_retries_used(const char *work_item_id, const char *stage)
{
   db1_lifecycle_event_t *evs = NULL;
   int n = db1_lifecycle_event_list(work_item_id, &evs);
   if (n <= 0)
   {
      free(evs); /* NULL-safe: nothing read -> 0 retries used (fail toward retry) */
      return 0;
   }
   int used = 0;
   /* kind[] and stage[] are fixed-size arrays in db1_lifecycle_event_t, never
    * NULL, so strcmp is safe without a null guard. */
   for (int i = 0; i < n; i++)
      if (strcmp(evs[i].kind, "panel_retry") == 0 && strcmp(evs[i].stage, stage) == 0)
         used++;
   free(evs);
   return used;
}

/* Park an active, not-yet-parked autonomous work item at a runaway-backstop cap
 * and audit the reason. `reason` is the accurate pause token for the breach that
 * fired (turn_cap_exceeded / wall_cap_exceeded — NOT the dollar cost cap, which
 * is a separate ceiling that parks budget_exceeded); `why` is the human-readable
 * audit detail. Returns 0 on a clean park, 1 if it was already parked (still
 * audits the breach), -1 if the row could not be read (caller hard-stops — a
 * safety rail must not silently no-op on a read failure). */
static int wfe_autonomy_park_cap(const char *work_item_id, const char *reason, const char *why)
{
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1)
      return -1;
   if (wi.pause_reason[0])
   {
      /* already parked (e.g. at a gate): record the breach for audit, don't
       * overwrite the existing pause reason. */
      db1_lifecycle_event_add(work_item_id, wi.current_stage, "cap", "engine", why, "", 0);
      return 1;
   }
   db1_work_item_set_pause(work_item_id, reason, wi.current_stage);
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
    * cumulative turn cap. On breach -> park with the accurate cap reason
    * (turn_cap_exceeded / wall_cap_exceeded); never silently continue. These are
    * runaway backstops, NOT the dollar cost cap (which parks budget_exceeded).
    * Env-overridable; a bad override falls back to the default. */
   long max_turns = wfe_env_long("AIMEE_AUTONOMY_MAX_TURNS", 300);
   long max_wall = wfe_env_long("AIMEE_AUTONOMY_MAX_WALL_SECS", 1800);
   struct timespec ts0;
   clock_gettime(CLOCK_MONOTONIC, &ts0); /* monotonic: immune to NTP/clock jumps */

   /* An item parked 'stuck' cannot advance without human intervention (its stage
    * is unresolvable / has no executor); an item parked 'operator_paused' was
    * deliberately halted by an operator. Skip both so the backstop sweep doesn't
    * re-attempt an advance every cycle; a human resume clears the pause and lets
    * the next sweep retry.
    *
    * A transient roundtable park (panel_degraded / panel_unreachable), by
    * contrast, reflects momentary provider flakiness — reviewers that were DOWN
    * or unreachable when the panel convened — NOT a human decision. Auto-retry it
    * a bounded number of times (one attempt per backstop sweep = natural
    * backoff): clear the pause here so the advance loop below re-runs the
    * roundtable node, and let a recovered panel self-heal. Only once the retry
    * budget is spent does it stay parked to escalate to a human. Without this an
    * autonomous run dead-ends forever on a momentary degradation, which is
    * exactly the opposite of what a panel "fail-closed" should mean for autonomy.
    *
    * Only stuck/operator_paused are short-circuited HERE. Every other park —
    * pending_human, budget_exceeded, ci_pending, … — falls through to the advance
    * loop, where wfe_engine_advance sees the still-set pause_reason and returns
    * PENDING without re-running work ("caller must resume explicitly"); those stay
    * human-/event-gated. The explicit skip exists only to keep the backstop sweep
    * from re-attempting an advance every cycle on the two never-self-resolving
    * reasons. */
   {
      db1_work_item_t wi0;
      if (db1_work_item_get(work_item_id, &wi0) == 1 && wi0.pause_reason[0])
      {
         if (strcmp(wi0.pause_reason, "stuck") == 0 ||
             strcmp(wi0.pause_reason, "operator_paused") == 0)
            return 0;
         if (autonomy_pause_is_transient(wi0.pause_reason))
         {
            /* Budget is per (work item, stage): a stage revisited after a loop
             * inherits its earlier retry count on purpose — a stage that has
             * already burned its budget and comes back still-degrading should
             * escalate to a human, not silently reset. current_stage here is the
             * workflow node id (a stable machine string, not user input), so the
             * per-stage key does not drift across a degraded-park/retry cycle. The
             * cap accepts an explicit 0 (escalate immediately) but floors a
             * malformed/negative override to the default. */
            long cap = autonomy_panel_retry_cap();
            if (cap == 0 || autonomy_panel_retries_used(work_item_id, wi0.current_stage) >= cap)
               return 0; /* budget spent (or disabled): stay parked to escalate to a human */
            /* Claim the retry with a compare-and-clear: clear the pause only while
             * it still equals the (reason, stage) we just read, so at most one
             * retry runs per park even if the single-threaded-scheduler invariant
             * is ever relaxed (a concurrent driver's clear would find the row
             * already moved). rc: 1 = we won, 0 = another driver moved it first,
             * -1 = error. On anything but a win, leave it and retry next sweep — no
             * budget spent, no uncounted retry. NOTE the panel_retry cap bounds
             * ATTEMPTS (parks claimed + cleared), not successful re-convenes: if the
             * advance below dies before the panel re-runs, the attempt still counted
             * — which is the intended, honest semantics (the turn cap is the hard
             * backstop for a genuinely runaway loop). */
            int claimed =
                db1_work_item_clear_pause_if(work_item_id, wi0.pause_reason, wi0.current_stage);
            if (claimed != 1)
               return 0;
            /* Symmetric on the write side: if the panel_retry audit write fails the
             * attempt would run UNCOUNTED and could bypass the cap, so treat it like
             * a failed clear — re-park with the ORIGINAL (reason, paused_state) and
             * do not advance. If the re-park ALSO fails the item is left unpaused
             * with no retry event, which must not be reported as a benign no-op:
             * surface it (return -1) so the scheduler logs it rather than silently
             * looping. (Belt-and-braces: even if the process died in the tiny window
             * between the clear and this write, the re-convened panel writes its own
             * "pause" lifecycle event, which the cumulative turn cap counts, so the
             * runaway backstop still holds.) */
            if (db1_lifecycle_event_add(work_item_id, wi0.current_stage, "panel_retry", "engine",
                                        wi0.pause_reason, "", 0) != 0)
            {
               if (db1_work_item_set_pause(work_item_id, wi0.pause_reason, wi0.paused_state) != 0)
               {
                  snprintf(err, errlen, "panel-retry: audit write and re-park both failed for '%s'",
                           work_item_id);
                  return -1;
               }
               return 0;
            }
            /* pause cleared + retry durably recorded -> fall through to the advance
             * loop, which re-runs the parked roundtable node for a fresh panel. */
         }
      }
   }

   for (int i = 0; i < 10000; i++)
   {
      db1_lifecycle_event_t *evs = NULL;
      int nev = db1_lifecycle_event_list(work_item_id, &evs);
      free(evs);
      const char *breach = NULL; /* human-readable audit detail */
      const char *reason = NULL; /* accurate pause token for the breach */
      if (nev < 0)
      {
         breach = "turn cap: audit log unreadable"; /* can't evaluate -> fail closed */
         reason = "turn_cap_exceeded";
      }
      else if ((long)nev >= max_turns)
      {
         breach = "turn cap reached";
         reason = "turn_cap_exceeded";
      }
      else
      {
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         if ((long)(ts.tv_sec - ts0.tv_sec) >= max_wall)
         {
            breach = "wall-clock cap reached (this resume)";
            reason = "wall_cap_exceeded";
         }
      }
      if (breach)
      {
         if (wfe_autonomy_park_cap(work_item_id, reason, breach) < 0)
         {
            snprintf(err, errlen, "autonomy: run cap breach but work item unreadable to park");
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
