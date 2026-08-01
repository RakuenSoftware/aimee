#ifndef AIMEE_TEST_ECONOMIZER_PLANNER_FIXTURE_H
#define AIMEE_TEST_ECONOMIZER_PLANNER_FIXTURE_H 1

#include "economizer_anthropic.h"
#include "economizer_openai.h"

econ_openai_context_t *econ_test_openai_context_create(const char *model,
                                                       econ_openai_prices_t prices,
                                                       uint64_t guard_tokens,
                                                       econ_money_t safety_margin);
econ_anthropic_context_t *econ_test_anthropic_context_create(const char *model,
                                                             econ_anthropic_prices_t prices,
                                                             int has_beta_headers,
                                                             econ_money_t safety_margin);
econ_token_evidence_t *econ_test_token_evidence_create(econ_provider_t provider,
                                                       uint32_t endpoint_id,
                                                       const char *serialized_buffer,
                                                       uint64_t input_tokens,
                                                       econ_token_source_t source);
void econ_test_openai_context_free(econ_openai_context_t *context);
void econ_test_anthropic_context_free(econ_anthropic_context_t *context);
void econ_test_token_evidence_free(econ_token_evidence_t *evidence);

#endif
