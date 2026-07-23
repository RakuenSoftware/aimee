/* economizer_anthropic.c: conservative Claude accounting, without network I/O. */
#include "economizer_anthropic.h"

#include "cJSON.h"
#include "economizer_planner_internal.h"
#include <limits.h>
#include <string.h>

_Static_assert(ECON_ANTHROPIC_NO_CACHE_SCENARIO_COUNT <= ECON_MAX_SCENARIOS,
               "Anthropic scenario set must fit the shared proof");

typedef struct
{
   size_t markers_5m;
   size_t markers_1h;
   int saw_5m;
   int automatic_cache;
   int has_tools;
   uint64_t max_output_tokens;
} anthropic_cache_layout_t;

static econ_anthropic_plan_t plan_result(econ_reason_t reason)
{
   econ_anthropic_plan_t out;
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

static int record_control(const cJSON *control, anthropic_cache_layout_t *layout)
{
   if (!cJSON_IsObject(control) || !layout)
      return -1;
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(control, "type");
   const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(control, "ttl");
   if (!cJSON_IsString(type) || strcmp(type->valuestring, "ephemeral") != 0)
      return -1;
   if (!ttl || (cJSON_IsString(ttl) && strcmp(ttl->valuestring, "5m") == 0))
   {
      layout->markers_5m++;
      layout->saw_5m = 1;
      return 0;
   }
   if (!cJSON_IsString(ttl) || strcmp(ttl->valuestring, "1h") != 0 || layout->saw_5m)
      return -1;
   layout->markers_1h++;
   return 0;
}

static int scan_controls(const cJSON *node, anthropic_cache_layout_t *layout)
{
   if (!node || !layout)
      return -1;
   if (cJSON_IsObject(node))
   {
      const cJSON *child;
      cJSON_ArrayForEach(child, node)
      {
         if (child->string && strcmp(child->string, "cache_control") == 0)
         {
            if (record_control(child, layout) != 0)
               return -1;
         }
         else if (scan_controls(child, layout) != 0)
            return -1;
      }
   }
   else if (cJSON_IsArray(node))
   {
      const cJSON *child;
      cJSON_ArrayForEach(child, node) if (scan_controls(child, layout) != 0) return -1;
   }
   return 0;
}

static int parse_layout(const char *json, const char *model, anthropic_cache_layout_t *layout)
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
   const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
   const cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(root, "max_tokens");
   const cJSON *top_control = cJSON_GetObjectItemCaseSensitive(root, "cache_control");
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
   int ok = cJSON_IsString(request_model) && strcmp(request_model->valuestring, model) == 0 &&
            cJSON_IsArray(messages) && cJSON_IsNumber(max_tokens) && max_tokens->valuedouble > 0 &&
            max_tokens->valuedouble <= INT_MAX &&
            max_tokens->valuedouble == (double)max_tokens->valueint;
   if (ok)
   {
      layout->max_output_tokens = (uint64_t)max_tokens->valueint;
      layout->automatic_cache = top_control != NULL;
      layout->has_tools = tools != NULL;
   }

   /* Provider prompt order is tools, system, messages. */
   const cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
   if (ok && tools && scan_controls(tools, layout) != 0)
      ok = 0;
   if (ok && system && scan_controls(system, layout) != 0)
      ok = 0;
   if (ok && scan_controls(messages, layout) != 0)
      ok = 0;
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

static econ_reason_t evidence_reason(const econ_token_evidence_t *evidence,
                                     const econ_anthropic_context_t *context, const char *json)
{
   if (!evidence || !context || !json || evidence->cookie != ECON_TOKEN_EVIDENCE_COOKIE ||
       evidence->provider != ECON_PROVIDER_ANTHROPIC || evidence->endpoint_id != 1 ||
       evidence->model_snapshot_id != context->model_snapshot_id ||
       evidence->tokenizer_id != context->tokenizer_id || evidence->serialized_buffer != json ||
       evidence->serialized_size != strlen(json))
      return ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT;
   if (evidence->source == ECON_TOKEN_SOURCE_REMOTE_ESTIMATE)
      return ECON_REASON_REMOTE_TOKEN_COUNT;
   return evidence->source == ECON_TOKEN_SOURCE_LOCAL_EXACT ? ECON_REASON_NONE
                                                            : ECON_REASON_TOKENIZER_NOT_LOCAL_EXACT;
}

static int prices_valid(const econ_anthropic_prices_t *prices)
{
   if (!prices || prices->input_per_token <= 0 || prices->cached_read_per_token <= 0 ||
       prices->cache_write_5m_per_token <= 0 || prices->cache_write_1h_per_token <= 0 ||
       prices->output_per_token <= 0 ||
       (prices->long_context_threshold != 0 &&
        (prices->long_input_per_token <= 0 || prices->long_output_per_token <= 0)))
      return 0;
   /* Anthropic prompt-caching contract: read=0.1x, 5m write=1.25x,
    * 1h write=2x. The signed context pins the contract generation. */
   if (prices->input_per_token % 10 != 0 ||
       prices->cached_read_per_token != prices->input_per_token / 10 ||
       prices->input_per_token % 4 != 0 || prices->input_per_token / 4 > INT64_MAX / 5 ||
       prices->cache_write_5m_per_token != (prices->input_per_token / 4) * 5 ||
       prices->input_per_token > INT64_MAX / 2 ||
       prices->cache_write_1h_per_token != prices->input_per_token * 2)
      return 0;
   return 1;
}

static int request_cost(uint64_t input_tokens, uint64_t output_tokens,
                        const econ_anthropic_prices_t *prices, econ_money_t *input_cost_out,
                        econ_money_t *output_cost_out)
{
   if (!prices || !input_cost_out || !output_cost_out)
      return -1;
   int long_context =
       prices->long_context_threshold != 0 && input_tokens > prices->long_context_threshold;
   econ_money_t input_rate = long_context ? prices->long_input_per_token : prices->input_per_token;
   econ_money_t output_rate =
       long_context ? prices->long_output_per_token : prices->output_per_token;
   return mul_money(input_tokens, input_rate, input_cost_out) == 0 &&
                  mul_money(output_tokens, output_rate, output_cost_out) == 0
              ? 0
              : -1;
}

econ_anthropic_plan_t econ_anthropic_plan(const econ_anthropic_plan_input_t *input)
{
   if (!input || !input->baseline_json || !input->candidate_json || !input->context)
      return plan_result(ECON_REASON_INVALID_ARGUMENT);
   const econ_anthropic_context_t *context = input->context;
   if (context->cookie != ECON_PLANNER_CONTEXT_COOKIE || !context->pinned_model ||
       context->model_snapshot_id == 0 || context->tokenizer_id == 0 ||
       context->pricing_table_id == 0 || context->contract_versions == 0)
      return plan_result(ECON_REASON_INVALID_IDENTITY);
   econ_reason_t baseline_evidence =
       evidence_reason(input->baseline_tokens, context, input->baseline_json);
   econ_reason_t candidate_evidence =
       evidence_reason(input->candidate_tokens, context, input->candidate_json);
   if (baseline_evidence != ECON_REASON_NONE)
      return plan_result(baseline_evidence);
   if (candidate_evidence != ECON_REASON_NONE)
      return plan_result(candidate_evidence);
   if (context->has_beta_headers)
      return plan_result(ECON_REASON_INVALID_REQUEST_SHAPE);
   if (!prices_valid(&context->prices) || context->safety_margin < 0)
      return plan_result(ECON_REASON_PRICING_UNAVAILABLE);

   anthropic_cache_layout_t baseline, candidate;
   if (parse_layout(input->baseline_json, context->pinned_model, &baseline) != 0 ||
       parse_layout(input->candidate_json, context->pinned_model, &candidate) != 0)
      return plan_result(ECON_REASON_INVALID_CACHE_CONTROL);

   econ_anthropic_plan_t out = plan_result(ECON_REASON_NONE);
   out.baseline_breakpoints_5m = baseline.markers_5m;
   out.baseline_breakpoints_1h = baseline.markers_1h;
   out.candidate_breakpoints_5m = candidate.markers_5m;
   out.candidate_breakpoints_1h = candidate.markers_1h;
   if (baseline.max_output_tokens != candidate.max_output_tokens)
   {
      out.reason = ECON_REASON_OUTPUT_BOUND_UNAVAILABLE;
      return out;
   }
   if (baseline.automatic_cache || candidate.automatic_cache || baseline.has_tools ||
       candidate.has_tools)
   {
      out.reason = ECON_REASON_UNSUPPORTED_CACHE_LAYOUT;
      return out;
   }
   if (baseline.markers_5m != candidate.markers_5m || baseline.markers_1h != candidate.markers_1h)
   {
      out.reason = ECON_REASON_UNSUPPORTED_CACHE_LAYOUT;
      return out;
   }
   if (baseline.markers_5m != 0 || baseline.markers_1h != 0)
   {
      out.reason = ECON_REASON_PROTECTED_PREFIX_UNPROVEN;
      return out;
   }

   uint64_t baseline_tokens = input->baseline_tokens->input_tokens;
   uint64_t candidate_tokens = input->candidate_tokens->input_tokens;
   econ_money_t baseline_input = 0, ignored_output = 0;
   econ_money_t candidate_input = 0, candidate_output = 0;
   if (request_cost(baseline_tokens, 0, &context->prices, &baseline_input, &ignored_output) != 0 ||
       request_cost(candidate_tokens, candidate.max_output_tokens, &context->prices,
                    &candidate_input, &candidate_output) != 0 ||
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
   local.transform.provider = ECON_PROVIDER_ANTHROPIC;
   local.transform.endpoint_id = 1;
   local.transform.model_snapshot_id = context->model_snapshot_id;
   local.transform.tokenizer_id = context->tokenizer_id;
   local.transform.pricing_table_id = context->pricing_table_id;
   local.transform.contract_versions = context->contract_versions;
   local.transform.transform_id = local.transform.transform_version = 1;
   local.transform.scenario_set_id = 1;
   local.transform.scenario_coverage = UINT64_C(1);
   local.scenario_count = ECON_ANTHROPIC_NO_CACHE_SCENARIO_COUNT;
   local.safety_margin = context->safety_margin;
   local.scenarios[0] = out.scenario;
   econ_cost_result_t cost = econ_proof_cost_evaluate(&local);
   out.cost_verdict = cost.verdict;
   out.reason = cost.reason;
   return out;
}
