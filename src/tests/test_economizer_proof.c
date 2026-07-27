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
       .scenario_set_id = 8,
       .scenario_coverage = 1,
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
   proof.scenarios[0].scenario_id = 0;
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
   proof.transform.scenario_coverage = 3;
   proof.scenarios[1] = proof.scenarios[0];
   proof.scenarios[1].scenario_id = 1;
   proof.scenarios[1].candidate_upper = 95;
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_REJECTED);
   assert(result.reason == ECON_REASON_NOT_STRICTLY_CHEAPER);
   PASS("every_scenario_must_win");
}

static void test_scenario_coverage_is_signed_and_exhaustive(void)
{
   econ_proof_t proof = valid_proof();
   proof.transform.scenario_coverage = 3;
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_INDETERMINATE);
   assert(result.reason == ECON_REASON_INVALID_SCENARIO_COVERAGE);

   proof.scenario_count = 2;
   proof.scenarios[1] = proof.scenarios[0];
   result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_INDETERMINATE);
   assert(result.reason == ECON_REASON_INVALID_SCENARIO_COVERAGE);

   proof.scenarios[1].scenario_id = 1;
   result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_PROVEN);
   PASS("scenario_coverage_is_signed_and_exhaustive");
}

static void test_global_cross_scenario_bound_is_conservative(void)
{
   econ_proof_t proof = valid_proof();
   proof.scenario_count = 2;
   proof.transform.scenario_coverage = 3;
   proof.scenarios[0].baseline_lower = 200;
   proof.scenarios[0].baseline_upper = 200;
   proof.scenarios[0].candidate_lower = 150;
   proof.scenarios[0].candidate_upper = 190;
   proof.scenarios[1] = proof.scenarios[0];
   proof.scenarios[1].scenario_id = 1;
   proof.scenarios[1].baseline_lower = 100;
   proof.scenarios[1].baseline_upper = 100;
   proof.scenarios[1].candidate_lower = 50;
   proof.scenarios[1].candidate_upper = 90;
   /* Both paired scenarios win, but max(candidate)+margin is not below
    * min(baseline), so the deliberately stronger global gate rejects. */
   econ_cost_result_t result = econ_proof_cost_evaluate(&proof);
   assert(result.verdict == ECON_COST_REJECTED);
   assert(result.reason == ECON_REASON_NOT_STRICTLY_CHEAPER);
   PASS("global_cross_scenario_bound_is_conservative");
}

static void test_registry_artifact_rejects_single_byte_tampering(void)
{
   unsigned char manifest[] = "AIMEE-ECONOMIZER-REGISTRY-V1\n"
                              "generation=1\n"
                              "entry_count=0\n";
   unsigned char public_key[32] = {0x58, 0x72, 0x3b, 0x81, 0x63, 0xad, 0x3c, 0xc7, 0x86, 0x71, 0xb7,
                                   0x94, 0x93, 0xf7, 0xba, 0x91, 0x02, 0xff, 0xf9, 0xb8, 0x4e, 0x32,
                                   0xec, 0x87, 0xb3, 0xee, 0x12, 0x38, 0xeb, 0xb6, 0x93, 0x44};
   unsigned char signature[64] = {0x00, 0x56, 0x8d, 0xcf, 0xb2, 0x15, 0xc7, 0x6c, 0x55, 0xe4, 0x86,
                                  0xe4, 0xbc, 0x2c, 0x52, 0x4a, 0xb5, 0x86, 0x32, 0xdf, 0xaa, 0x28,
                                  0x94, 0xba, 0x88, 0xb5, 0xfb, 0x0d, 0xd9, 0x0b, 0xf9, 0x64, 0x88,
                                  0x4a, 0x2c, 0x40, 0x6d, 0x7b, 0xeb, 0x91, 0x00, 0x0b, 0xb2, 0x0d,
                                  0x78, 0x74, 0x42, 0x31, 0x74, 0xb6, 0x92, 0x2c, 0x15, 0x3d, 0x55,
                                  0xaf, 0x50, 0x99, 0xff, 0x67, 0xd8, 0x58, 0xa0, 0x07};
   size_t manifest_len = sizeof(manifest) - 1;
   assert(econ_registry_artifact_signature_valid(manifest, manifest_len, public_key,
                                                 sizeof(public_key), signature,
                                                 sizeof(signature)) == 1);
   manifest[0] ^= 1;
   assert(econ_registry_artifact_signature_valid(manifest, manifest_len, public_key,
                                                 sizeof(public_key), signature,
                                                 sizeof(signature)) == 0);
   manifest[0] ^= 1;
   public_key[0] ^= 1;
   assert(econ_registry_artifact_signature_valid(manifest, manifest_len, public_key,
                                                 sizeof(public_key), signature,
                                                 sizeof(signature)) == 0);
   public_key[0] ^= 1;
   signature[0] ^= 1;
   assert(econ_registry_artifact_signature_valid(manifest, manifest_len, public_key,
                                                 sizeof(public_key), signature,
                                                 sizeof(signature)) == 0);
   PASS("registry_artifact_rejects_single_byte_tampering");
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

static void test_registry_tuple_serialization_is_complete(void)
{
   econ_registry_key_t key = valid_key();
   char tuple[512];
   size_t n = 0;
   assert(econ_registry_key_serialize(&key, tuple, sizeof(tuple), &n) == 0);
   assert(n == strlen(tuple));
   assert(strcmp(tuple, "provider=1,endpoint=1,model=2,tokenizer=3,pricing=4,contracts=5,"
                        "transform=6,version=7,scenario_set=8,coverage=1\n") == 0);
   key.transform_version = 0;
   assert(econ_registry_key_serialize(&key, tuple, sizeof(tuple), &n) == -1);
   PASS("registry_tuple_serialization_is_complete");
}

int main(void)
{
   printf("economizer_proof tests:\n");
   test_production_registry_is_empty();
   test_strict_cost_gate();
   test_every_scenario_must_win();
   test_scenario_coverage_is_signed_and_exhaustive();
   test_global_cross_scenario_bound_is_conservative();
   test_registry_artifact_rejects_single_byte_tampering();
   test_overflow_and_invalid_bounds_are_indeterminate();
   test_registry_generation_denies();
   test_registry_tuple_serialization_is_complete();
   printf("ALL PASS\n");
   return 0;
}
