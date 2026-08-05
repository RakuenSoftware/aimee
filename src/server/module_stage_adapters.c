#define _POSIX_C_SOURCE 200809L

#include "module_stage_adapters.h"

#include <aimee/tools/agent_tools.h>
#include <aimee/git/git_ops.h>
#include "gw_stage_governance.h"
#include "ingress_preinject.h"
#include "wfe_advance.h"
#include <aimee/learning/learning.h>
#include "response_dedup.h"
#include "server_error_kind.h"
#include "modules/skills/skill_trigger_policy.h"
#include "modules/workspace/workspace_scope.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/governance/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/response-composition/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workspace/module_api.h>
#include <aimee/workflows/module_api.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODULE_STAGE_DEADLINE_NS (500ULL * 1000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int call_module(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return obs_bus_module_call(event_kind, stage_id, trace, now + MODULE_STAGE_DEADLINE_NS, request,
                              request_len, response, response_capacity, response_len, NULL,
                              NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

static int memory_confidence(double score, const char **confidence)
{
   if (!confidence)
      return -1;
   double scaled = score * 1000000.0;
   int64_t micros = scaled >= (double)INT64_MAX   ? INT64_MAX
                    : scaled <= (double)INT64_MIN ? INT64_MIN
                                                  : (int64_t)scaled;
   uint8_t request[AIMEE_MEMORY_REQUEST_LEN], response[AIMEE_MEMORY_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_memory_confidence_t result;
   if (aimee_memory_request_encode(micros, request, sizeof(request)) != 0 ||
       call_module(AIMEE_MEMORY_EVENT_RERANK, AIMEE_MEMORY_STAGE_RERANK, request, sizeof(request),
                   response, sizeof(response), &response_len) != 0 ||
       aimee_memory_response_decode(response, response_len, &result) != 0)
      return -1;
   *confidence = result == AIMEE_MEMORY_CONFIDENCE_HIGH     ? "high"
                 : result == AIMEE_MEMORY_CONFIDENCE_MEDIUM ? "medium"
                                                            : "low";
   return 0;
}

static int learning_classify(const char *signal, uint32_t *sink_mask)
{
   uint8_t request[AIMEE_LEARNING_REQUEST_LEN], response[AIMEE_LEARNING_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_learning_request_encode(signal, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_LEARNING_EVENT_OBSERVE, AIMEE_LEARNING_STAGE_OBSERVE, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_learning_response_decode(response, response_len, sink_mask)
              : -1;
}

static int delegate_canonicalize(const char *role, char *out, size_t out_cap)
{
   uint8_t request[AIMEE_DELEGATES_MESSAGE_LEN], response[AIMEE_DELEGATES_MESSAGE_LEN];
   uint32_t response_len = 0;
   return aimee_delegates_message_encode(AIMEE_DELEGATES_REQUEST_MAGIC, role, request,
                                         sizeof(request)) == 0 &&
                  call_module(AIMEE_DELEGATES_EVENT_INVOKE, AIMEE_DELEGATES_STAGE_INVOKE, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_delegates_message_decode(response, response_len,
                                               AIMEE_DELEGATES_RESPONSE_MAGIC, out, out_cap)
              : -1;
}

static int tool_classify(const char *name, int *classification)
{
   uint8_t request[AIMEE_TOOLS_REQUEST_LEN], response[AIMEE_TOOLS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_tool_class_t result;
   if (!classification || aimee_tools_request_encode(name, request, sizeof(request)) != 0 ||
       call_module(AIMEE_TOOLS_EVENT_DISPATCH, AIMEE_TOOLS_STAGE_DISPATCH, request, sizeof(request),
                   response, sizeof(response), &response_len) != 0 ||
       aimee_tools_response_decode(response, response_len, &result) != 0)
      return -1;
   *classification = (int)result;
   return 0;
}

static int workspace_validate(const char *ref, size_t ref_len, int *allowed)
{
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_workspace_request_encode(ref, ref_len, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_WORKSPACE_EVENT_ACCESS, AIMEE_WORKSPACE_STAGE_ACCESS, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_workspace_response_decode(response, response_len, allowed)
              : -1;
}

static int git_classify(const char *op, aimee_git_classification_t *classification)
{
   uint8_t request[AIMEE_GIT_REQUEST_LEN], response[AIMEE_GIT_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_git_request_encode(op, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_GIT_EVENT_OPERATION, AIMEE_GIT_STAGE_OPERATION, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_git_response_decode(response, response_len, classification)
              : -1;
}

static int git_validate_ref(const char *ref, int *allowed)
{
   uint8_t request[AIMEE_GIT_REF_REQUEST_LEN], response[AIMEE_GIT_REF_RESPONSE_LEN];
   uint32_t response_len = 0;
   return allowed && aimee_git_ref_request_encode(ref, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_GIT_EVENT_REF_VALIDATE, AIMEE_GIT_STAGE_REF_VALIDATE, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_git_ref_response_decode(response, response_len, allowed)
              : -1;
}

static int governance_evaluate(int policy_active, const char *const *tool_names,
                               uint32_t tool_count, const char *stop_reason,
                               aimee_governance_decision_t *decision)
{
   uint8_t request[AIMEE_GOVERNANCE_REQUEST_LEN], response[AIMEE_GOVERNANCE_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!decision ||
       aimee_governance_request_encode(policy_active, tool_names, tool_count, stop_reason, request,
                                       sizeof(request)) != 0 ||
       call_module(AIMEE_GOVERNANCE_EVENT_EVALUATE, AIMEE_GOVERNANCE_STAGE_EVALUATE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_governance_response_decode(response, response_len, tool_count, decision);
}

static int workflows_advance_decide(const char *bound_wi, const wfe_advance_args_t *args,
                                    const char *actual_stage, const char *actual_state,
                                    const char *last_nonce, wfe_advance_outcome_t *outcome)
{
   _Static_assert((int)WFE_ADV_OK == (int)AIMEE_WORKFLOWS_ADVANCE_OK, "workflow outcome drift");
   _Static_assert((int)WFE_ADV_BADARGS == (int)AIMEE_WORKFLOWS_ADVANCE_BADARGS,
                  "workflow outcome drift");
   uint8_t request[AIMEE_WORKFLOWS_REQUEST_LEN], response[AIMEE_WORKFLOWS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_workflows_advance_outcome_t decision;
   if (!args || !outcome ||
       aimee_workflows_request_encode(bound_wi, args->work_item_id, args->observed_stage,
                                      actual_stage, actual_state, args->have_nonce, args->nonce,
                                      last_nonce, request, sizeof(request)) != 0 ||
       call_module(AIMEE_WORKFLOWS_EVENT_ADVANCE, AIMEE_WORKFLOWS_STAGE_ADVANCE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0 ||
       aimee_workflows_response_decode(response, response_len, &decision) != 0)
      return -1;
   *outcome = (wfe_advance_outcome_t)decision;
   return 0;
}

int server_module_skill_should_fire(int hook_count, int interval, int *fire)
{
   uint8_t request[AIMEE_SKILLS_REQUEST_LEN], response[AIMEE_SKILLS_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_skills_request_encode(hook_count, interval, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_SKILLS_EVENT_CONTEXT, AIMEE_SKILLS_STAGE_CONTEXT, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_skills_response_decode(response, response_len, fire)
              : -1;
}

static int skill_trigger_match(const char *content, const char *tool_name, const char *subject,
                               int *match)
{
   if (!match)
      return -1;
   size_t request_len = aimee_skills_trigger_request_size(content, tool_name, subject);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_SKILLS_TRIGGER_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc =
       aimee_skills_trigger_request_encode(content, tool_name, subject, request, request_len) ==
                   0 &&
               call_module(AIMEE_SKILLS_EVENT_TRIGGER, AIMEE_SKILLS_STAGE_TRIGGER, request,
                           (uint32_t)request_len, response, sizeof(response), &response_len) == 0
           ? aimee_skills_trigger_response_decode(response, response_len, match)
           : -1;
   free(request);
   return rc;
}

int server_module_benchmark_score(const int64_t *retrieved, uint32_t retrieved_count,
                                  const int64_t *relevant, uint32_t relevant_count, uint32_t k,
                                  aimee_benchmarks_ir_scores_t *scores)
{
   uint8_t request[AIMEE_BENCHMARKS_REQUEST_LEN], response[AIMEE_BENCHMARKS_RESPONSE_LEN];
   uint32_t response_len = 0;
   return scores &&
                  aimee_benchmarks_request_encode(retrieved, retrieved_count, relevant,
                                                  relevant_count, k, request,
                                                  sizeof(request)) == 0 &&
                  call_module(AIMEE_BENCHMARKS_EVENT_RUN, AIMEE_BENCHMARKS_STAGE_RUN, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_benchmarks_response_decode(response, response_len, scores)
              : -1;
}

int server_module_benchmark_latency(const double *latencies, uint32_t count,
                                    aimee_benchmarks_latency_summary_t *summary)
{
   uint8_t request[AIMEE_BENCHMARKS_LATENCY_REQUEST_LEN];
   uint8_t response[AIMEE_BENCHMARKS_LATENCY_RESPONSE_LEN];
   uint32_t response_len = 0;
   return summary &&
                  aimee_benchmarks_latency_request_encode(latencies, count, request,
                                                          sizeof(request)) == 0 &&
                  call_module(AIMEE_BENCHMARKS_EVENT_LATENCY, AIMEE_BENCHMARKS_STAGE_LATENCY,
                              request, sizeof(request), response, sizeof(response),
                              &response_len) == 0
              ? aimee_benchmarks_latency_response_decode(response, response_len, summary)
              : -1;
}

static int runtime_web_http_status(const char *kind, uint32_t *http_status)
{
   uint8_t request[AIMEE_RUNTIME_WEB_REQUEST_LEN], response[AIMEE_RUNTIME_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   return http_status && aimee_runtime_web_request_encode(kind, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_RUNTIME_WEB_EVENT_CLASSIFY, AIMEE_RUNTIME_WEB_STAGE_CLASSIFY,
                              request, sizeof(request), response, sizeof(response),
                              &response_len) == 0
              ? aimee_runtime_web_response_decode(response, response_len, http_status)
              : -1;
}

static int response_key(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   if (!in || !out || out_cap == 0)
      return -1;
   aimee_response_key_input_t module_input = {.principal = in->principal,
                                              .source = in->source,
                                              .provider = in->provider,
                                              .model = in->model,
                                              .endpoint = in->endpoint,
                                              .idempotency_key = in->idempotency_key,
                                              .body = in->body,
                                              .context = in->context,
                                              .behavior_flags = in->behavior_flags,
                                              .stream = in->stream};
   size_t request_len = aimee_response_request_size(&module_input);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_RESPONSE_KEY_MAX + 4u];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc =
       aimee_response_request_encode(&module_input, request, request_len) == 0 &&
               call_module(AIMEE_RESPONSE_EVENT_COMPOSE, AIMEE_RESPONSE_STAGE_COMPOSE, request,
                           (uint32_t)request_len, response, sizeof(response), &response_len) == 0
           ? aimee_response_response_decode(response, response_len, out, out_cap)
           : -1;
   free(request);
   return rc;
}

void server_module_stage_adapters_configure(void)
{
   ingress_preinject_register_confidence_provider(memory_confidence);
   learning_router_register_signal_classifier(learning_classify);
   delegate_role_register_canonicalizer(delegate_canonicalize);
   agent_tools_register_classifier(tool_classify);
   ws_scope_register_ref_validator(workspace_validate);
   git_ops_register_classifier(git_classify);
   git_ops_register_ref_validator(git_validate_ref);
   gw_response_governance_register_provider(governance_evaluate);
   wfe_advance_register_decision_provider(workflows_advance_decide);
   skill_trigger_register_match_provider(skill_trigger_match);
   response_dedup_register_key_provider(response_key);
   server_error_kind_register_http_status_provider(runtime_web_http_status);
}
