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
                             panel_unreachable */
   char paused_state[64];
   char content_hash[72];
   /* The forge ref returned by g_forge->open (PR number/url), opaque to the
    * workflow. "" means no PR has been opened yet (pre-open flow, or a forge
    * provider with no open method). Set when a pr.open block advances; bounded by
    * the wfe_step_result_t.content_hash transport (validated < 64 chars at open). */
   char pr_ref[128];
   char worktree[1024]; /* per-work-item git worktree (aimee/wi/<id>); "" until created */
   char submitter[128]; /* attested principal that submitted the run (intake-auth) */
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
int db1_work_item_add_cost(const char *work_item_id, double cost);
int db1_work_item_set_cost_cap(const char *work_item_id, double cap);
int db1_work_item_inc_override(const char *work_item_id); /* returns new count, -1 err */

/* List work items (newest first). Caller frees *out. Returns count or -1. */
int db1_work_item_list(db1_work_item_t **out);

/* Append an audit event. */
int db1_lifecycle_event_add(const char *work_item_id, const char *stage, const char *kind,
                            const char *actor, const char *detail, const char *content_hash,
                            double cost);
/* List events for a work item (oldest first). Caller frees *out. Returns count. */
int db1_lifecycle_event_list(const char *work_item_id, db1_lifecycle_event_t **out);

/* Per-stage attempt counter (for loop-back max_attempts). */
int db1_stage_attempt_inc(const char *work_item_id, const char *stage); /* new count */
int db1_stage_attempt_get(const char *work_item_id, const char *stage);

/* Coarse transaction control for an atomic advance critical section (the engine
 * uses these so it never touches the raw handle). Return 0 on success. */
int db1_lifecycle_txn_begin(void);
int db1_lifecycle_txn_commit(void);
int db1_lifecycle_txn_rollback(void);

#endif /* DEC_DB1_LIFECYCLE_H */
