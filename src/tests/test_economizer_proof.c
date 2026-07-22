#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "economizer_proof.h"

#define PASS(name) printf("  PASS: %s\n", name)

static econ_registry_key_t valid_key(void)
{
   econ_registry_key_t key = {
       .provider = ECON_PROVIDER_OPENAI,
       .endpoint_id = 1,
       .model_snapshot_id = 2,
       .tokenizer_id = 3,
       .pricing_table_id = 4,
       .contract_versions = 5,
       .transform_id = 6,
       .transform_version = 7,
   };
   return key;
}

static econ_proof_t valid_proof(void)
{
   econ_proof_t proof;
   memset(&proof, 0, sizeof(proof));
   proof.tenant_id = 10;
   proof.account_id = 11;
   proof.call_id = 12;
   proof.registry_generation = 9;
   proof.transform = valid_key();
   proof.scenario_count = 1;
   proof.safety_margin = 5;
   proof.scenarios[0].baseline_lower = 100;
   proof.scenarios[0].baseline_upper = 120;
   proof.scenarios[0].candidate_lower = 70;
   proof.scenarios[0].candidate_upper = 90;
   return proof;
}

static void test_production_registry_is_empty(void)
{
   assert(econ_registry_entry_count() == 0);
   assert(econ_registry_signature_valid() == 1);

   econ_proof_t proof = valid_proof();
   proof.registry_generation = econ_registry_generation();
   econ_proof_result_t result = econ_proof_evaluate(&proof);
   assert(result.decision == ECON_PASS_THROUGH);
   assert(result.reason == ECON_REASON_REGISTRY_ABSENT);
   PASS("production_registry_is_empty");
}

static void test_strict_cost_gate(void)
{
   econ_proof_t proof = valid_proof();

   /* Pin the exact strict boundary: 94 + 5 < 100, but 95 + 5 == 100. */
   proof.scenarios[0].candidate_upper = 94;
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_PROVEN);
   assert(result.reason == ECON_REASON_PROOF_ACCEPTED);

   proof.scenarios[0].candidate_upper = 95;
   result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_REJECTED);
   assert(result.reason == ECON_REASON_NOT_STRICTLY_CHEAPER);

   PASS("strict_cost_gate");
}

static void test_every_scenario_must_win(void)
{
   econ_proof_t proof = valid_proof();
   proof.scenario_count = 2;
   proof.scenarios[1] = proof.scenarios[0];
   proof.scenarios[1].candidate_upper = 95;
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_REJECTED);
   assert(result.reason == ECON_REASON_NOT_STRICTLY_CHEAPER);
   PASS("every_scenario_must_win");
}

static void test_overflow_and_invalid_bounds_are_indeterminate(void)
{
   econ_proof_t proof = valid_proof();
   proof.scenarios[0].candidate_upper = INT64_MAX;
   proof.scenarios[0].candidate_lower = INT64_MAX;
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_INDETERMINATE);
   assert(result.reason == ECON_REASON_MONEY_OVERFLOW);

   proof = valid_proof();
   proof.scenarios[0].baseline_lower = 121;
   result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_INDETERMINATE);
   assert(result.reason == ECON_REASON_INVALID_MONEY_BOUND);
   PASS("overflow_and_invalid_bounds_are_indeterminate");
}

static void test_registry_generation_denies(void)
{
   econ_proof_t proof = valid_proof();
   assert(proof.registry_generation != econ_registry_generation());
   econ_proof_result_t result = econ_proof_evaluate(&proof);
   assert(result.decision == ECON_PASS_THROUGH);
   assert(result.reason == ECON_REASON_REGISTRY_STALE);
   PASS("registry_generation_denies");
}

int main(void)
{
   printf("economizer_proof tests:\n");
   test_production_registry_is_empty();
   test_strict_cost_gate();
   test_every_scenario_must_win();
   test_overflow_and_invalid_bounds_are_indeterminate();
   test_registry_generation_denies();
   printf("ALL PASS\n");
   return 0;
}
