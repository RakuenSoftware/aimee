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
    {
        .id = "kb_fusion_mode",
        .description = "KB-search hybrid fusion strategy: how lexical and dense "
                       "results are combined when no explicit mode is requested.",
        .arms = {"rrf", "static_alpha", "dynamic_alpha"},
        .n_arms = 3,
        .reward_fn = "recall_sufficiency_v1",
        .status = "live",
    },
    {
        .id = "delegate_routing",
        .description = "Delegate cost-tier preference when a sub-task is launched "
                       "with no explicit --via/--tier/--provider override.",
        .arms = {"cheapest", "premium"},
        .n_arms = 2,
        .reward_fn = "delegate_success_v1",
        .status = "live",
    },
    {
        .id = "briefing_style",
        .description = "Session briefing density: compact start-of-session context "
                       "versus an evidence-heavy bundle.",
        .arms = {"compact", "evidence_heavy"},
        .n_arms = 2,
        .reward_fn = "briefing_quality_replay_v1",
        .status = "static",
    },
    {
        .id = "guardrail_strictness",
        .description = "Semantic guardrail threshold profile for write-intent tools.",
        .arms = {"balanced", "strict"},
        .n_arms = 2,
        .reward_fn = "guardrail_outcome_replay_v1",
        .status = "static",
    },
    {
        .id = "reflection_synthesis_mode",
        .description = "Idle-reflection synthesis rollout (proposal §4): keep the "
                       "candidate stream in shadow (scored, unpromoted) or promote it "
                       "to active (writes into the promotion pipeline). Declared here; "
                       "the shadow->active flip is wired separately, never a config edit.",
        .arms = {"shadow", "active"},
        .n_arms = 2,
        .reward_fn = "reflection_synthesis_quality_replay_v1",
        .status = "static",
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
