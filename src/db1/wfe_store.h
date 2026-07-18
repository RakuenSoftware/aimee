/* db1/lifecycle.h: workflow-engine work-item state + audit log (per-user DB1).
 * Typed domain accessors; callers never touch the sqlite handle. */
#ifndef DEC_DB1_LIFECYCLE_H
#define DEC_DB1_LIFECYCLE_H 1

#include <stddef.h>

typedef struct
{
   char work_item_id[80];
   char repo[512];
   char proposal_path[1024];
   char workflow_name[64];
   char workflow_version[80];
   char current_stage[64];
   char state[24];        /* active | accepted | rejected | abandoned */
   char mode[16];         /* interactive | autonomous */
   char pause_reason[32]; /* "" | pending_human | panel_degraded | budget_exceeded |
                             panel_unreachable | ci_pending | merge_pending |
                             turn_cap_exceeded | wall_cap_exceeded | stuck |
                             slices_running | operator_paused */
   char paused_state[64];
   char content_hash[72];
   /* The forge ref returned by g_forge->open (PR number/url), opaque to the
    * workflow. "" means no PR has been opened yet (pre-open flow, or a forge
    * provider with no open method). Set when a pr.open block advances; bounded by
    * the wfe_step_result_t.content_hash transport (validated < 64 chars at open). */
   char pr_ref[128];
   char worktree[1024]; /* per-work-item git worktree (aimee/wi/<id>); "" until created */
   char submitter[128]; /* attested principal that submitted the run (intake-auth) */
   char parent_id[80];  /* parent work item (foreach.workflow child), "" for a top-level run */
   double cum_cost_usd;
   double work_item_max_cost_usd; /* 0 = no cap */
   int override_count;
} db1_work_item_t;

typedef struct
{
   long id;
   char stage[64];
   char kind[24];
   char actor[64];
   char detail[512];
   char content_hash[72];
   double cost_usd;
   char created_at[40];
} db1_lifecycle_event_t;

/* Create a new work item. Returns 0 on success, -1 on error (incl. UNIQUE
 * violation on (repo, proposal_path) or work_item_id). */
int db1_work_item_create(const char *work_item_id, const char *repo, const char *proposal_path,
                         const char *workflow_name, const char *workflow_version,
                         const char *start_stage, const char *mode);

/* Fetch a work item by id. Returns 1 if found, 0 if not, -1 on error. */
int db1_work_item_get(const char *work_item_id, db1_work_item_t *out);

/* Resolve a work-item id by its unique (repo, proposal_path). Fills `out` (id-sized,
 * >= 80). Returns 1 if found, 0 if not, -1 on error. Lets a caller that hit the
 * UNIQUE(repo, proposal_path) create-collision recover + reuse the existing id
 * (e.g. resume an interactive work-item after its lease was reclaimed). */
int db1_work_item_id_by_proposal(const char *repo, const char *proposal_path, char *out, size_t n);

/* Resolve a work-item id from its forge PR ref (the value stored by
 * db1_work_item_set_pr_ref) — used by the CI-event webhook to route an inbound CI
 * outcome to its run. Returns 1 + fills out on a unique match, 0 if none, -1 on a
 * bad arg / db error. pr_ref must be non-empty. */
int db1_work_item_id_by_pr_ref(const char *pr_ref, char *out, size_t n);

/* Move to a new stage + update content_hash (state unchanged). */
int db1_work_item_set_stage(const char *work_item_id, const char *stage, const char *content_hash);
/* Record the forge PR ref opened for this work item (set when pr.open advances),
 * so the downstream forge gates (gate.ci / check.mergeable / merge) resolve the
 * real PR instead of the work-item id. */
int db1_work_item_set_pr_ref(const char *work_item_id, const char *pr_ref);
/* Record the per-work-item git worktree path (set when it is first created). */
int db1_work_item_set_worktree(const char *work_item_id, const char *worktree);
/* Record the attested submitter principal (intake-auth audit binding). */
int db1_work_item_set_submitter(const char *work_item_id, const char *submitter);
/* Record the parent work item of a foreach.workflow child ("slice") run. */
int db1_work_item_set_parent(const char *work_item_id, const char *parent_id);
/* Aggregate the terminal state of a parent's children (foreach.workflow fan-in).
 * Fills the counts (any may be NULL): total children, those in state 'accepted',
 * and those that reached a terminal state OTHER than accepted -- i.e. 'rejected' or
 * 'abandoned' ('failed', a slice that will never merge). Returns 0 on success, -1 on
 * error. A parent whose total==accepted (and total>0) has every slice merged; any
 * failed child means a slice will not merge and the parent must park for a human. */
int db1_work_item_child_counts(const char *parent_id, int *total, int *accepted, int *failed);
/* Count this submitter's ACTIVE autonomous work items (per-principal concurrency
 * cap). Returns the count, or -1 on error. */
int db1_work_item_count_active_by_submitter(const char *submitter);
/* Count this submitter's work items created within the last `secs` seconds
 * (per-principal rate window). Returns the count, or -1 on error. */
int db1_work_item_count_recent_by_submitter(const char *submitter, int secs);
/* Atomic intake-auth submit (POST /v1/dev/submit): enforce the per-principal
 * concurrency (max_active) + rate (rate_max within rate_secs) caps and, if both
 * pass, create the autonomous work item, bind the submitter, and write the
 * attributed "submit" audit event — all under one BEGIN IMMEDIATE so concurrent
 * submits from one principal serialize (TOCTOU-free) and a DB fault or partial
 * write fails closed (rollback). A cap value <= 0 disables that cap. Returns
 * 0 = created, 1 = concurrency cap hit, 2 = rate cap hit, -1 = error. */
int db1_work_item_submit_capped(const char *work_item_id, const char *repo,
                                const char *proposal_path, const char *workflow_name,
                                const char *workflow_version, const char *start_stage,
                                const char *submitter, int max_active, int rate_max, int rate_secs);
/* Set terminal state (accepted | rejected | abandoned) and clear pause. */
int db1_work_item_set_terminal(const char *work_item_id, const char *state);

/* Apply an operator human-gate decision ATOMICALLY, guarded against TOCTOU /
 * double-action. The row must still be parked exactly as the caller observed:
 * current_stage == expect_stage AND content_hash == expect_hash AND
 * pause_reason == 'pending_human'. Content is immutable while parked, so
 * content_hash is an identity / optimistic-concurrency guard (never re-derived).
 * Exactly one decision per call:
 *   - new_stage non-empty   -> loop back (current_stage := new_stage, clear pause)
 *   - terminal_state non-empty -> state := terminal_state, clear pause
 *   - both NULL/empty       -> approve (clear pause only; stage unchanged)
 * (new_stage and terminal_state must not both be set.) Single UPDATE, so two
 * concurrent operators cannot both apply. Returns 1 = applied, 0 = precondition
 * not met (caller should report 409 conflict), -1 = error. */
int db1_work_item_gate_apply(const char *work_item_id, const char *expect_stage,
                             const char *expect_hash, const char *new_stage,
                             const char *terminal_state);
int db1_work_item_set_pause(const char *work_item_id, const char *reason, const char *paused_state);
int db1_work_item_clear_pause(const char *work_item_id);
/* Compare-and-clear: clear the pause only while it still equals (expect_reason,
 * expect_stage). Returns 1 if this caller cleared it, 0 if the row no longer
 * matched (another driver moved it first), -1 on error (incl. SQLITE_BUSY — a
 * transient lock is deliberately folded into -1: the autonomy caller treats -1
 * and 0 identically as "did not claim; retry next sweep", so a lock just defers
 * the attempt). Lets the autonomy retry path claim a transient park atomically,
 * so at most one retry runs per park. */
int db1_work_item_clear_pause_if(const char *work_item_id, const char *expect_reason,
                                 const char *expect_stage);
int db1_work_item_add_cost(const char *work_item_id, double cost);
int db1_work_item_set_cost_cap(const char *work_item_id, double cap);
int db1_work_item_inc_override(const char *work_item_id); /* returns new count, -1 err */

/* Permanently delete a work item and all its history (lifecycle_event +
 * lifecycle_stage_attempt + lifecycle_work_item rows) under one transaction.
 * There is no FK cascade, so all three tables are cleared explicitly. Returns
 * 0 on success (incl. an already-absent item — DELETE is idempotent), -1 on
 * error. The caller is responsible for any out-of-DB artifacts (proposal file,
 * worktree). */
int db1_work_item_delete(const char *work_item_id);

/* Backstop reaper: abandon (make terminal) any autonomous run parked in a
 * runaway/failure backstop (stuck / turn_cap_exceeded / wall_cap_exceeded /
 * budget_exceeded) whose last update is older than grace_secs, so a dead run does
 * not linger 'active' forever. Human-review parks (pending_human) and
 * operator_paused are legitimate waits and are NOT reaped. grace_secs <= 0
 * disables. Returns the number of runs reaped. The scheduler's terminal walk then
 * tears down each reaped run's worktree on the next sweep. */
int db1_work_item_reap_stale_parks(long grace_secs);

/* List work items (newest first). Caller frees *out. Returns count or -1. */
int db1_work_item_list(db1_work_item_t **out);

/* List work items LEAST-RECENTLY-UPDATED first — the scheduler's fairness
 * order. A newest-first sweep starves older siblings: the sequential autonomy
 * pass gives each item up to its wall-clock cap, so whichever items sort first
 * eat every sweep (observed live: 2 of 13 fan-out slices monopolized 3.5h
 * while the rest never advanced). Staleness-first makes starvation
 * self-correcting: whoever was skipped longest goes first next sweep.
 * Caller frees *out. Returns count or -1. */
int db1_work_item_list_lru(db1_work_item_t **out);

/* Append an audit event. */
int db1_lifecycle_event_add(const char *work_item_id, const char *stage, const char *kind,
                            const char *actor, const char *detail, const char *content_hash,
                            double cost);
/* List events for a work item (oldest first). Caller frees *out. Returns count. */
int db1_lifecycle_event_list(const char *work_item_id, db1_lifecycle_event_t **out);

/* Per-stage attempt counter (for loop-back max_attempts). */
int db1_stage_attempt_inc(const char *work_item_id, const char *stage);   /* new count */
int db1_stage_attempt_reset(const char *work_item_id, const char *stage); /* re-arm the loop */
int db1_stage_attempt_get(const char *work_item_id, const char *stage);

/* Coarse transaction control for an atomic advance critical section (the engine
 * uses these so it never touches the raw handle). Return 0 on success. */
int db1_lifecycle_txn_begin(void);
int db1_lifecycle_txn_commit(void);
int db1_lifecycle_txn_rollback(void);

#endif /* DEC_DB1_LIFECYCLE_H */
