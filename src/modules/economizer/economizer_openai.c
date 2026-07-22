/* economizer_openai.c: conservative GPT-5.6 accounting, without network I/O. */
#include "economizer_openai.h"

#include "cJSON.h"
#include "economizer_planner_internal.h"
#include <limits.h>
#include <string.h>

_Static_assert(ECON_OPENAI_GPT56_SCENARIO_COUNT <= ECON_MAX_SCENARIOS,
               "OpenAI scenario set must fit the shared proof");

typedef struct
{
   int explicit_mode;
   int ttl_present;
   int cache_key_present;
   size_t breakpoints;
   uint64_t max_output_tokens;
   char cache_key[256];
} openai_cache_layout_t;

static econ_openai_plan_t plan_result(econ_reason_t reason)
{
   econ_openai_plan_t out;
   memset(&out, 0, sizeof(out));
   out.cost_verdict = ECON_COST_INDETERMINATE;
   out.reason = reason;
   return out;
}

static int add_money(econ_money_t a, econ_money_t b, econ_money_t *out)
{
   if (!out || a < 0 || b < 0 || a > INT64_MAX - b)
      return -1;
   *out = a + b;
   return 0;
}

static int mul_money(uint64_t tokens, econ_money_t rate, econ_money_t *out)
{
   if (!out || rate < 0 || (tokens != 0 && (uint64_t)rate > (uint64_t)INT64_MAX / tokens))
      return -1;
   *out = (econ_money_t)(tokens * (uint64_t)rate);
   return 0;
}

static int reject_duplicate_keys(const cJSON *node)
{
   if (!node)
      return -1;
   if (cJSON_IsObject(node))
   {
      const cJSON *a;
      cJSON_ArrayForEach(a, node)
      {
         if (!a->string)
            return -1;
         for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && strcmp(a->string, b->string) == 0)
               return -1;
         if (reject_duplicate_keys(a) != 0)
            return -1;
      }
   }
   else if (cJSON_IsArray(node))
   {
      const cJSON *child;
      cJSON_ArrayForEach(child, node) if (reject_duplicate_keys(child) != 0) return -1;
   }
   return 0;
}

static int scan_breakpoints(const cJSON *node, size_t *count)
{
   if (!node || !count)
      return -1;
   if (cJSON_IsObject(node))
   {
      const cJSON *child;
      cJSON_ArrayForEach(child, node)
      {
         if (child->string && strcmp(child->string, "prompt_cache_breakpoint") == 0)
         {
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(child, "mode");
            if (!cJSON_IsObject(child) || !cJSON_IsString(mode) ||
                strcmp(mode->valuestring, "explicit") != 0)
               return -1;
            const cJSON *field;
            cJSON_ArrayForEach(field, child) if (!field->string ||
                                                 strcmp(field->string, "mode") != 0) return -1;
            (*count)++;
         }
         else if (scan_breakpoints(child, count) != 0)
            return -1;
      }
   }
   else if (cJSON_IsArray(node))
   {
      const cJSON *child;
      cJSON_ArrayForEach(child, node) if (scan_breakpoints(child, count) != 0) return -1;
   }
   return 0;
}

static int parse_layout(const char *json, const char *model, econ_openai_endpoint_t endpoint,
                        openai_cache_layout_t *layout)
{
   if (!json || !model || !layout)
      return -1;
   memset(layout, 0, sizeof(*layout));
   cJSON *root = cJSON_ParseWithOpts(json, NULL, 1);
   if (!root || !cJSON_IsObject(root) || reject_duplicate_keys(root) != 0)
   {
      cJSON_Delete(root);
      return -1;
   }

   const cJSON *request_model = cJSON_GetObjectItemCaseSensitive(root, "model");
   const cJSON *shape = cJSON_GetObjectItemCaseSensitive(
       root, endpoint == ECON_OPENAI_RESPONSES ? "input" : "messages");
   const cJSON *options = cJSON_GetObjectItemCaseSensitive(root, "prompt_cache_options");
   const cJSON *cache_key = cJSON_GetObjectItemCaseSensitive(root, "prompt_cache_key");
   const cJSON *legacy_retention = cJSON_GetObjectItemCaseSensitive(root, "prompt_cache_retention");
   const cJSON *mode = options ? cJSON_GetObjectItemCaseSensitive(options, "mode") : NULL;
   const cJSON *ttl = options ? cJSON_GetObjectItemCaseSensitive(options, "ttl") : NULL;
   const cJSON *max_output = cJSON_GetObjectItemCaseSensitive(
       root, endpoint == ECON_OPENAI_RESPONSES ? "max_output_tokens" : "max_completion_tokens");

   int ok =
       cJSON_IsString(request_model) && strcmp(request_model->valuestring, model) == 0 &&
       ((endpoint == ECON_OPENAI_RESPONSES && (cJSON_IsString(shape) || cJSON_IsArray(shape))) ||
        (endpoint == ECON_OPENAI_CHAT_COMPLETIONS && cJSON_IsArray(shape))) &&
       cJSON_IsNumber(max_output) && max_output->valuedouble > 0 &&
       max_output->valuedouble <= INT_MAX &&
       max_output->valuedouble == (double)max_output->valueint &&
       (!cache_key || (cJSON_IsString(cache_key) && cache_key->valuestring &&
                       strlen(cache_key->valuestring) < sizeof(layout->cache_key))) &&
       !legacy_retention &&
       (!options || (cJSON_IsObject(options) && cJSON_IsString(mode) &&
                     (strcmp(mode->valuestring, "explicit") == 0 ||
                      strcmp(mode->valuestring, "implicit") == 0) &&
                     (!ttl || (cJSON_IsString(ttl) && strcmp(ttl->valuestring, "30m") == 0))));
   if (ok)
   {
      if (options)
      {
         const cJSON *field;
         cJSON_ArrayForEach(field, options) if (!field->string ||
                                                (strcmp(field->string, "mode") != 0 &&
                                                 strcmp(field->string, "ttl") != 0)) ok = 0;
      }
      layout->explicit_mode = options && strcmp(mode->valuestring, "explicit") == 0;
      layout->ttl_present = ttl != NULL;
      layout->cache_key_present = cache_key != NULL;
      if (cache_key)
         memcpy(layout->cache_key, cache_key->valuestring, strlen(cache_key->valuestring) + 1);
      layout->max_output_tokens = (uint64_t)max_output->valueint;
      ok = ok && scan_breakpoints(root, &layout->breakpoints) == 0;
   }
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

static int evidence_valid(const econ_token_evidence_t *evidence,
                          const econ_openai_context_t *context, const char *json,
                          econ_openai_endpoint_t endpoint)
{
   return evidence && context && json && evidence->cookie == ECON_TOKEN_EVIDENCE_COOKIE &&
          evidence->provider == ECON_PROVIDER_OPENAI &&
          evidence->endpoint_id == (uint32_t)endpoint &&
          evidence->model_snapshot_id == context->model_snapshot_id &&
          evidence->tokenizer_id == context->tokenizer_id &&
          evidence->source == ECON_TOKEN_SOURCE_LOCAL_EXACT &&
          evidence->serialized_buffer == json && evidence->serialized_size == strlen(json);
}

static int prices_valid(const econ_openai_prices_t *prices)
{
   if (!prices || prices->input_per_token <= 0 || prices->cached_read_per_token <= 0 ||
       prices->cache_write_per_token <= 0 || prices->output_per_token <= 0)
      return 0;
   if (prices->input_per_token % 10 != 0 ||
       prices->cached_read_per_token != prices->input_per_token / 10 ||
       prices->input_per_token % 4 != 0 || prices->input_per_token / 4 > INT64_MAX / 5 ||
       prices->cache_write_per_token != (prices->input_per_token / 4) * 5 ||
       prices->output_per_token % 2 != 0)
      return 0;
   return 1;
}

static int within_guard(uint64_t tokens, uint64_t guard)
{
   const uint64_t boundary = ECON_OPENAI_GPT56_LONG_CONTEXT_THRESHOLD;
   return tokens >= boundary ? tokens - boundary <= guard : boundary - tokens <= guard;
}

static int input_cost(uint64_t tokens, econ_money_t rate, econ_money_t *out)
{
   econ_money_t base = 0;
   if (mul_money(tokens, rate, &base) != 0)
      return -1;
   if (tokens > ECON_OPENAI_GPT56_LONG_CONTEXT_THRESHOLD)
      return add_money(base, base, out);
   *out = base;
   return 0;
}

static int output_cost(uint64_t tokens, econ_money_t rate, int long_context, econ_money_t *out)
{
   if (long_context)
   {
      econ_money_t half = rate / 2;
      if (rate % 2 != 0 || add_money(rate, half, &rate) != 0)
         return -1;
   }
   return mul_money(tokens, rate, out);
}

econ_openai_plan_t econ_openai_gpt56_plan(const econ_openai_plan_input_t *input)
{
   if (!input || !input->baseline_json || !input->candidate_json || !input->context)
      return plan_result(ECON_REASON_INVALID_ARGUMENT);
   const econ_openai_context_t *context = input->context;
   if (context->cookie != ECON_PLANNER_CONTEXT_COOKIE || !context->pinned_model ||
       context->model_snapshot_id == 0 || context->tokenizer_id == 0 ||
       context->pricing_table_id == 0 || context->contract_versions == 0)
      return plan_result(ECON_REASON_INVALID_IDENTITY);
   if (input->endpoint != ECON_OPENAI_RESPONSES && input->endpoint != ECON_OPENAI_CHAT_COMPLETIONS)
      return plan_result(ECON_REASON_UNSUPPORTED_ENDPOINT);
   if (!evidence_valid(input->baseline_tokens, context, input->baseline_json, input->endpoint) ||
       !evidence_valid(input->candidate_tokens, context, input->candidate_json, input->endpoint))
      return plan_result(ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT);
   if (!prices_valid(&context->prices) || context->safety_margin < 0)
      return plan_result(ECON_REASON_PRICING_UNAVAILABLE);

   openai_cache_layout_t baseline, candidate;
   if (parse_layout(input->baseline_json, context->pinned_model, input->endpoint, &baseline) != 0 ||
       parse_layout(input->candidate_json, context->pinned_model, input->endpoint, &candidate) != 0)
      return plan_result(ECON_REASON_INVALID_REQUEST_SHAPE);

   econ_openai_plan_t out = plan_result(ECON_REASON_NONE);
   out.baseline_breakpoints = baseline.breakpoints;
   out.candidate_breakpoints = candidate.breakpoints;
   if (!baseline.explicit_mode || !candidate.explicit_mode)
   {
      out.reason = ECON_REASON_UNSUPPORTED_CACHE_LAYOUT;
      return out;
   }
   if (baseline.ttl_present != candidate.ttl_present ||
       baseline.cache_key_present != candidate.cache_key_present ||
       (baseline.cache_key_present && strcmp(baseline.cache_key, candidate.cache_key) != 0))
   {
      out.reason = ECON_REASON_UNSUPPORTED_CACHE_LAYOUT;
      return out;
   }
   if (baseline.max_output_tokens != candidate.max_output_tokens)
   {
      out.reason = ECON_REASON_OUTPUT_BOUND_UNAVAILABLE;
      return out;
   }
   if (baseline.breakpoints != candidate.breakpoints)
   {
      out.reason = ECON_REASON_UNSUPPORTED_CACHE_LAYOUT;
      return out;
   }
   if (baseline.breakpoints != 0)
   {
      out.reason = ECON_REASON_PROTECTED_PREFIX_UNPROVEN;
      return out;
   }

   uint64_t baseline_tokens = input->baseline_tokens->input_tokens;
   uint64_t candidate_tokens = input->candidate_tokens->input_tokens;
   if (within_guard(baseline_tokens, context->tokenizer_guard_tokens) ||
       within_guard(candidate_tokens, context->tokenizer_guard_tokens))
   {
      out.reason = ECON_REASON_TOKEN_GUARD_BAND;
      return out;
   }

   econ_money_t baseline_input = 0, candidate_input = 0, candidate_output = 0;
   if (input_cost(baseline_tokens, context->prices.input_per_token, &baseline_input) != 0 ||
       input_cost(candidate_tokens, context->prices.input_per_token, &candidate_input) != 0 ||
       output_cost(candidate.max_output_tokens, context->prices.output_per_token,
                   candidate_tokens > ECON_OPENAI_GPT56_LONG_CONTEXT_THRESHOLD,
                   &candidate_output) != 0 ||
       add_money(candidate_input, candidate_output, &out.scenario.candidate_upper) != 0)
   {
      out.reason = ECON_REASON_MONEY_OVERFLOW;
      return out;
   }

   out.scenario.baseline_lower = baseline_input;
   out.scenario.baseline_upper = baseline_input;
   out.scenario.candidate_lower = candidate_input;
   out.scenario.baseline_lower_tokens.ordinary_input = baseline_tokens;
   out.scenario.baseline_upper_tokens.ordinary_input = baseline_tokens;
   out.scenario.candidate_lower_tokens.ordinary_input = candidate_tokens;
   out.scenario.candidate_upper_tokens.ordinary_input = candidate_tokens;
   out.scenario.candidate_upper_tokens.output = candidate.max_output_tokens;

   econ_proof_t local;
   memset(&local, 0, sizeof(local));
   local.tenant_id = local.account_id = local.call_id = 1;
   local.transform.provider = ECON_PROVIDER_OPENAI;
   local.transform.endpoint_id = (uint32_t)input->endpoint;
   local.transform.model_snapshot_id = context->model_snapshot_id;
   local.transform.tokenizer_id = context->tokenizer_id;
   local.transform.pricing_table_id = context->pricing_table_id;
   local.transform.contract_versions = context->contract_versions;
   local.transform.transform_id = local.transform.transform_version = 1;
   local.scenario_count = ECON_OPENAI_GPT56_SCENARIO_COUNT;
   local.safety_margin = context->safety_margin;
   local.scenarios[0] = out.scenario;
   econ_cost_result_t cost = econ_proof_cost_evaluate(&local);
   out.cost_verdict = cost.verdict;
   out.reason = cost.reason;
   return out;
}
