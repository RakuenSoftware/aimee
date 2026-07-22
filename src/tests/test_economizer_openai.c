#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "economizer_openai.h"
#include "economizer_planner_fixture.h"

#define PASS(name) printf("  PASS: %s\n", name)

static const char *baseline_no_cache =
    "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10000,"
    "\"prompt_cache_options\":{\"mode\":\"explicit\"},\"input\":\"large baseline\"}";
static const char *candidate_no_cache =
    "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10000,"
    "\"prompt_cache_options\":{\"mode\":\"explicit\"},\"input\":\"small candidate\"}";

typedef struct
{
   econ_openai_plan_input_t input;
   econ_openai_context_t *context;
   econ_token_evidence_t *baseline_evidence;
   econ_token_evidence_t *candidate_evidence;
} fixture_t;

static fixture_t fixture_make_endpoint(const char *baseline, uint64_t baseline_tokens,
                                       const char *candidate, uint64_t candidate_tokens,
                                       econ_openai_endpoint_t endpoint)
{
   fixture_t f;
   memset(&f, 0, sizeof(f));
   econ_openai_prices_t prices = {
       .input_per_token = 20,
       .cached_read_per_token = 2,
       .cache_write_per_token = 25,
       .output_per_token = 60,
   };
   f.context = econ_test_openai_context_create("gpt-5.6-sol-snapshot", prices, 0, 1);
   f.baseline_evidence = econ_test_token_evidence_create(
       ECON_PROVIDER_OPENAI, endpoint, baseline, baseline_tokens, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   f.candidate_evidence = econ_test_token_evidence_create(
       ECON_PROVIDER_OPENAI, endpoint, candidate, candidate_tokens, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   f.input.baseline_json = baseline;
   f.input.candidate_json = candidate;
   f.input.context = f.context;
   f.input.baseline_tokens = f.baseline_evidence;
   f.input.candidate_tokens = f.candidate_evidence;
   f.input.endpoint = endpoint;
   return f;
}

static fixture_t fixture_make(const char *baseline, uint64_t baseline_tokens, const char *candidate,
                              uint64_t candidate_tokens)
{
   return fixture_make_endpoint(baseline, baseline_tokens, candidate, candidate_tokens,
                                ECON_OPENAI_RESPONSES);
}

static void fixture_free(fixture_t *f)
{
   econ_test_token_evidence_free(f->baseline_evidence);
   econ_test_token_evidence_free(f->candidate_evidence);
   econ_test_openai_context_free(f->context);
}

static void test_threshold_crossing_cost_evidence_is_not_authority(void)
{
   fixture_t f = fixture_make(baseline_no_cache, 300000, candidate_no_cache, 200000);
   econ_openai_plan_t plan = econ_openai_gpt56_plan(&f.input);
   assert(plan.cost_verdict == ECON_COST_PROVEN);
   assert(plan.reason == ECON_REASON_PROOF_ACCEPTED);
   assert(plan.scenario.baseline_lower == 12000000);
   assert(plan.scenario.candidate_upper == 4600000);
   fixture_free(&f);
   PASS("threshold_crossing_cost_evidence_is_not_authority");
}

static void test_strict_threshold_and_guard(void)
{
   fixture_t f = fixture_make(baseline_no_cache, 272001, candidate_no_cache, 271999);
   econ_openai_plan_t plan = econ_openai_gpt56_plan(&f.input);
   assert(plan.scenario.baseline_lower == 10880040);
   assert(plan.scenario.candidate_lower == 5439980);
   fixture_free(&f);

   f = fixture_make(baseline_no_cache, 272001, candidate_no_cache, 272000);
   plan = econ_openai_gpt56_plan(&f.input);
   assert(plan.reason == ECON_REASON_TOKEN_GUARD_BAND);
   fixture_free(&f);
   PASS("strict_threshold_and_guard");
}

static void test_implicit_and_breakpoint_cache_are_denied(void)
{
   const char *implicit = "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10000,"
                          "\"input\":\"small candidate\"}";
   fixture_t f = fixture_make(baseline_no_cache, 300000, implicit, 200000);
   assert(econ_openai_gpt56_plan(&f.input).reason == ECON_REASON_UNSUPPORTED_CACHE_LAYOUT);
   fixture_free(&f);

   const char *marked =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10000,"
       "\"prompt_cache_options\":{\"mode\":\"explicit\"},\"input\":[{\"type\":\"message\","
       "\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"stable\","
       "\"prompt_cache_breakpoint\":{\"mode\":\"explicit\"}}]}]}";
   f = fixture_make(marked, 300000, marked, 200000);
   econ_openai_plan_t plan = econ_openai_gpt56_plan(&f.input);
   assert(plan.baseline_breakpoints == 1);
   assert(plan.reason == ECON_REASON_PROTECTED_PREFIX_UNPROVEN);
   fixture_free(&f);
   PASS("implicit_and_breakpoint_cache_are_denied");
}

static void test_evidence_is_bound_to_buffer_and_duplicates_rejected(void)
{
   char candidate_copy[256];
   snprintf(candidate_copy, sizeof(candidate_copy), "%s", candidate_no_cache);
   fixture_t f = fixture_make(baseline_no_cache, 300000, candidate_no_cache, 200000);
   f.input.candidate_json = candidate_copy;
   assert(econ_openai_gpt56_plan(&f.input).reason == ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT);
   fixture_free(&f);

   const char *duplicate =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"model\":\"other\","
       "\"max_output_tokens\":10000,\"prompt_cache_options\":{\"mode\":\"explicit\"},"
       "\"input\":\"small\"}";
   f = fixture_make(baseline_no_cache, 300000, duplicate, 200000);
   assert(econ_openai_gpt56_plan(&f.input).reason == ECON_REASON_INVALID_REQUEST_SHAPE);
   fixture_free(&f);
   PASS("evidence_is_bound_to_buffer_and_duplicates_rejected");
}

static void test_odd_long_context_output_count_is_representable(void)
{
   const char *baseline =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10001,"
       "\"prompt_cache_options\":{\"mode\":\"explicit\"},\"input\":\"baseline\"}";
   const char *candidate =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_output_tokens\":10001,"
       "\"prompt_cache_options\":{\"mode\":\"explicit\"},\"input\":\"candidate\"}";
   fixture_t f = fixture_make(baseline, 400000, candidate, 300000);
   econ_openai_plan_t plan = econ_openai_gpt56_plan(&f.input);
   assert(plan.reason != ECON_REASON_MONEY_OVERFLOW);
   assert(plan.scenario.candidate_upper == 12900090);
   fixture_free(&f);
   PASS("odd_long_context_output_count_is_representable");
}

static void test_chat_shape_and_cache_intent(void)
{
   const char *baseline =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_completion_tokens\":10000,"
       "\"prompt_cache_key\":\"stable-key\",\"prompt_cache_options\":{\"mode\":\"explicit\"},"
       "\"messages\":[{\"role\":\"user\",\"content\":\"large\"}]}";
   const char *candidate =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_completion_tokens\":10000,"
       "\"prompt_cache_key\":\"stable-key\",\"prompt_cache_options\":{\"mode\":\"explicit\"},"
       "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   fixture_t f =
       fixture_make_endpoint(baseline, 300000, candidate, 200000, ECON_OPENAI_CHAT_COMPLETIONS);
   assert(econ_openai_gpt56_plan(&f.input).cost_verdict == ECON_COST_PROVEN);
   fixture_free(&f);

   const char *changed_key =
       "{\"model\":\"gpt-5.6-sol-snapshot\",\"max_completion_tokens\":10000,"
       "\"prompt_cache_key\":\"changed-key\",\"prompt_cache_options\":{\"mode\":\"explicit\"},"
       "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   f = fixture_make_endpoint(baseline, 300000, changed_key, 200000, ECON_OPENAI_CHAT_COMPLETIONS);
   assert(econ_openai_gpt56_plan(&f.input).reason == ECON_REASON_UNSUPPORTED_CACHE_LAYOUT);
   fixture_free(&f);
   PASS("chat_shape_and_cache_intent");
}

int main(void)
{
   printf("economizer_openai tests:\n");
   test_threshold_crossing_cost_evidence_is_not_authority();
   test_strict_threshold_and_guard();
   test_implicit_and_breakpoint_cache_are_denied();
   test_evidence_is_bound_to_buffer_and_duplicates_rejected();
   test_odd_long_context_output_count_is_representable();
   test_chat_shape_and_cache_intent();
   printf("ALL PASS\n");
   return 0;
}
