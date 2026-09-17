/* Deterministic in-process stand-in for memory-module policy in DB2 unit tests. */
#ifndef AIMEE_TEST_MEMORY_POLICY_STUB_H
#define AIMEE_TEST_MEMORY_POLICY_STUB_H

#include "modules/memory/memory_fact_gate.h"

#include <string.h>

static inline int test_memory_fact_gate(int head_kind, const char *rel_type, int tail_kind,
                                        int *verdict, int *commit_allowed)
{
   if (!verdict || !commit_allowed)
      return -1;
   if (!rel_type || !rel_type[0])
      *verdict = FACT_GATE_BADARG;
   else
   {
      const rel_type_def_t *def = rel_types_seed_lookup(rel_type);
      *verdict = !def                          ? FACT_GATE_NOVEL
                 : !rel_type_kind_allowed(def, 1, (memory_node_kind_t)head_kind) ||
                           !rel_type_kind_allowed(def, 0, (memory_node_kind_t)tail_kind)
                     ? FACT_GATE_REJECT_KIND
                     : FACT_GATE_ACCEPT;
   }
   const rel_type_def_t *definition = rel_type ? rel_types_seed_lookup(rel_type) : NULL;
   int secret = rel_type && (strcmp(rel_type, "api_key") == 0 || strcmp(rel_type, "password") == 0 ||
                            strcmp(rel_type, "token") == 0 || strcmp(rel_type, "private_key") == 0);
   *commit_allowed = (*verdict == FACT_GATE_ACCEPT || *verdict == FACT_GATE_NOVEL) &&
                     !secret && (!definition || definition->sensitivity != SENS_SECRET);
   return 0;
}

static inline void test_memory_policy_register(void)
{
   aimee_db2_register_fact_gate_provider(test_memory_fact_gate);
}

#endif
