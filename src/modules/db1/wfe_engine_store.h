/* wfe_engine_store.h: the store operations the workflow ENGINE needs.
 *
 * wfe_store.h carries what the daemon's API surface asks of a work item --
 * fetch it, list them, apply a gate. This header carries what the engine that
 * EXECUTES them asks, which is a different set: budget reservation, descendant
 * trees, counts taken since the last progress event.
 *
 * They are separate because of who was asking. The engine is
 * server-go/cmd/aimee-server, and until now it asked SQLite directly -- it
 * opened $home/aimee.db, the module's own file, ran its own CREATE TABLE and
 * ALTER ladder against it, and issued its own recursive CTEs. Two processes
 * with two schema authorities on one store, which is the arrangement the module
 * doctrine exists to prevent, and which was measured rather than assumed:
 * scripts/validation/db1-module-wfe-coexistence.sh watches both processes hold
 * the file open.
 *
 * So these are the engine's queries, moved to the side of the boundary that
 * owns the data. Each one is the whole of what the engine used to do in one
 * statement or one transaction, because a transaction cannot be assembled from
 * separate calls across a wire -- the same reason wfe_engine.c's sixteen writes
 * became db1_work_item_record_outcome.
 */
#ifndef AIMEE_DB1_WFE_ENGINE_STORE_H
#define AIMEE_DB1_WFE_ENGINE_STORE_H 1

#include "wfe_store.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Bounds for the id lists below. A tree deeper or wider than this is not a
 * tree the engine can act on in one call anyway, and an unbounded list across
 * the wire is a reply the caller cannot size. */
#define DB1_WFE_ID_LEN      80
#define DB1_WFE_ID_LIST_MAX 512

   /* Child work items of parent_id, in insertion order, ids only. The engine
    * aggregates children's terminal states; it does not need their rows. */
   int db1_wfe_children_list(const char *parent_id, char (*out_ids)[DB1_WFE_ID_LEN], int max);

   /* How many top-level runs are active. The engine's admission cap is over
    * roots, so a child slice must not count against it. */
   int db1_wfe_active_root_count(void);

   /* The root run for a git-sourced proposal. Matches proposal_path exactly, or
    * a "git:" path whose tail is the given suffix -- the engine records the
    * full ref and later looks the run up by the part it still has. */
   int db1_wfe_work_item_id_by_git_proposal(const char *repo, const char *proposal_path,
                                            const char *suffix, char *out, size_t n);

   /* Turns the engine has actually executed for a run: advance and loop events.
    * Used against the turn cap, so a pause or a retry must not inflate it. */
   int db1_wfe_executed_turn_count(const char *work_item_id);

   /* How many times a stage has been entered, counting both a fresh advance and
    * a loop back into it. */
   int db1_wfe_stage_loop_count(const char *work_item_id, const char *stage);

   /* Pauses recorded for a stage since the run last made progress, split by
    * whether they were capacity backpressure.
    *
    * "Since progress" is the id of the newest advance/loop/create event, so a
    * run that moved on starts counting again from zero: these drive the
    * engine's give-up thresholds, and a threshold that counted a run's whole
    * history would trip on runs that had already recovered. Capacity waits are
    * separated because waiting for a runner is not the same failure as a stage
    * that will not pass, and they are not allowed to exhaust the same budget. */
   int db1_wfe_runner_failures_since_progress(const char *work_item_id, const char *stage);
   int db1_wfe_capacity_waits_since_progress(const char *work_item_id, const char *stage);

   /* Every id in a run's subtree, the run itself first. The engine stops or
    * deletes a tree as a unit, so it needs the whole set before it acts on any
    * of it. */
   int db1_wfe_descendant_ids(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max);

   /* Bulk resumes and abandons, each one statement over every run that
    * qualifies. They return how many rows moved, because the caller logs the
    * number and a caller that had to count them itself would need the ids it
    * has no other use for.
    *
    * The cutoffs are seconds rather than timestamps: the store already decides
    * what "now" means for every other row it writes, and a caller computing a
    * cutoff from its own clock is how two processes come to disagree about
    * which rows are stale. */
   long long db1_wfe_resume_transient(const char *pause_reason, int older_than_secs);
   long long db1_wfe_resume_wall_caps(int max_resumes);
   long long db1_wfe_abandon_exhausted_wall_caps(int max_resumes, int grace_secs);

   /* A parent parked on 'slices_running' whose children have all finished. It
    * must have children -- a parent with none never fanned out, and resuming it
    * would advance a run whose slices were never created. */
   long long db1_wfe_resume_ready_parents(void);

   /* The engine's record of which delegate job is executing which step. */
   int db1_wfe_delegate_job_save(const char *execution_key, const char *work_item_id,
                                 long long job_id, const char *participant_token);

   typedef struct
   {
      char execution_key[256];
      long long job_id;
   } db1_wfe_delegate_job_t;

   /* Delegate jobs still pending or running under a run that has already
    * finished, oldest-attempted first -- and claiming them, because reading this
    * list is only ever the prelude to cancelling them and a reader that did not
    * record the attempt would hand the same jobs to the next caller forever.
    * Returns how many were claimed. */
   int db1_wfe_delegate_jobs_terminal_claim(db1_wfe_delegate_job_t *out, int max);

   /* One run's claim on its tree's budget. Amount is the estimate it may spend;
    * allowed says whether it may run at all; busy means another owner holds a live
    * lease; replay_only means the invocation may already have happened and this
    * caller may only reconcile it, never spend again. */
   typedef struct
   {
      char root_id[DB1_WFE_ID_LEN];
      double max_usd;
      double amount;
      int allowed;
      int busy;
      int replay_only;
   } db1_wfe_budget_reservation_t;

   /* Take (or renew, or take over) this run's reservation. Retries internally on
    * a lost race, because the race is only visible inside the transaction. */
   int db1_wfe_budget_reserve(const char *work_item_id, const char *owner,
                              db1_wfe_budget_reservation_t *out);

   typedef struct
   {
      char root_id[DB1_WFE_ID_LEN];
      double spent;
      double max_usd;
   } db1_wfe_budget_totals_t;

   /* What the run's whole tree has spent, and the cap declared on its root. */
   int db1_wfe_budget_totals(const char *work_item_id, db1_wfe_budget_totals_t *out);

   /* Give back an estimate that was never spent. Only a 'reserved' state is
    * released: 'actual' and 'unresolved' are authorized spend. */
   int db1_wfe_budget_release(const char *work_item_id, const char *owner);

   /* Extend the lease while an invocation is still running. */
   int db1_wfe_budget_heartbeat(const char *work_item_id, const char *owner);

   /* Replace the estimate with measured cost, exactly once. Returns 1 when this
    * call was the one that did it, 0 when someone else already had, -1 on
    * error. The caller needs that distinction because only the winner may then
    * charge the cost to the tree -- both charging would spend it twice. */
   int db1_wfe_budget_reconcile(const char *work_item_id, const char *owner, double actual);

   /* Stage transitions, each one whole transaction. All of them release the
    * run's budget reservation, because the invocation it authorized has ended.
    *
    * Every one is guarded by the stage the caller believes the run is in: a
    * transition computed from a stale view must fail rather than overwrite
    * whatever actually happened. They return -1 when that guard does not match,
    * which is a lost race and not a broken store. */
   int db1_wfe_move(const char *work_item_id, const char *from_stage, const char *to_stage,
                    const char *kind, const char *detail, const char *content_hash, double cost);

   /* Charge one attempt against the stage's retry budget. Returns 1 if that
    * exhausted it and the run is now parked on 'retry_limit', 0 if it loops
    * again, -1 on error. */
   int db1_wfe_record_retry(const char *work_item_id, const char *stage, const char *to_stage,
                            const char *detail, int max_attempts, double cost);

   int db1_wfe_park_with_detail(const char *work_item_id, const char *stage, const char *reason,
                                const char *detail, double cost);

   /* Clear a pause and let the stage start over: the attempts that exhausted its
    * budget are what parked it. */
   int db1_wfe_resume(const char *work_item_id);

   /* Terminal state, with the run's frozen create records dropped -- they exist
    * to make a re-run reuse its children, and there will be no re-run. */
   int db1_wfe_finish(const char *work_item_id, const char *stage, const char *state,
                      const char *detail, const char *content_hash, double cost);

   /* Whole-tree operations. A run fans out into child slices, and these four
    * things must happen to the tree as a unit: a stop that reached half of it
    * leaves agents running under a run the operator believes is stopped.
    *
    * stop_tree and reconcile_orphans return the ids they ended, because the
    * caller cancels the delegate jobs those runs left behind. */
   int db1_wfe_stop_tree(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max);

   /* Active runs whose ancestor has already finished. Nothing will advance them
    * and nothing is waiting for them, so they are ended and reported. */
   int db1_wfe_reconcile_orphans(char (*out_ids)[DB1_WFE_ID_LEN], int max);

   /* Charge a finished invocation whose cost exhausted the tree's budget, and
    * park every runnable member on 'budget_cap'. */
   int db1_wfe_park_budget_tree(const char *root_id, const char *completed_item_id,
                                double added_cost);

   /* Delete a tree outright. Refuses while any member is still active: deleting
    * the row does not stop the agent working under it. */
   int db1_wfe_delete_tree(const char *work_item_id);

   /* The engine's human gate, which parks on 'human_gate'. Distinct from
    * db1_work_item_gate_apply, which serves the daemon's own gate flow, guards
    * on 'pending_human' and compares a content hash -- two different gates that
    * happen to share a word. */
   int db1_wfe_resolve_gate(const char *work_item_id, const char *from_stage, const char *to_stage,
                            const char *decision, const char *content_hash);
   int db1_wfe_reject_gate(const char *work_item_id, const char *stage, const char *content_hash);

   /* Park a runner failure with whatever is known about what it cost. When the
    * invocation was dispatched but its cost is unknown, the known prefix is
    * committed and the rest of the authorization is retained as 'unresolved' --
    * releasing it would let the tree hand out money that may already be gone. */
   int db1_wfe_park_runner_failure(const char *work_item_id, const char *stage, const char *owner,
                                   const char *reason, const char *detail, int dispatched,
                                   int cost_known, double actual);

   /* A replay whose result never came back. Returns 1 when the run may be
    * re-dispatched fresh (the estimate was never committed), 0 when the spend
    * was measured but its result is unreproducible and the run has been parked
    * for a human instead, -1 on error or a reservation this owner does not
    * hold. */
   int db1_wfe_recover_lost_replay(const char *work_item_id, const char *stage, const char *owner);

   typedef struct
   {
      int attempts;
      int identical_repeats;
      int parked;
      char pause_reason[32];
   } db1_wfe_review_outcome_t;

   /* A review gate that asked for changes. Bounded twice: by how many rounds it
    * has run, and by how many of those produced the SAME artifact against the
    * SAME feedback. The second is reported in preference to the first, because
    * a run that is repeating itself should be described as repeating itself
    * rather than as merely out of rounds. */
   int db1_wfe_record_requested_changes(const char *work_item_id, const char *gate,
                                        const char *plan_stage, const char *plan_hash,
                                        const char *feedback_hash, const char *unresolved,
                                        int max_iterations, int max_identical, double cost,
                                        db1_wfe_review_outcome_t *out);

#define DB1_WFE_FROZEN_PATH_LEN 1024
#define DB1_WFE_FROZEN_HASH_LEN 72
#define DB1_WFE_FROZEN_MAX      64

   typedef struct
   {
      char path[DB1_WFE_FROZEN_PATH_LEN];
      char content_hash[DB1_WFE_FROZEN_HASH_LEN];
   } db1_wfe_frozen_create_t;

   typedef struct
   {
      char parent_id[DB1_WFE_ID_LEN];
      char work_item_id[DB1_WFE_ID_LEN];
      db1_wfe_frozen_create_t creates[DB1_WFE_FROZEN_MAX];
      int create_count;
   } db1_wfe_frozen_claim_t;

   /* An empty path means there was no conflict. The alternative -- a separate
    * boolean -- gives two ways to say the same thing and eventually they disagree. */
   typedef struct
   {
      char path[DB1_WFE_FROZEN_PATH_LEN];
      char existing_work_item[DB1_WFE_ID_LEN];
      char conflicting_work_item[DB1_WFE_ID_LEN];
   } db1_wfe_frozen_conflict_t;

   /* Publish every path a slice's frozen diff creates, as a unit.
    *
    * Sibling slices may create the same path with the same content -- that is
    * two slices agreeing -- but two slices creating the same path with
    * DIFFERENT content is a conflict, and the whole claim rolls back so there
    * is exactly one winner and never a partial path set.
    *
    * Returns 0 with an empty conflict path when the claim was published, 0 with
    * a conflict when it was refused, -1 on error. */
   int db1_wfe_claim_frozen_creates(const db1_wfe_frozen_claim_t *claim,
                                    db1_wfe_frozen_conflict_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB1_WFE_ENGINE_STORE_H */
