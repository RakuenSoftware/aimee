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
/* Set terminal state (accepted | rejected | abandoned) and clear pause. */
int db1_work_item_set_terminal(const char *work_item_id, const char *state);
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
