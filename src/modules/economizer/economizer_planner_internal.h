/* Internal planner trust objects. No production constructor exists in this
 * slice. The test fixture is linked only into the two focused unit binaries. */
#ifndef DEC_ECONOMIZER_PLANNER_INTERNAL_H
#define DEC_ECONOMIZER_PLANNER_INTERNAL_H 1

#include "economizer_anthropic.h"
#include "economizer_openai.h"

#define ECON_PLANNER_CONTEXT_COOKIE UINT64_C(0x65636f6e63747831)
#define ECON_TOKEN_EVIDENCE_COOKIE  UINT64_C(0x65636f6e746f6b31)

struct econ_token_evidence
{
   uint64_t cookie;
   econ_provider_t provider;
   uint32_t endpoint_id;
   uint64_t model_snapshot_id;
   uint64_t tokenizer_id;
   const char *serialized_buffer;
   size_t serialized_size;
   uint64_t input_tokens;
   econ_token_source_t source;
};

struct econ_openai_context
{
   uint64_t cookie;
   const char *pinned_model;
   uint64_t model_snapshot_id;
   uint64_t tokenizer_id;
   uint64_t pricing_table_id;
   uint64_t contract_versions;
   uint64_t tokenizer_guard_tokens;
   econ_money_t safety_margin;
   econ_openai_prices_t prices;
};

struct econ_anthropic_context
{
   uint64_t cookie;
   const char *pinned_model;
   uint64_t model_snapshot_id;
   uint64_t tokenizer_id;
   uint64_t pricing_table_id;
   uint64_t contract_versions;
   int has_beta_headers;
   econ_money_t safety_margin;
   econ_anthropic_prices_t prices;
};

#endif /* DEC_ECONOMIZER_PLANNER_INTERNAL_H */
