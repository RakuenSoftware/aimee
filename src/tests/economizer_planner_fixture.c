#include "economizer_planner_fixture.h"

#include "economizer_planner_internal.h"
#include <stdlib.h>
#include <string.h>

enum
{
   TEST_MODEL_SNAPSHOT_ID = 41,
   TEST_TOKENIZER_ID = 42,
   TEST_PRICING_TABLE_ID = 43,
   TEST_CONTRACT_VERSIONS = 44
};

econ_openai_context_t *econ_test_openai_context_create(const char *model,
                                                       econ_openai_prices_t prices,
                                                       uint64_t guard_tokens,
                                                       econ_money_t safety_margin)
{
   econ_openai_context_t *context = calloc(1, sizeof(*context));
   if (!context)
      return NULL;
   context->cookie = ECON_PLANNER_CONTEXT_COOKIE;
   context->pinned_model = model;
   context->model_snapshot_id = TEST_MODEL_SNAPSHOT_ID;
   context->tokenizer_id = TEST_TOKENIZER_ID;
   context->pricing_table_id = TEST_PRICING_TABLE_ID;
   context->contract_versions = TEST_CONTRACT_VERSIONS;
   context->tokenizer_guard_tokens = guard_tokens;
   context->safety_margin = safety_margin;
   context->prices = prices;
   return context;
}

econ_anthropic_context_t *econ_test_anthropic_context_create(const char *model,
                                                             econ_anthropic_prices_t prices,
                                                             int has_beta_headers,
                                                             econ_money_t safety_margin)
{
   econ_anthropic_context_t *context = calloc(1, sizeof(*context));
   if (!context)
      return NULL;
   context->cookie = ECON_PLANNER_CONTEXT_COOKIE;
   context->pinned_model = model;
   context->model_snapshot_id = TEST_MODEL_SNAPSHOT_ID;
   context->tokenizer_id = TEST_TOKENIZER_ID;
   context->pricing_table_id = TEST_PRICING_TABLE_ID;
   context->contract_versions = TEST_CONTRACT_VERSIONS;
   context->has_beta_headers = has_beta_headers;
   context->safety_margin = safety_margin;
   context->prices = prices;
   return context;
}

econ_token_evidence_t *econ_test_token_evidence_create(econ_provider_t provider,
                                                       uint32_t endpoint_id,
                                                       const char *serialized_buffer,
                                                       uint64_t input_tokens,
                                                       econ_token_source_t source)
{
   if (!serialized_buffer)
      return NULL;
   econ_token_evidence_t *evidence = calloc(1, sizeof(*evidence));
   if (!evidence)
      return NULL;
   evidence->cookie = ECON_TOKEN_EVIDENCE_COOKIE;
   evidence->provider = provider;
   evidence->endpoint_id = endpoint_id;
   evidence->model_snapshot_id = TEST_MODEL_SNAPSHOT_ID;
   evidence->tokenizer_id = TEST_TOKENIZER_ID;
   evidence->serialized_buffer = serialized_buffer;
   evidence->serialized_size = strlen(serialized_buffer);
   evidence->input_tokens = input_tokens;
   evidence->source = source;
   return evidence;
}

void econ_test_openai_context_free(econ_openai_context_t *context)
{
   free(context);
}

void econ_test_anthropic_context_free(econ_anthropic_context_t *context)
{
   free(context);
}

void econ_test_token_evidence_free(econ_token_evidence_t *evidence)
{
   free(evidence);
}
