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

#include <sqlite3.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Bounds for the id lists below. A tree deeper or wider than this is not a
    * tree the engine can act on in one call anyway, and an unbounded list across
    * the wire is a reply the caller cannot size. */
   /* Open a write transaction, waiting out a busy store rather than reporting
    * one. Shared by every transactional operation here: see the implementation
    * for why a plain BEGIN IMMEDIATE is not enough under load. */
   int db1_wfe_begin_immediate(sqlite3 *db);

#define DB1_WFE_ID_LEN      80
#define DB1_WFE_ID_LIST_MAX 512

   typedef struct
   {
      char execution_key[256];
      long long job_id;
   } db1_wfe_delegate_job_t;

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

   typedef struct
   {
      char root_id[DB1_WFE_ID_LEN];
      double spent;
      double max_usd;
   } db1_wfe_budget_totals_t;

   typedef struct
   {
      int attempts;
      int identical_repeats;
      int parked;
      char pause_reason[32];
   } db1_wfe_review_outcome_t;

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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB1_WFE_ENGINE_STORE_H */
