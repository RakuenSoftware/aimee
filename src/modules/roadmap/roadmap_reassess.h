/* roadmap_reassess.h: post-completion reassessment pass for spec-driven roadmap loop.
 *
 * Provides the reassessment step after a roadmap is marked complete: builds a
 * prompt that includes the roadmap goal and all milestone acceptance criteria,
 * runs it through the reason agent, and parses the response for a completion
 * verdict.
 */

#ifndef DEC_ROADMAP_REASSESS_H
#define DEC_ROADMAP_REASSESS_H

#include <stddef.h>
#include "agent_config.h"

/* Build the reassessment prompt for a roadmap. Loads the roadmap goal
 * and all milestone acceptance criteria from DB2. Returns heap string
 * (caller frees), or NULL on error. */
char *roadmap_reassess_build_prompt(const char *roadmap_id);

/* Run the reassessment pass: calls agent_run("reason") with the
 * reassessment prompt, parses the response for completeness.
 * Writes the verdict text into *out_verdict (heap, caller frees).
 * Returns:
 *   0 — all goals met (verdict contains the reason)
 *   1 — gaps found (verdict contains a gap description)
 *  -1 — error
 * cfg must be non-NULL (pre-loaded agent_config_t). */
int roadmap_reassess_run(const char *roadmap_id, agent_config_t *cfg, char **out_verdict);

/* Parse the reassessment response text for a completion verdict.
 * Returns 1 if the response indicates "complete"/"all goals met"/
 * "no gaps"/"pass" (case-insensitive), 0 otherwise. */
int roadmap_reassess_is_complete(const char *response_text);

#endif /* DEC_ROADMAP_REASSESS_H */
