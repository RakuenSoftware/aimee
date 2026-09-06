/* Typed-fact gate connection seam.
 *
 * Policy lives in the Go memory process. This file only forwards to the
 * event-bus provider installed by the host and fails closed when unavailable.
 */
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
   if (!g_checker)
      return FACT_GATE_DEFER;
   int verdict = FACT_GATE_DEFER;
   if (g_checker(head_kind, rel_type, tail_kind, &verdict) != 0 || verdict < FACT_GATE_ACCEPT ||
       verdict > FACT_GATE_BADARG)
      return FACT_GATE_DEFER;
   return (fact_gate_verdict_t)verdict;
}
