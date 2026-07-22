/* economizer_proof.h: provider-neutral proof gate for provider-specific planners.
 *
 * This module does not estimate tokens, prices, or cache outcomes. OpenAI and
 * Anthropic planners must produce their own closed scenario sets. The shared
 * gate only checks that every candidate upper bound is strictly below the
 * corresponding baseline lower bound and that the exact transform tuple is in
 * the verified registry.
 *
 * The production registry is intentionally empty. Consequently, production
 * calls can only pass through until a separately reviewed registry is shipped. */
#ifndef DEC_ECONOMIZER_PROOF_H
#define DEC_ECONOMIZER_PROOF_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef int64_t econ_money_t;

   typedef enum
   {
      ECON_PASS_THROUGH = 0,
      ECON_INTERVENE,
      ECON_INDETERMINATE
   } econ_decision_t;

   typedef enum
   {
      ECON_REASON_NONE = 0,
      ECON_REASON_INVALID_ARGUMENT,
      ECON_REASON_INVALID_IDENTITY,
      ECON_REASON_INVALID_SCENARIO_COUNT,
      ECON_REASON_INVALID_MONEY_BOUND,
      ECON_REASON_MONEY_OVERFLOW,
      ECON_REASON_REGISTRY_UNVERIFIED,
      ECON_REASON_REGISTRY_STALE,
      ECON_REASON_REGISTRY_ABSENT,
      ECON_REASON_NOT_STRICTLY_CHEAPER,
      ECON_REASON_UNSUPPORTED_ENDPOINT,
      ECON_REASON_MODEL_NOT_PINNED,
      ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT,
      ECON_REASON_REMOTE_TOKEN_COUNT,
      ECON_REASON_INVALID_REQUEST_SHAPE,
      ECON_REASON_UNSUPPORTED_CACHE_LAYOUT,
      ECON_REASON_INVALID_CACHE_CONTROL,
      ECON_REASON_PROTECTED_PREFIX_UNPROVEN,
      ECON_REASON_TOKEN_GUARD_BAND,
      ECON_REASON_OUTPUT_BOUND_UNAVAILABLE,
      ECON_REASON_PRICING_UNAVAILABLE,
      ECON_REASON_PROOF_ACCEPTED
   } econ_reason_t;

   typedef enum
   {
      ECON_PROVIDER_OPENAI = 1,
      ECON_PROVIDER_ANTHROPIC = 2
   } econ_provider_t;

   typedef enum
   {
      ECON_TOKEN_SOURCE_NONE = 0,
      ECON_TOKEN_SOURCE_LOCAL_EXACT,
      ECON_TOKEN_SOURCE_REMOTE_ESTIMATE
   } econ_token_source_t;

   typedef struct
   {
      uint64_t ordinary_input;
      uint64_t cached_read_input;
      uint64_t cache_write_input;
      uint64_t output;
   } econ_token_buckets_t;

   typedef struct
   {
      uint32_t cache_outcome;
      econ_token_buckets_t baseline_lower_tokens;
      econ_token_buckets_t baseline_upper_tokens;
      econ_token_buckets_t candidate_lower_tokens;
      econ_token_buckets_t candidate_upper_tokens;
      econ_money_t baseline_lower;
      econ_money_t baseline_upper;
      econ_money_t candidate_lower;
      econ_money_t candidate_upper;
   } econ_scenario_t;

   typedef struct
   {
      econ_provider_t provider;
      uint32_t endpoint_id;
      uint64_t model_snapshot_id;
      uint64_t tokenizer_id;
      uint64_t pricing_table_id;
      uint64_t contract_versions;
      uint64_t transform_id;
      uint64_t transform_version;
   } econ_registry_key_t;

#define ECON_MAX_SCENARIOS 16u

   typedef struct
   {
      uint64_t tenant_id;
      uint64_t account_id;
      uint64_t call_id;
      uint64_t registry_generation;
      econ_registry_key_t transform;
      econ_scenario_t scenarios[ECON_MAX_SCENARIOS];
      size_t scenario_count;
      econ_money_t safety_margin;
   } econ_proof_t;

   typedef struct
   {
      econ_decision_t decision;
      econ_reason_t reason;
   } econ_proof_result_t;

   /* Cost evidence is deliberately not an authorization decision. */
   typedef enum
   {
      ECON_COST_REJECTED = 0,
      ECON_COST_PROVEN,
      ECON_COST_INDETERMINATE
   } econ_cost_verdict_t;

   typedef struct
   {
      econ_cost_verdict_t verdict;
      econ_reason_t reason;
   } econ_cost_result_t;

   /* Read-only facts about the module-owned production registry. There is no
    * public registry constructor: callers cannot self-assert signature status. */
   uint64_t econ_registry_generation(void);
   size_t econ_registry_entry_count(void);
   int econ_registry_signature_valid(void);

   /* Check provider-planner cost bounds without authorizing a transform. This
    * pure helper has no ECON_INTERVENE value; only econ_proof_evaluate() can
    * produce an authorization decision. */
   econ_cost_result_t econ_proof_cost_evaluate(const econ_proof_t *proof);

   /* Evaluate one complete-call proof against the immutable module-owned
    * production registry. The initial registry is empty, so this function
    * cannot return ECON_INTERVENE in the initial release. */
   econ_proof_result_t econ_proof_evaluate(const econ_proof_t *proof);

   const char *econ_decision_str(econ_decision_t decision);
   const char *econ_reason_str(econ_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_PROOF_H */
