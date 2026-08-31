/* wfe_store_lists.c: the unbounded list names, on the caller's side.
 *
 * A reply crossing the module boundary has to have a ceiling. These three
 * listings never declared one -- they returned as many rows as existed, which
 * was a bound only by accident. The served operations take one explicitly; these
 * keep the names their 45 call sites already use and pass it.
 *
 * The ceiling is the same number the wire enforces, so a caller cannot ask for
 * more than a reply can carry and then be handed a silently short list.
 */
#include "db1_client/wfe_store.h"

int db1_work_item_list(db1_work_item_t **out)
{
   return db1_work_item_list_bounded(out, DB1_WORK_ITEM_LIST_MAX);
}

int db1_work_item_list_lru(db1_work_item_t **out)
{
   return db1_work_item_list_lru_bounded(out, DB1_WORK_ITEM_LIST_MAX);
}

int db1_lifecycle_event_list(const char *work_item_id, db1_lifecycle_event_t **out)
{
   return db1_lifecycle_event_list_bounded(work_item_id, out, DB1_LIFECYCLE_EVENT_MAX);
}
