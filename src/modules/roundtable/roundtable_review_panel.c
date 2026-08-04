/* Panel resolution for roundtable.review.
 *
 * This lived inline in src/server/roundtable_review_bus.c, which forced the
 * transport to include the optional module's private ensemble header just to
 * name a panel. The decision is roundtable's, so it lives here and the bus asks
 * for it through <aimee/roundtable/review_panel.h>.
 */
#include <aimee/roundtable/review_panel.h>

#include "delegate_ensemble.h" /* ensemble_panel_from_config */
#include "roundtable_preset.h"

/* The published buffer size is the preset store's limit. If the store ever grows
 * a longer name, this fails the build rather than silently truncating. */
_Static_assert(ROUNDTABLE_REVIEW_PANEL_NAME_MAX == RT_PRESET_NAME_MAX,
               "published panel-name buffer must match the preset store limit");

int roundtable_review_resolve_panel(const char *requested_preset, char *out, size_t out_len,
                                    int *timeout_ms)
{
   /* A published surface validates what a static helper could assume. A caller
    * that gets this wrong must not corrupt memory or leave the deadline unset. */
   if (!timeout_ms)
      return 0;
   if (!out || out_len == 0)
   {
      *timeout_ms = roundtable_review_deadline_ms(0, 0);
      return 0;
   }
   out[0] = '\0';
   ensemble_panel_t panel;
   ensemble_panel_from_config(&panel);
   if (roundtable_preset_resolve_runtime(requested_preset, &panel, out, out_len, NULL, 0) > 0)
   {
      roundtable_preset_t acquired;
      int chairman = roundtable_preset_load(out, &acquired) == 0 ? acquired.chairman_enabled : 0;
      *timeout_ms = roundtable_review_deadline_ms(panel.deadline_ms, chairman);
      return 1;
   }
   *timeout_ms = roundtable_review_deadline_ms(0, 0);
   return 0;
}
