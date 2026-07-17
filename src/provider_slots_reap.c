/* provider_slots_reap.c: dependency-free TTL-reap arithmetic for the
 * per-provider concurrency slot table (server_provider_slots.c). Kept in its own
 * translation unit so the arithmetic is unit-testable without dragging in the
 * whole server. */
#include "provider_catalog.h"

int provider_slots_reap_stamps(time_t *stamps, int active, time_t now, int ttl)
{
   if (active <= 0)
      return 0;
   /* stamps are kept sorted oldest-first, so all expired ones are a prefix. */
   int expired = 0;
   while (expired < active && (now - stamps[expired]) >= (time_t)ttl)
      expired++;
   if (expired <= 0)
      return active;
   int remaining = active - expired;
   for (int k = 0; k < remaining; k++)
      stamps[k] = stamps[k + expired];
   return remaining;
}
