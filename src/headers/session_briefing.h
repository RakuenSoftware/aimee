#ifndef DEC_SESSION_BRIEFING_H
#define DEC_SESSION_BRIEFING_H 1

/* Session-start briefing helpers. See
 * docs/proposals/done/personal-agent-phase-2-recall.md §Step 2.
 *
 * Each helper returns a heap-allocated markdown fragment (caller
 * frees) or NULL when there is nothing to surface. A zero/negative
 * limit selects the default. The helpers are pure reads over the
 * existing prospective-memory and epistemic-directive stores; they
 * do not mutate state. */

#include <stdarg.h>

char *session_briefing_render_skill_index(const char *project_root, int limit);

#endif /* DEC_SESSION_BRIEFING_H */
