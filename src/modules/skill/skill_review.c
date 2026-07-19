/* skill_review.c: background skill review nudge helpers.
 *
 * Provides skill_review_should_fire() — a pure predicate used by the
 * server hook handler to decide whether to spawn a review delegate.
 * Kept server-context-free so it is easily unit-tested.
 *
 * Integration: server.c calls skill_review_should_fire(hook_call_count, nudge_interval)
 * and invokes server_compute_skill_review_async() when it returns 1.
 */
#include "skill_review.h"

int skill_review_should_fire(int hook_call_count, int nudge_interval)
{
   if (nudge_interval <= 0 || hook_call_count <= 0)
      return 0;
   return (hook_call_count % nudge_interval) == 0;
}
