#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "provider_catalog.h"

/* TTL reap for the per-provider concurrency slot table: a holder that never
 * released within the TTL is reclaimed so the counter self-heals instead of
 * staying pinned until a server restart. */
int main(void)
{
   const int TTL = PROVIDER_SLOT_TTL_SEC;
   const time_t now = 1000000;

   /* nothing expired: all held recently */
   {
      time_t s[3] = {now - 10, now - 5, now};
      assert(provider_slots_reap_stamps(s, 3, now, TTL) == 3);
   }
   /* empty / zero active */
   {
      time_t s[1] = {0};
      assert(provider_slots_reap_stamps(s, 0, now, TTL) == 0);
   }
   /* the two oldest (a leaked holder + a stale one) are reaped; the live one
    * survives and is compacted to the front */
   {
      time_t s[3] = {now - (TTL + 100), now - (TTL + 1), now - 5};
      int n = provider_slots_reap_stamps(s, 3, now, TTL);
      assert(n == 1);
      assert(s[0] == now - 5); /* the live holder moved to index 0 */
   }
   /* all expired -> fully reclaimed (the permanent-leak-until-restart case) */
   {
      time_t s[3] = {now - (TTL + 1), now - (TTL + 2), now - (TTL + 3)};
      assert(provider_slots_reap_stamps(s, 3, now, TTL) == 0);
   }
   /* boundary: exactly TTL old is reaped (>=), one second short is kept */
   {
      time_t s1[1] = {now - TTL};
      assert(provider_slots_reap_stamps(s1, 1, now, TTL) == 0);
      time_t s2[1] = {now - (TTL - 1)};
      assert(provider_slots_reap_stamps(s2, 1, now, TTL) == 1);
   }

   printf("provider_slots_reap: ok\n");
   return 0;
}
