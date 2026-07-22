/* rounds_to_resume.c: post-compaction recovery accounting.
 *
 * See headers/rounds_to_resume.h for the metric definition and rationale.
 */
#include "aimee.h"
#include "rounds_to_resume.h"
#include "session_compact.h"
#include <string.h>

void rtr_begin(rtr_tracker_t *t, const session_compact_result_t *r)
{
   if (!t)
      return;
   memset(t, 0, sizeof(*t));
   if (!r || !r->compacted)
      return; /* no boundary -> nothing was lost -> nothing to recover */

   t->active = 1;
   t->sig_count = r->readonly_sig_count;
   t->sigs_dropped = r->readonly_sigs_dropped;
   for (int i = 0; i < r->readonly_sig_count && i < SESSION_COMPACT_MAX_SIGS; i++)
      memcpy(t->sigs[i], r->readonly_sigs[i], SESSION_COMPACT_SIG_LEN);
}

/* 1 if this call reproduces a lookup the agent had already made pre-boundary. */
static int is_rederivation(const rtr_tracker_t *t, const char *name, const char *args)
{
   char sig[SESSION_COMPACT_SIG_LEN];
   if (!session_compact_tool_sig(name, args, sig, sizeof(sig)))
      return 0; /* not a read-only lookup: repeating it is legitimate */

   for (int i = 0; i < t->sig_count; i++)
      if (strcmp(t->sigs[i], sig) == 0)
         return 1;
   return 0; /* a read-only call the agent had NOT made before is new work */
}

int rtr_observe_turn(rtr_tracker_t *t, const char *const *names, const char *const *args, int n)
{
   if (!t || !t->active || t->resolved || n <= 0 || !names)
      return 0;

   int rederived_here = 0;
   int progress = 0;

   for (int i = 0; i < n; i++)
   {
      if (is_rederivation(t, names[i], args ? args[i] : NULL))
         rederived_here++;
      else
         progress = 1; /* anything not a known-lookup replay moves the task on */
   }

   t->rederived_calls += rederived_here;

   if (progress)
   {
      /* First turn that does something new: the recovery is over. Rounds spent
       * re-deriving is the cost the boundary imposed. A turn that mixes replay
       * with real work still counts as progress — the agent did not stall. */
      t->resolved = 1;
      t->active = 0;
      return 1;
   }

   /* Every call this turn was a replay of something already known: a wasted
    * round, caused by the boundary dropping it. */
   t->rounds_to_resume++;
   return 0;
}
