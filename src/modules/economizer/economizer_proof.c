/* economizer_proof.c: pure checked proof gate; no provider or network I/O. */
#include "economizer_proof.h"

#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define ECON_PRODUCTION_REGISTRY_GENERATION 1u

typedef struct
{
   econ_registry_key_t key;
} econ_registry_entry_t;

typedef struct
{
   uint64_t generation;
   const econ_registry_entry_t *entries;
   size_t entry_count;
} econ_registry_t;

static const econ_registry_t production_registry = {
    .generation = ECON_PRODUCTION_REGISTRY_GENERATION,
    .entries = NULL,
    .entry_count = 0,
};

/* Signed release artifact for the initial empty registry. The verifier below
 * rejects non-empty registries until canonical tuple serialization is added by
 * a separately reviewed change. */
static const unsigned char registry_manifest[] = "AIMEE-ECONOMIZER-REGISTRY-V1\n"
                                                 "generation=1\n"
                                                 "entry_count=0\n";
static const unsigned char registry_public_key[32] = {
    0x58, 0x72, 0x3b, 0x81, 0x63, 0xad, 0x3c, 0xc7, 0x86, 0x71, 0xb7, 0x94, 0x93, 0xf7, 0xba, 0x91,
    0x02, 0xff, 0xf9, 0xb8, 0x4e, 0x32, 0xec, 0x87, 0xb3, 0xee, 0x12, 0x38, 0xeb, 0xb6, 0x93, 0x44,
};
static const unsigned char registry_signature[64] = {
    0x00, 0x56, 0x8d, 0xcf, 0xb2, 0x15, 0xc7, 0x6c, 0x55, 0xe4, 0x86, 0xe4, 0xbc, 0x2c, 0x52, 0x4a,
    0xb5, 0x86, 0x32, 0xdf, 0xaa, 0x28, 0x94, 0xba, 0x88, 0xb5, 0xfb, 0x0d, 0xd9, 0x0b, 0xf9, 0x64,
    0x88, 0x4a, 0x2c, 0x40, 0x6d, 0x7b, 0xeb, 0x91, 0x00, 0x0b, 0xb2, 0x0d, 0x78, 0x74, 0x42, 0x31,
    0x74, 0xb6, 0x92, 0x2c, 0x15, 0x3d, 0x55, 0xaf, 0x50, 0x99, 0xff, 0x67, 0xd8, 0x58, 0xa0, 0x07,
};

static econ_proof_result_t result(econ_decision_t decision, econ_reason_t reason)
{
   econ_proof_result_t out = {.decision = decision, .reason = reason};
   return out;
}

static econ_cost_result_t cost_result(econ_cost_verdict_t verdict, econ_reason_t reason)
{
   econ_cost_result_t out = {.verdict = verdict, .reason = reason};
   return out;
}

static int key_equal(const econ_registry_key_t *a, const econ_registry_key_t *b)
{
   return a->provider == b->provider && a->endpoint_id == b->endpoint_id &&
          a->model_snapshot_id == b->model_snapshot_id && a->tokenizer_id == b->tokenizer_id &&
          a->pricing_table_id == b->pricing_table_id &&
          a->contract_versions == b->contract_versions && a->transform_id == b->transform_id &&
          a->transform_version == b->transform_version;
}

static int key_valid(const econ_registry_key_t *key)
{
   if (!key)
      return 0;
   if (key->provider != ECON_PROVIDER_OPENAI && key->provider != ECON_PROVIDER_ANTHROPIC)
      return 0;
   return key->endpoint_id != 0 && key->model_snapshot_id != 0 && key->tokenizer_id != 0 &&
          key->pricing_table_id != 0 && key->contract_versions != 0 && key->transform_id != 0 &&
          key->transform_version != 0;
}

static int money_add(econ_money_t a, econ_money_t b, econ_money_t *out)
{
   if (!out || a < 0 || b < 0 || a > INT64_MAX - b)
      return -1;
   *out = a + b;
   return 0;
}

uint64_t econ_registry_generation(void)
{
   return production_registry.generation;
}

size_t econ_registry_entry_count(void)
{
   return production_registry.entry_count;
}

int econ_registry_signature_valid(void)
{
   /* The current signature covers an empty registry only. This explicit guard
    * prevents future entries from inheriting trust without tuple signing. */
   if (production_registry.generation != ECON_PRODUCTION_REGISTRY_GENERATION ||
       production_registry.entry_count != 0 || production_registry.entries != NULL)
      return 0;

   unsigned char canonical[128];
   int n = snprintf((char *)canonical, sizeof(canonical),
                    "AIMEE-ECONOMIZER-REGISTRY-V1\ngeneration=%llu\nentry_count=%zu\n",
                    (unsigned long long)production_registry.generation,
                    production_registry.entry_count);
   if (n < 0 || (size_t)n != sizeof(registry_manifest) - 1 ||
       memcmp(canonical, registry_manifest, sizeof(registry_manifest) - 1) != 0)
      return 0;

   EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, registry_public_key,
                                               sizeof(registry_public_key));
   EVP_MD_CTX *ctx = key ? EVP_MD_CTX_new() : NULL;
   int ok = ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
            EVP_DigestVerify(ctx, registry_signature, sizeof(registry_signature), registry_manifest,
                             sizeof(registry_manifest) - 1) == 1;
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(key);
   return ok ? 1 : 0;
}

static int registry_contains(const econ_registry_t *registry, const econ_registry_key_t *key)
{
   if (!registry || !key_valid(key))
      return 0;
   if (registry->entry_count > 0 && !registry->entries)
      return 0;
   for (size_t i = 0; i < registry->entry_count; i++)
      if (key_equal(&registry->entries[i].key, key))
         return 1;
   return 0;
}

static econ_proof_result_t validate_proof(const econ_proof_t *proof)
{
   if (!proof)
      return result(ECON_INDETERMINATE, ECON_REASON_INVALID_ARGUMENT);
   if (proof->tenant_id == 0 || proof->account_id == 0 || proof->call_id == 0 ||
       !key_valid(&proof->transform))
      return result(ECON_INDETERMINATE, ECON_REASON_INVALID_IDENTITY);
   if (proof->scenario_count == 0 || proof->scenario_count > ECON_MAX_SCENARIOS)
      return result(ECON_INDETERMINATE, ECON_REASON_INVALID_SCENARIO_COUNT);
   if (proof->safety_margin < 0)
      return result(ECON_INDETERMINATE, ECON_REASON_INVALID_MONEY_BOUND);
   return result(ECON_INTERVENE, ECON_REASON_PROOF_ACCEPTED);
}

econ_cost_result_t econ_proof_cost_evaluate(const econ_proof_t *proof)
{
   econ_proof_result_t valid = validate_proof(proof);
   if (valid.decision != ECON_INTERVENE)
      return cost_result(ECON_COST_INDETERMINATE, valid.reason);

   for (size_t i = 0; i < proof->scenario_count; i++)
   {
      const econ_scenario_t *scenario = &proof->scenarios[i];
      if (scenario->baseline_lower < 0 || scenario->baseline_upper < 0 ||
          scenario->candidate_lower < 0 || scenario->candidate_upper < 0 ||
          scenario->baseline_lower > scenario->baseline_upper ||
          scenario->candidate_lower > scenario->candidate_upper)
         return cost_result(ECON_COST_INDETERMINATE, ECON_REASON_INVALID_MONEY_BOUND);

      econ_money_t candidate_with_margin = 0;
      if (money_add(scenario->candidate_upper, proof->safety_margin, &candidate_with_margin) != 0)
         return cost_result(ECON_COST_INDETERMINATE, ECON_REASON_MONEY_OVERFLOW);
      if (candidate_with_margin >= scenario->baseline_lower)
         return cost_result(ECON_COST_REJECTED, ECON_REASON_NOT_STRICTLY_CHEAPER);
   }

   return cost_result(ECON_COST_PROVEN, ECON_REASON_PROOF_ACCEPTED);
}

econ_proof_result_t econ_proof_evaluate(const econ_proof_t *proof)
{
   econ_proof_result_t valid = validate_proof(proof);
   if (valid.decision != ECON_INTERVENE)
      return valid;
   if (!econ_registry_signature_valid())
      return result(ECON_INDETERMINATE, ECON_REASON_REGISTRY_UNVERIFIED);
   if (proof->registry_generation != production_registry.generation)
      return result(ECON_PASS_THROUGH, ECON_REASON_REGISTRY_STALE);
   if (!registry_contains(&production_registry, &proof->transform))
      return result(ECON_PASS_THROUGH, ECON_REASON_REGISTRY_ABSENT);

   econ_cost_result_t cost = econ_proof_cost_evaluate(proof);
   if (cost.verdict == ECON_COST_PROVEN)
      return result(ECON_INTERVENE, cost.reason);
   if (cost.verdict == ECON_COST_REJECTED)
      return result(ECON_PASS_THROUGH, cost.reason);
   return result(ECON_INDETERMINATE, cost.reason);
}

const char *econ_decision_str(econ_decision_t decision)
{
   switch (decision)
   {
   case ECON_PASS_THROUGH:
      return "pass_through";
   case ECON_INTERVENE:
      return "intervene";
   case ECON_INDETERMINATE:
      return "indeterminate";
   }
   return "unknown";
}

const char *econ_reason_str(econ_reason_t reason)
{
   switch (reason)
   {
   case ECON_REASON_NONE:
      return "none";
   case ECON_REASON_INVALID_ARGUMENT:
      return "invalid_argument";
   case ECON_REASON_INVALID_IDENTITY:
      return "invalid_identity";
   case ECON_REASON_INVALID_SCENARIO_COUNT:
      return "invalid_scenario_count";
   case ECON_REASON_INVALID_MONEY_BOUND:
      return "invalid_money_bound";
   case ECON_REASON_MONEY_OVERFLOW:
      return "money_overflow";
   case ECON_REASON_REGISTRY_UNVERIFIED:
      return "registry_unverified";
   case ECON_REASON_REGISTRY_STALE:
      return "registry_stale";
   case ECON_REASON_REGISTRY_ABSENT:
      return "registry_absent";
   case ECON_REASON_NOT_STRICTLY_CHEAPER:
      return "not_strictly_cheaper";
   case ECON_REASON_UNSUPPORTED_ENDPOINT:
      return "unsupported_endpoint";
   case ECON_REASON_MODEL_NOT_PINNED:
      return "model_not_pinned";
   case ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT:
      return "tokenizer_not_local_exact";
   case ECON_REASON_REMOTE_TOKEN_COUNT:
      return "remote_token_count";
   case ECON_REASON_INVALID_REQUEST_SHAPE:
      return "invalid_request_shape";
   case ECON_REASON_UNSUPPORTED_CACHE_LAYOUT:
      return "unsupported_cache_layout";
   case ECON_REASON_INVALID_CACHE_CONTROL:
      return "invalid_cache_control";
   case ECON_REASON_PROTECTED_PREFIX_UNPROVEN:
      return "protected_prefix_unproven";
   case ECON_REASON_TOKEN_GUARD_BAND:
      return "token_guard_band";
   case ECON_REASON_OUTPUT_BOUND_UNAVAILABLE:
      return "output_bound_unavailable";
   case ECON_REASON_PRICING_UNAVAILABLE:
      return "pricing_unavailable";
   case ECON_REASON_PROOF_ACCEPTED:
      return "proof_accepted";
   }
   return "unknown";
}
