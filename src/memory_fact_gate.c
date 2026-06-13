/* memory_fact_gate.c: pure type-validation core of the typed-fact write gate.
 * See memory_fact_gate.h. The DB-backed commit lives in db2/rel_types_store.c. */
#include "memory_fact_gate.h"

fact_gate_verdict_t memory_fact_gate_check(memory_node_kind_t head_kind, const char *rel_type,
                                           memory_node_kind_t tail_kind,
                                           const rel_type_def_t **matched)
{
   if (matched)
      *matched = NULL;
   if (!rel_type || !rel_type[0])
      return FACT_GATE_BADARG;

   const rel_type_def_t *def = rel_types_seed_lookup(rel_type);
   if (!def)
      return FACT_GATE_NOVEL; /* caller consults the live ontology: stage or defer */

   if (matched)
      *matched = def;
   if (!rel_type_kind_allowed(def, 1, head_kind) || !rel_type_kind_allowed(def, 0, tail_kind))
      return FACT_GATE_REJECT_KIND;
   return FACT_GATE_ACCEPT;
}

const char *fact_gate_verdict_to_text(fact_gate_verdict_t v)
{
   switch (v)
   {
   case FACT_GATE_ACCEPT:
      return "accept";
   case FACT_GATE_REJECT_KIND:
      return "reject_kind";
   case FACT_GATE_NOVEL:
      return "novel";
   case FACT_GATE_BADARG:
      return "bad_argument";
   }
   return "unknown";
}
