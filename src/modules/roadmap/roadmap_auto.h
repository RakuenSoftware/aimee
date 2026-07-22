/* roadmap_auto.h: deterministic spec-driven roadmap dispatch loop.
 *
 * A single-track state machine: reconcile DB1 runtime state → select next
 * ready unit → compile tool-policy → dispatch leaf task via delegate →
 * verification gate (auto-fix retries) → persist result → refresh projections
 * → advance; with budget/timeout/stuck guards throughout.
 *
 * The loop is deterministic: no LLM component makes state-transition decisions.
 * Decomposition and leaf-task execution are delegated; the loop only selects,
 * gates, and advances.
 *
 * See docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
 * (Phase 2: single-track gated loop). */
#ifndef DEC_ROADMAP_AUTO_H
#define DEC_ROADMAP_AUTO_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* --- Configuration ---------------------------------------------------- */

   /* Maximum verify retries before a task is moved to needs_review. */
#define ROADMAP_AUTO_MAX_VERIFY_RETRIES 3

   /* Maximum iterations before stuck-loop detection triggers. */
#define ROADMAP_AUTO_STUCK_WINDOW 8

   /* Default soft timeout (seconds) before the loop wraps up the current
    * unit and pauses. 0 means no soft timeout. */
#define ROADMAP_AUTO_DEFAULT_SOFT_TIMEOUT_S 0

   typedef struct
   {
      int max_verify_retries;       /* per-task auto-fix retries (0 → ROADMAP_AUTO_MAX) */
      int budget_ceiling_tokens;    /* 0 means no ceiling */
      int soft_timeout_s;           /* 0 means no timeout */
      int require_slice_discussion; /* gate at slice boundary (default 1) */
      int max_iterations;           /* safety ceiling on total loop iterations (0 = unlimited) */
      const char *cwd;              /* working directory for delegate dispatch */
      /* Headless/unattended mode: loop restarts automatically after completion
       * or recoverable pause (budget/timeout) up to max_restarts times. A hard
       * error or 'stopped' status terminates regardless of restarts remaining.
       * 0 = gated mode (default); >0 = headless with up to N restarts. */
      int headless;     /* 0 = gated (default), 1 = headless */
      int max_restarts; /* 0 = unlimited headless restarts */
      /* Run reassessment pass when all milestones complete. If the reassessment
       * delegate finds gaps, the loop injects new tasks and continues. */
      int run_reassessment; /* 0 = off (default), 1 = run at completion */
   } roadmap_auto_cfg_t;

   /* --- Loop control ------------------------------------------------------- */

   /* Exit reasons written to roadmap_dispatch.exit_reason. */
#define ROADMAP_AUTO_EXIT_ALL_COMPLETE     "all_complete"
#define ROADMAP_AUTO_EXIT_PAUSED           "paused"
#define ROADMAP_AUTO_EXIT_STOPPED          "stopped"
#define ROADMAP_AUTO_EXIT_BUDGET           "budget_ceiling"
#define ROADMAP_AUTO_EXIT_TIMEOUT          "timeout"
#define ROADMAP_AUTO_EXIT_STUCK            "stuck"
#define ROADMAP_AUTO_EXIT_BLOCKED          "blocked"
#define ROADMAP_AUTO_EXIT_VERIFY_EXHAUSTED "verification_exhausted"
#define ROADMAP_AUTO_EXIT_ERROR            "error"

   /* --- Public API --------------------------------------------------------- */

   /* Run the dispatch loop for roadmap_id until a stop condition is reached.
    * cfg may be NULL (all defaults are used).
    * Writes the exit reason into exit_reason (if non-NULL).
    * Returns 0 on normal completion/pause/stop, -1 on hard error. */
   int roadmap_auto_run(const char *roadmap_id, const roadmap_auto_cfg_t *cfg, char *exit_reason,
                        size_t exit_reason_len);

   /* Pause an active loop: sets roadmap_dispatch.status = 'paused'.
    * The running roadmap_auto_run will complete the current unit and exit. */
   int roadmap_auto_pause(const char *roadmap_id);

   /* Resume a paused loop (calls roadmap_auto_run internally). */
   int roadmap_auto_resume(const char *roadmap_id, const roadmap_auto_cfg_t *cfg, char *exit_reason,
                           size_t exit_reason_len);

   /* Stop an active or paused loop: sets status = 'stopped'. */
   int roadmap_auto_stop(const char *roadmap_id);

   /* Print the current loop status to stdout. json_output != 0 emits JSON. */
   int roadmap_auto_status(const char *roadmap_id, int json_output);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROADMAP_AUTO_H */
