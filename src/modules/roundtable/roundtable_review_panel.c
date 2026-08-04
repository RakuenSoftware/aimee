/* roundtable_review_panel.c: the review's panel decision, owned by the module.
 *
 * The transport asks which saved panel a request means and how long it may run.
 * Answering needs the preset store and the configured ensemble, both private to
 * this module, so the answer is produced here and only plain types cross out.
 */
#include <aimee/roundtable/review_panel.h>

#include "delegate_ensemble.h"
#include "roundtable_preset.h"

#include <assert.h>
#include <stdio.h>

/* The published buffer must be able to hold any name the store can produce. */
_Static_assert(ROUNDTABLE_REVIEW_PANEL_NAME_MAX >= RT_PRESET_NAME_MAX,
               "published review panel name buffer is smaller than the preset store's limit");

void roundtable_review_resolve_panel(const char *requested, char *name_out, size_t name_cap,
                                     int *deadline_ms_out)
{
   if (name_out && name_cap)
      name_out[0] = '\0';
   if (!deadline_ms_out)
      return;

   ensemble_panel_t panel;
   ensemble_panel_from_config(&panel);
   if (name_out && name_cap &&
       roundtable_preset_resolve_runtime(requested, &panel, name_out, name_cap, NULL, 0) > 0)
   {
      /* A configured chairman is a second full phase, so the deadline has to
       * account for it. Loading the acquired preset is the only way to know. */
      roundtable_preset_t acquired;
      int chairman = roundtable_preset_load(name_out, &acquired) == 0 ? acquired.chairman_enabled : 0;
      *deadline_ms_out = roundtable_review_deadline_ms(panel.deadline_ms, chairman);
      return;
   }
   /* No preset acquired: the module will reject the review, but the caller
    * still needs a bound rather than an unbounded wait. */
   *deadline_ms_out = roundtable_review_deadline_ms(0, 0);
}
