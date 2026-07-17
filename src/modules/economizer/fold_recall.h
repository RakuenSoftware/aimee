/* fold_recall.h: page folded content back in on re-touch (fold §4, P4).
 *
 * When the rolling fold (§1) skeletonizes old turns, the exact bodies leave the
 * prompt but their "coordinates" (paths, handle:/memory: ids) are conserved in the
 * Coordinate Closet (§2). If the agent later re-references one of those folded
 * coordinates, this module detects the re-touch and emits a bounded recall hint so
 * the agent knows the body can be paged back in on demand (via the existing
 * code_span_get / memory_get tools). A residency TTL prevents re-surfacing the
 * same coordinate every turn (anti-thrash). Pure, deterministic; the actual body
 * fetch is the caller's resolver step.
 *
 * Default-off: the caller gates on config (fold_recall_enabled). */
#ifndef DEC_FOLD_RECALL_H
#define DEC_FOLD_RECALL_H 1

#include "dstr.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FOLD_RECALL_DEFAULT_TTL_TURNS 4

   /* Page table: the coordinates that were folded away and could be paged back in,
    * each with the turn it was last surfaced (residency, -1 = never). */
   typedef struct
   {
      char **keys;
      int *last_turn;
      size_t count;
      size_t cap;
   } fold_recall_index_t;

   void fold_recall_index_init(fold_recall_index_t *ix);
   void fold_recall_index_free(fold_recall_index_t *ix);

   /* Add a recall key (path / handle:id / memory:id) if not already present.
    * Copies the string. Empty/NULL keys are ignored. */
   void fold_recall_index_add(fold_recall_index_t *ix, const char *key);

   /* Scan `turn_text` for any indexed key whose body is worth paging back in now:
    * the key appears in the text AND it has not been surfaced within `ttl_turns`
    * (turn - last_turn >= ttl_turns, or never). For each such key, append a bounded
    * recall-hint line to `out` (if non-NULL) and stamp last_turn = turn. Keys are
    * considered in index order (deterministic). ttl_turns <= 0 -> default.
    * Returns the number of keys surfaced. */
   size_t fold_recall_detect(fold_recall_index_t *ix, const char *turn_text, int turn,
                             int ttl_turns, dstr_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_FOLD_RECALL_H */
