/* kb_demote.h: outcome-driven demotion maintenance runner.
 * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md */
#ifndef DEC_KB_DEMOTE_H
#define DEC_KB_DEMOTE_H 1

#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run one demotion maintenance cycle.  In shadow mode (demotion_enabled == 1),
    * scores are computed and demotion_profile artifacts are written, but no
    * lifecycle transitions are applied to memory rows.
    * Returns the number of demotion_profile artifacts written (>= 0), or -1 on
    * fatal error. Returns 0 immediately when demotion_enabled == 0. */
   int kb_demote_run(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_DEMOTE_H */
