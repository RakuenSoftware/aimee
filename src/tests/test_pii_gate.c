/* test_pii_gate.c: typed-fact §7 per-attribute PII recall gating. Pure. P5. */
#include "../headers/memory_pii_gate.h"
#include "../headers/rel_types.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
   /* turn-requests-sensitive detection. */
   assert(memory_pii_turn_requests_sensitive("what is my address again?") == 1);
   assert(memory_pii_turn_requests_sensitive("remind me of my password") == 1);
   assert(memory_pii_turn_requests_sensitive("what's my email?") == 1);
   assert(memory_pii_turn_requests_sensitive("when is my birthday") == 1);
   assert(memory_pii_turn_requests_sensitive("how are you today?") == 0);
   assert(memory_pii_turn_requests_sensitive("what do i do for work") == 0);
   assert(memory_pii_turn_requests_sensitive("") == 0);
   assert(memory_pii_turn_requests_sensitive(NULL) == 0);

   /* rel_type -> sensitivity. Known: seed lookup. Unknown: default OPEN
    * (SENS_NORMAL) so free-form extracted relations are not all withheld, except
    * names that plainly denote a credential or a regulated PII identifier. */
   assert(memory_pii_rel_sensitivity("works_for") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("also_known_as") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("age") == SENS_PII);
   assert(memory_pii_rel_sensitivity("totally_unknown_rel") == SENS_NORMAL); /* unknown -> open */
   assert(memory_pii_rel_sensitivity("favorite_food") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity(NULL) == SENS_NORMAL);
   /* unknown but obviously sensitive by name: still gated by the heuristic. */
   assert(memory_pii_rel_sensitivity("home_password") == SENS_SECRET);
   assert(memory_pii_rel_sensitivity("api_key") == SENS_SECRET);
   assert(memory_pii_rel_sensitivity("ssn") == SENS_PII);
   assert(memory_pii_rel_sensitivity("home_address") == SENS_PII);

   /* injection decision. */
   /* NORMAL: passes above the floor regardless of request; withheld below it. */
   assert(memory_pii_should_inject(SENS_NORMAL, 0.5, 0) == 1);
   assert(memory_pii_should_inject(SENS_NORMAL, 0.4, 0) == 1); /* at the floor */
   assert(memory_pii_should_inject(SENS_NORMAL, 0.3, 1) == 0); /* below floor, even if asked */
   /* PII: only when the turn explicitly asks (and above the floor). */
   assert(memory_pii_should_inject(SENS_PII, 0.9, 0) == 0);
   assert(memory_pii_should_inject(SENS_PII, 0.9, 1) == 1);
   assert(memory_pii_should_inject(SENS_PII, 0.3, 1) == 0); /* below floor */
   /* SECRET: never injected, even when asked at full confidence. */
   assert(memory_pii_should_inject(SENS_SECRET, 1.0, 1) == 0);
   /* non-finite confidence fails closed (must not slip past the floor check). */
   assert(memory_pii_should_inject(SENS_NORMAL, NAN, 1) == 0);

   printf("pii_gate: all tests passed\n");
   return 0;
}
