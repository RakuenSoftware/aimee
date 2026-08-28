/* Unit-only fallback for code that emits content-free durable evidence.
 * Tests which inspect the event provide a strong definition that overrides
 * this weak sink; production targets never link this object. */
#include <aimee/audit/obs_bus.h>

__attribute__((weak)) void obs_bus_emit_durable_event(const char *action, const char *subject,
                                                      const char *verdict, const char *detail)
{
   (void)action;
   (void)subject;
   (void)verdict;
   (void)detail;
}
