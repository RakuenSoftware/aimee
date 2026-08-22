/* test_fidelity_check.c: auditable-correctness P3 fidelity-check eligibility —
 * the fail-closed gate (fidelity_check_enabled AND its hard dependencies).
 *
 * fidelity_check_eligible() reads config through accessors rather than taking a
 * legacy_config_record, so this suite (which links no config module) supplies them. The
 * three flags are the same three the cases always set, by the same names. The
 * old "NULL cfg" case becomes config_present() reporting 0 -- the same question,
 * can the gate see config at all, asked without a struct. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fidelity_check.h"

static int g_present = 1;
static int g_fidelity_check_enabled;
static int g_kb_evidence_emit_enabled;
static int g_ingress_preinject_enabled;

int config_present(void)
{
   return g_present;
}

int config_fidelity_check_enabled(void)
{
   return g_fidelity_check_enabled;
}

int config_kb_evidence_emit_enabled(void)
{
   return g_kb_evidence_emit_enabled;
}

int config_ingress_preinject_enabled(void)
{
   return g_ingress_preinject_enabled;
}

int main(void)
{
   /* config unreadable → ineligible (fail-closed). Was: NULL cfg. */
   g_present = 0;
   assert(fidelity_check_eligible() == 0);
   g_present = 1;

   /* all off → ineligible */
   assert(fidelity_check_eligible() == 0);

   /* master flag on but BOTH dependencies off → still ineligible */
   g_fidelity_check_enabled = 1;
   assert(fidelity_check_eligible() == 0);

   /* one dependency on, the other off → still ineligible (needs both) */
   g_kb_evidence_emit_enabled = 1;
   assert(fidelity_check_eligible() == 0);

   /* master + both dependencies on → eligible (helper returns the 0/1 boolean) */
   g_ingress_preinject_enabled = 1;
   assert(fidelity_check_eligible() == 1);

   /* master flag back off → ineligible even with deps on (the flag still gates) */
   g_fidelity_check_enabled = 0;
   assert(fidelity_check_eligible() == 0);

   /* master on but one dep cleared (ingress still on) → fail-closed */
   g_fidelity_check_enabled = 1;
   g_kb_evidence_emit_enabled = 0;
   assert(fidelity_check_eligible() == 0);

   printf("fidelity_check: all tests passed\n");
   return 0;
}
