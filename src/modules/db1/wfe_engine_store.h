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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB1_WFE_ENGINE_STORE_H */
