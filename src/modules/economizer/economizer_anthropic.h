/* economizer_anthropic.h: local-only Claude proof planner.
 *
 * Anthropic's remote token-count API is documented as an estimate. It is
 * therefore never accepted as authorization evidence by this planner.
 * Pricing contract: https://platform.claude.com/docs/en/build-with-claude/prompt-caching */
#ifndef DEC_ECONOMIZER_ANTHROPIC_H
#define DEC_ECONOMIZER_ANTHROPIC_H 1

#include "economizer_proof.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ECON_ANTHROPIC_NO_CACHE_SCENARIO_COUNT 1u

   typedef struct econ_anthropic_context econ_anthropic_context_t;
   typedef struct econ_token_evidence econ_token_evidence_t;

   typedef struct
   {
      econ_money_t input_per_token;
      econ_money_t cached_read_per_token;
      econ_money_t cache_write_5m_per_token;
      econ_money_t cache_write_1h_per_token;
      econ_money_t output_per_token;
      econ_money_t long_input_per_token;
      econ_money_t long_output_per_token;
      uint64_t long_context_threshold;
   } econ_anthropic_prices_t;

   typedef struct
   {
      const char *baseline_json;
      const char *candidate_json;
      const econ_anthropic_context_t *context;
      const econ_token_evidence_t *baseline_tokens;
      const econ_token_evidence_t *candidate_tokens;
   } econ_anthropic_plan_input_t;

   typedef struct
   {
      econ_cost_verdict_t cost_verdict;
      econ_reason_t reason;
      size_t baseline_breakpoints_5m;
      size_t baseline_breakpoints_1h;
      size_t candidate_breakpoints_5m;
      size_t candidate_breakpoints_1h;
      econ_scenario_t scenario;
   } econ_anthropic_plan_t;

   /* Non-authorizing local accounting evidence only. */
   econ_anthropic_plan_t econ_anthropic_plan(const econ_anthropic_plan_input_t *input);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_ANTHROPIC_H */
