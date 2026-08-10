/* memory_fact_gate.c: pure type-validation core of the typed-fact write gate.
 * See memory_fact_gate.h. The DB-backed commit lives in db2/rel_types_store.c. */
#include "memory_fact_gate.h"

static memory_fact_gate_checker_fn g_checker;

void memory_fact_gate_register_checker(memory_fact_gate_checker_fn checker)
{
   g_checker = checker;
}

fact_gate_verdict_t memory_fact_gate_check(memory_node_kind_t head_kind, const char *rel_type,
                                           memory_node_kind_t tail_kind,
                                           const rel_type_def_t **matched)
{
   if (matched)
      *matched = NULL;

   /* The seed lookup still runs under a registered checker: it is what fills
    * *matched, which the module has no way to return. The verdict is the
    * module's; this is only the definition the caller reads alongside it. */
   const rel_type_def_t *def = (rel_type && rel_type[0]) ? rel_types_seed_lookup(rel_type) : NULL;
   if (g_checker)
   {
      int verdict = 0;
      if (g_checker(head_kind, rel_type, tail_kind, &verdict) != 0)
         return FACT_GATE_DEFER; /* no verdict: never write, let the caller retry */
      if (matched && verdict != FACT_GATE_NOVEL && verdict != FACT_GATE_BADARG)
         *matched = def;
      return (fact_gate_verdict_t)verdict;
   }

   if (!rel_type || !rel_type[0])
      return FACT_GATE_BADARG;
   if (!def)
      return FACT_GATE_NOVEL; /* caller consults the live ontology: stage or defer */

   if (matched)
      *matched = def;
   if (!rel_type_kind_allowed(def, 1, head_kind) || !rel_type_kind_allowed(def, 0, tail_kind))
      return FACT_GATE_REJECT_KIND;
   return FACT_GATE_ACCEPT;
}
