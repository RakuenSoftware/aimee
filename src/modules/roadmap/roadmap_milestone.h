/* roadmap_milestone.h: milestone validation gate for spec-driven-roadmap dispatch.
 *
 * Provides the review gate that runs after all child tasks of a milestone
 * complete. The gate calls a review agent with the milestone's acceptance
 * criteria and the ACs of its child tasks, then parses the verdict
 * ("pass", "partial", "fail") from the agent's text response.
 */

#ifndef DEC_ROADMAP_MILESTONE_H
#define DEC_ROADMAP_MILESTONE_H 1

#include <stddef.h>
#include "agent_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse the review delegate's text response and extract the verdict.
    * Scans for the keywords "pass", "fail", or "partial" (case-insensitive,
    * word-boundary match sufficient). Returns one of:
    *   "pass"    — review approved the milestone
    *   "fail"    — review found critical gaps
    *   "partial" — some gaps, not critical
    *   "unknown" — no verdict keyword found
    * Returns a static string; never NULL. */
   const char *roadmap_milestone_parse_verdict(const char *review_text);

   /* Check whether all plan_unit artifacts (DB2) at a given level that are
    * scoped to parent_id have state='done' in the DB1 rdm_unit_dispatch table.
    * Returns 1 if all done, 0 if not, -1 on error.
    * level is "task" (to check a slice's tasks) or "slice" (to check a
    * milestone's slices). */
   int roadmap_milestone_all_done(const char *roadmap_id, const char *parent_id, const char *level);

   /* Build the review prompt for a milestone.
    * Includes the milestone title, intent, acceptance criteria, and the
    * acceptance criteria of all its child tasks (loaded from DB2 artifacts
    * scoped to milestone_id via their slice parents). Returns heap string
    * (caller frees), or NULL on error. */
   char *roadmap_milestone_review_prompt(const char *roadmap_id, const char *milestone_id);

   /* Run the milestone validation gate. Calls agent_run with role="review",
    * parses the verdict from the response text, and returns the verdict string
    * ("pass"/"fail"/"partial"/"unknown"). On "fail" or "partial", the caller
    * should reopen the milestone as new units. Returns heap string (caller frees)
    * or NULL on agent error. cfg is the loaded agent_config_t (non-NULL). */
   char *roadmap_milestone_validate(const char *roadmap_id, const char *milestone_id,
                                    agent_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROADMAP_MILESTONE_H */