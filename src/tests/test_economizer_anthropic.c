#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "economizer_anthropic.h"
#include "economizer_planner_fixture.h"

#define PASS(name) printf("  PASS: %s\n", name)

static const char *baseline_no_cache =
    "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"large baseline\"}]}";
static const char *candidate_no_cache =
    "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"small candidate\"}]}";

typedef struct
{
   econ_anthropic_plan_input_t input;
   econ_anthropic_context_t *context;
   econ_token_evidence_t *baseline_evidence;
   econ_token_evidence_t *candidate_evidence;
} fixture_t;

static fixture_t fixture_make(const char *baseline, uint64_t baseline_tokens, const char *candidate,
                              uint64_t candidate_tokens, econ_token_source_t source)
{
   fixture_t f;
   memset(&f, 0, sizeof(f));
   econ_anthropic_prices_t prices = {
       .input_per_token = 20,
       .cached_read_per_token = 2,
       .cache_write_5m_per_token = 25,
       .cache_write_1h_per_token = 40,
       .output_per_token = 60,
       .long_input_per_token = 40,
       .long_output_per_token = 90,
       .long_context_threshold = 200000,
   };
   f.context = econ_test_anthropic_context_create("claude-snapshot", prices, 0, 1);
   f.baseline_evidence = econ_test_token_evidence_create(ECON_PROVIDER_ANTHROPIC, 1, baseline,
                                                         baseline_tokens, source);
   f.candidate_evidence = econ_test_token_evidence_create(ECON_PROVIDER_ANTHROPIC, 1, candidate,
                                                          candidate_tokens, source);
   f.input.baseline_json = baseline;
   f.input.candidate_json = candidate;
   f.input.context = f.context;
   f.input.baseline_tokens = f.baseline_evidence;
   f.input.candidate_tokens = f.candidate_evidence;
   return f;
}

static void fixture_free(fixture_t *f)
{
   econ_test_token_evidence_free(f->baseline_evidence);
   econ_test_token_evidence_free(f->candidate_evidence);
   econ_test_anthropic_context_free(f->context);
}

static void test_no_cache_cost_evidence(void)
{
   fixture_t f = fixture_make(baseline_no_cache, 100000, candidate_no_cache, 50000,
                              ECON_TOKEN_SOURCE_LOCAL_EXACT);
   econ_anthropic_plan_t plan = econ_anthropic_plan(&f.input);
   assert(plan.cost_verdict == ECON_COST_PROVEN);
   assert(plan.reason == ECON_REASON_PROOF_ACCEPTED);
   assert(plan.scenario.baseline_lower == 2000000);
   assert(plan.scenario.candidate_upper == 1060000);
   fixture_free(&f);
   PASS("no_cache_cost_evidence");
}

static void test_remote_count_is_never_evidence(void)
{
   fixture_t f = fixture_make(baseline_no_cache, 100000, candidate_no_cache, 50000,
                              ECON_TOKEN_SOURCE_REMOTE_ESTIMATE);
   econ_anthropic_plan_t plan = econ_anthropic_plan(&f.input);
   assert(plan.cost_verdict == ECON_COST_INDETERMINATE);
   assert(plan.reason == ECON_REASON_REMOTE_TOKEN_COUNT);
   fixture_free(&f);
   PASS("remote_count_is_never_evidence");
}

static void test_automatic_tools_and_explicit_cache_are_denied(void)
{
   const char *automatic = "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,"
                           "\"cache_control\":{\"type\":\"ephemeral\"},"
                           "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   fixture_t f =
       fixture_make(baseline_no_cache, 100000, automatic, 50000, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_UNSUPPORTED_CACHE_LAYOUT);
   fixture_free(&f);

   const char *tools = "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,\"tools\":[],"
                       "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   f = fixture_make(baseline_no_cache, 100000, tools, 50000, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_UNSUPPORTED_CACHE_LAYOUT);
   fixture_free(&f);

   const char *marked =
       "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,\"system\":["
       "{\"type\":\"text\",\"text\":\"stable\",\"cache_control\":{\"type\":\"ephemeral\","
       "\"ttl\":\"1h\"}}],\"messages\":[{\"role\":\"user\",\"content\":["
       "{\"type\":\"text\",\"text\":\"later\",\"cache_control\":{\"type\":\"ephemeral\"}}]}]}";
   f = fixture_make(marked, 100000, marked, 50000, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   econ_anthropic_plan_t plan = econ_anthropic_plan(&f.input);
   assert(plan.baseline_breakpoints_1h == 1);
   assert(plan.baseline_breakpoints_5m == 1);
   assert(plan.reason == ECON_REASON_PROTECTED_PREFIX_UNPROVEN);
   fixture_free(&f);
   PASS("automatic_tools_and_explicit_cache_are_denied");
}

static void test_ttl_order_duplicates_and_output_limit_are_validated(void)
{
   const char *bad_order =
       "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,\"system\":["
       "{\"type\":\"text\",\"text\":\"short\",\"cache_control\":{\"type\":\"ephemeral\"}}],"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"long\","
       "\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}]}]}";
   fixture_t f = fixture_make(bad_order, 100000, bad_order, 50000, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_INVALID_CACHE_CONTROL);
   fixture_free(&f);

   const char *duplicate = "{\"model\":\"claude-snapshot\",\"max_tokens\":1000,\"max_tokens\":1,"
                           "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   f = fixture_make(baseline_no_cache, 100000, duplicate, 50000, ECON_TOKEN_SOURCE_LOCAL_EXACT);
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_INVALID_CACHE_CONTROL);
   fixture_free(&f);

   const char *different_limit = "{\"model\":\"claude-snapshot\",\"max_tokens\":999,"
                                 "\"messages\":[{\"role\":\"user\",\"content\":\"small\"}]}";
   f = fixture_make(baseline_no_cache, 100000, different_limit, 50000,
                    ECON_TOKEN_SOURCE_LOCAL_EXACT);
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_OUTPUT_BOUND_UNAVAILABLE);
   fixture_free(&f);
   PASS("ttl_order_duplicates_and_output_limit_are_validated");
}

static void test_beta_header_context_is_denied(void)
{
   econ_anthropic_prices_t prices = {
       .input_per_token = 20,
       .cached_read_per_token = 2,
       .cache_write_5m_per_token = 25,
       .cache_write_1h_per_token = 40,
       .output_per_token = 60,
   };
   fixture_t f = fixture_make(baseline_no_cache, 100000, candidate_no_cache, 50000,
                              ECON_TOKEN_SOURCE_LOCAL_EXACT);
   econ_test_anthropic_context_free(f.context);
   f.context = econ_test_anthropic_context_create("claude-snapshot", prices, 1, 1);
   f.input.context = f.context;
   assert(econ_anthropic_plan(&f.input).reason == ECON_REASON_INVALID_REQUEST_SHAPE);
   fixture_free(&f);
   PASS("beta_header_context_is_denied");
}

int main(void)
{
   printf("economizer_anthropic tests:\n");
   test_no_cache_cost_evidence();
   test_remote_count_is_never_evidence();
   test_automatic_tools_and_explicit_cache_are_denied();
   test_ttl_order_duplicates_and_output_limit_are_validated();
   test_beta_header_context_is_denied();
   printf("ALL PASS\n");
   return 0;
}
