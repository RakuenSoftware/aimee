/* wfe_store_stub.c: the four work-item calls wfe_blocks.o makes, refusing.
 *
 * test_wfe_blocks is a pure test -- it asserts that every block kind has an
 * executor and that wfe_repo_local resolves a path -- and it reaches none of
 * these. They are here because wfe_blocks.o REFERENCES them, so the binary will
 * not link without them.
 *
 * The target used to satisfy that by linking the real store: db1_init.o,
 * db_schema.o and wfe_store.o, with the comment "no engine/db1; the schema +
 * externalization tests are pure". Pure, and linked against a database anyway,
 * because a link line answers to the linker rather than to the test.
 *
 * That option is gone -- the store is a separate process now, and the real
 * client would make this test need a running module to assert that a lookup
 * table is fully populated.
 *
 * REFUSING RATHER THAN SUCCEEDING, on purpose. A stub that returned 0 would let
 * a future test believe it had read a work item and assert on the zeroed struct
 * it got back. These return failure and write nothing, so any test that starts
 * depending on them fails and comes here, instead of passing against fiction.
 */

#include "db1_client/wfe_store.h"

int db1_work_item_get(const char *work_item_id, db1_work_item_t *out)
{
   (void)work_item_id;
   (void)out;
   return -1;
}

int db1_work_item_set_worktree(const char *work_item_id, const char *worktree)
{
   (void)work_item_id;
   (void)worktree;
   return -1;
}

int db1_work_item_child_counts(const char *parent_id, int *total, int *accepted, int *failed)
{
   (void)parent_id;
   (void)total;
   (void)accepted;
   (void)failed;
   return -1;
}

int db1_lifecycle_event_add(const char *work_item_id, const char *stage, const char *kind,
                            const char *actor, const char *detail, const char *content_hash,
                            double cost)
{
   (void)work_item_id;
   (void)stage;
   (void)kind;
   (void)actor;
   (void)detail;
   (void)content_hash;
   (void)cost;
   return -1;
}

int db1_lifecycle_event_list_bounded(const char *work_item_id, db1_lifecycle_event_t **out, int max)
{
   (void)work_item_id;
   (void)max;
   /* No rows, and say so through the out-parameter too: a caller that checks
    * only the pointer must not be handed a stale one. */
   if (out)
   {
      *out = NULL;
   }
   return -1;
}
