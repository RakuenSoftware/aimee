/* test_fidelity_check.c: auditable-correctness P3 fidelity-check eligibility —
 * the fail-closed gate (fidelity_check_enabled AND its hard dependencies). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fidelity_check.h"

int main(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof cfg);

   /* NULL cfg → ineligible (fail-closed) */
   assert(fidelity_check_eligible(NULL) == 0);

   /* all off → ineligible */
   assert(fidelity_check_eligible(&cfg) == 0);

   /* master flag on but BOTH dependencies off → still ineligible */
   cfg.fidelity_check_enabled = 1;
   assert(fidelity_check_eligible(&cfg) == 0);

   /* one dependency on, the other off → still ineligible (needs both) */
   cfg.kb_evidence_emit_enabled = 1;
   assert(fidelity_check_eligible(&cfg) == 0);

   /* master + both dependencies on → eligible (helper returns the 0/1 boolean) */
   cfg.ingress_preinject_enabled = 1;
   assert(fidelity_check_eligible(&cfg) == 1);

   /* master flag back off → ineligible even with deps on (the flag still gates) */
   cfg.fidelity_check_enabled = 0;
   assert(fidelity_check_eligible(&cfg) == 0);

   /* master on but one dep cleared (ingress still on) → fail-closed */
   cfg.fidelity_check_enabled = 1;
   cfg.kb_evidence_emit_enabled = 0;
   assert(fidelity_check_eligible(&cfg) == 0);

   printf("fidelity_check: all tests passed\n");
   return 0;
}
