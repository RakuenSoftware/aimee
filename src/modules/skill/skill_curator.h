/* skill_curator.h: weekly skill cluster analysis and consolidation planner.
 *
 * Phase 4 of the agent-self-improvement-skill-lifecycle proposal.
 *
 * Public entry point: skill_curator_maybe() — call from a background thread
 * or cron tick.  Implements idle guard via the maintenance_state table so
 * callers can invoke it on every hook without burning cycles.
 *
 * Curator never deletes skills; archive is the maximum action it may propose.
 * Pinned skills are excluded from all consolidation suggestions.
 * Skills with created_by != "agent" are excluded from automated merge suggestions.
 */
#ifndef DEC_SKILL_CURATOR_H
#define DEC_SKILL_CURATOR_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run the curator cycle iff the configured interval has elapsed since the
    * last run.  Returns 0 on success (including no-op skips).  Thread-safe. */
   int skill_curator_maybe(void);

   /* Thread-safe snapshot metrics for the dashboard card.
    *   runs:      total curator runs (including skips due to idle guard)
    *   skips:     runs that hit the idle guard
    *   clusters:  total consolidation clusters ever generated */
   void skill_curator_metrics(int64_t *runs, int64_t *skips, int64_t *clusters);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SKILL_CURATOR_H */
