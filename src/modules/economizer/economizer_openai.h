/* economizer_openai.h: local-only GPT-5.6 proof planner.
 *
 * The planner never calls OpenAI and never predicts cache residency. The first
 * release accepts only explicit cache mode with no breakpoints, for which the
 * provider contract documents that prompt caching and cache-write charges are
 * disabled. Requests containing breakpoints are parsed but remain denied until
 * exact protected serialized ranges are implemented. */
#ifndef DEC_ECONOMIZER_OPENAI_H
#define DEC_ECONOMIZER_OPENAI_H 1

#include "economizer_proof.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ECON_OPENAI_GPT56_LONG_CONTEXT_THRESHOLD 272000u
#define ECON_OPENAI_GPT56_SCENARIO_COUNT         1u

   typedef struct econ_openai_context econ_openai_context_t;
   typedef struct econ_token_evidence econ_token_evidence_t;

   typedef enum
   {
      ECON_OPENAI_RESPONSES = 1,
      ECON_OPENAI_CHAT_COMPLETIONS = 2
   } econ_openai_endpoint_t;

   typedef struct
   {
      econ_money_t input_per_token;
      econ_money_t cached_read_per_token;
      econ_money_t cache_write_per_token;
      econ_money_t output_per_token;
   } econ_openai_prices_t;

   typedef struct
   {
      const char *baseline_json;
      const char *candidate_json;
      const econ_openai_context_t *context;
      const econ_token_evidence_t *baseline_tokens;
      const econ_token_evidence_t *candidate_tokens;
      econ_openai_endpoint_t endpoint;
   } econ_openai_plan_input_t;

   typedef struct
   {
      econ_cost_verdict_t cost_verdict;
      econ_reason_t reason;
      size_t baseline_breakpoints;
      size_t candidate_breakpoints;
      econ_scenario_t scenario;
   } econ_openai_plan_t;

   /* Non-authorizing local accounting evidence only. This API returns neither
    * a dispatch decision nor an authorization-ready proof. */
   econ_openai_plan_t econ_openai_gpt56_plan(const econ_openai_plan_input_t *input);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_OPENAI_H */
