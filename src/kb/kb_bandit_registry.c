/* kb_bandit_registry.c: the seeded bandit decision-point registry.
 *
 * Keep this table small and reviewed — it is the set of decisions the optimizer
 * is allowed to tune, not an open registration surface (see the proposal's
 * "decision-point sprawl" risk).  Adding a point here makes it appear in the
 * `aimee optimize` surface and the intelligence export automatically. */
#include "kb_bandit_registry.h"
#include <string.h>

static const kb_bandit_decision_point_t REGISTRY[] = {
    {
        .id = "kb_memory_retrieval_limit",
        .description = "Memory recall fan-out: how many facts to retrieve when the "
                       "caller gives no explicit limit.",
        .arms = {"10", "20"},
        .n_arms = 2,
        .reward_fn = "recall_sufficiency_v1",
        .status = "live",
    },
};

int kb_bandit_registry_count(void)
{
   return (int)(sizeof(REGISTRY) / sizeof(REGISTRY[0]));
}

const kb_bandit_decision_point_t *kb_bandit_registry_at(int i)
{
   if (i < 0 || i >= kb_bandit_registry_count())
      return NULL;
   return &REGISTRY[i];
}

const kb_bandit_decision_point_t *kb_bandit_registry_get(const char *id)
{
   if (!id)
      return NULL;
   for (int i = 0; i < kb_bandit_registry_count(); i++)
      if (strcmp(REGISTRY[i].id, id) == 0)
         return &REGISTRY[i];
   return NULL;
}
