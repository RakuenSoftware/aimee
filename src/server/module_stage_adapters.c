#define _POSIX_C_SOURCE 200809L

#include "module_stage_adapters.h"

#include <aimee/tools/agent_tools.h>
#include <aimee/delegates/delegate_xml_fallback.h>
#include "delegate_verify.h"
#include <aimee/delegates/delegate_economics.h>
#include <aimee/delegates/delegate_patch_coordinator.h>
#include <aimee/git/git_ops.h>
#include "gw_stage_governance.h"
#include "ingress_preinject.h"
#include "wfe_advance.h"
#include <aimee/learning/learning.h>
#include "response_dedup.h"
#include "server_error_kind.h"
#include "modules/memory/memory_extract_patterns.h"
#include "modules/memory/memory_fact_gate.h"
#include "modules/memory/memory_pii_gate.h"
#include "modules/skills/skill_trigger_policy.h"
#include "cmd_agent_delegate_impl.h" /* delegate_routing_register_capability_provider */
#include "modules/webuser/webuser_runtime.h"
#include "modules/workspace/workspace_scope.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/delegate_launch_args.h>
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
   return (int)obs_bus_module_call(event_kind, stage_id, trace, now + MODULE_STAGE_DEADLINE_NS,
                                   request, request_len, response, response_capacity, response_len,
                                   NULL, NULL);
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

static int memory_fact_gate(memory_node_kind_t head_kind, const char *rel_type,
                            memory_node_kind_t tail_kind, int *verdict)
{
   uint8_t request[AIMEE_MEMORY_GATE_REQUEST_LEN], response[AIMEE_MEMORY_GATE_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_memory_fact_verdict_t result;
   if (!verdict)
      return -1;
   if (aimee_memory_gate_request_encode((uint32_t)head_kind, rel_type, (uint32_t)tail_kind, request,
                                        sizeof(request)) != 0)
   {
      /* Only an over-long label can fail encoding here. That is a terminal
       * answer, not a transport failure: BADARG so the caller drops it, where
       * DEFER would ask it to retry a label that will never get shorter. */
      *verdict = AIMEE_MEMORY_FACT_BADARG;
      return 0;
   }
   if (call_module(AIMEE_MEMORY_EVENT_WRITE, AIMEE_MEMORY_STAGE_WRITE, request, sizeof(request),
                   response, sizeof(response), &response_len) != 0 ||
       aimee_memory_gate_response_decode(response, response_len, &result) != 0)
      return -1;
   *verdict = (int)result;
   return 0;
}

/* The wire triple's field capacities have to be pattern_triple_t's own, or a
 * value that fits one side is truncated or refused by the other. This is the
 * only translation unit that sees both, so it is where they are checked. */
_Static_assert(sizeof(((pattern_triple_t *)0)->subject) == AIMEE_MEMORY_TRIPLE_SUBJECT_MAX,
               "wire subject capacity must match pattern_triple_t");
_Static_assert(sizeof(((pattern_triple_t *)0)->rel_type) == AIMEE_MEMORY_TRIPLE_REL_TYPE_MAX,
               "wire rel_type capacity must match pattern_triple_t");
_Static_assert(sizeof(((pattern_triple_t *)0)->object) == AIMEE_MEMORY_TRIPLE_OBJECT_MAX,
               "wire object capacity must match pattern_triple_t");

static int memory_extract(const char *text, pattern_triple_t *out, int max, int *count)
{
   if (!text || !out || max <= 0 || !count)
      return -1;
   size_t request_len = aimee_memory_extract_request_size(text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;

   size_t response_cap = AIMEE_MEMORY_EXTRACT_RESPONSE_MAX(max);
   uint8_t *request = malloc(request_len);
   aimee_memory_triple_t *triples = calloc((size_t)max, sizeof(*triples));
   uint8_t *response = malloc(response_cap);
   uint32_t response_len = 0, found = 0;
   int rc = -1;
   if (request && triples && response && response_cap <= UINT32_MAX &&
       aimee_memory_extract_request_encode(text, (uint32_t)max, request, request_len) == 0 &&
       call_module(AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &response_len) == 0 &&
       aimee_memory_extract_response_decode(response, response_len, triples, (uint32_t)max,
                                            &found) == 0)
   {
      for (uint32_t i = 0; i < found; ++i)
      {
         memset(&out[i], 0, sizeof(out[i]));
         memcpy(out[i].subject, triples[i].subject, sizeof(out[i].subject));
         memcpy(out[i].rel_type, triples[i].rel_type, sizeof(out[i].rel_type));
         memcpy(out[i].object, triples[i].object, sizeof(out[i].object));
         out[i].subject_kind = (memory_node_kind_t)triples[i].subject_kind;
         out[i].object_kind = (memory_node_kind_t)triples[i].object_kind;
      }
      *count = (int)found;
      rc = 0;
   }
   free(request);
   free(triples);
   free(response);
   return rc;
}

_Static_assert(sizeof(((memory_pattern_turn_t *)0)->attr) == AIMEE_MEMORY_SCAN_ATTR_MAX,
               "wire attribute capacity must match memory_pattern_turn_t");

static int memory_scan_turn(const char *text, memory_pattern_turn_t *out)
{
   if (!text || !out)
      return -1;
   size_t request_len = aimee_memory_scan_request_size(text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_MEMORY_SCAN_RESPONSE_MAX];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc = aimee_memory_scan_request_encode(text, request, request_len) == 0 &&
                    call_module(AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX,
                                request, (uint32_t)request_len, response, sizeof(response),
                                &response_len) == 0
                ? aimee_memory_scan_response_decode(response, response_len, &out->is_retraction,
                                                    &out->has_attr, out->attr, sizeof(out->attr))
                : -1;
   free(request);
   return rc;
}

static int memory_pii_turn(const char *turn_text, int *requests_sensitive)
{
   if (!turn_text || !requests_sensitive)
      return -1;
   size_t request_len = aimee_memory_pii_request_size(turn_text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_MEMORY_PII_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc =
       aimee_memory_pii_request_encode(turn_text, request, request_len) == 0 &&
               call_module(AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, request,
                           (uint32_t)request_len, response, sizeof(response), &response_len) == 0
           ? aimee_memory_pii_response_decode(response, response_len, requests_sensitive)
           : -1;
   free(request);
   return rc;
}

/* The wire tiers are compared against rel_sensitivity_t directly, so the two
 * enums have to agree. Checked here, the one place that sees both. */
_Static_assert((int)AIMEE_MEMORY_SENS_NORMAL == (int)SENS_NORMAL &&
                   (int)AIMEE_MEMORY_SENS_PII == (int)SENS_PII &&
                   (int)AIMEE_MEMORY_SENS_SECRET == (int)SENS_SECRET,
               "wire sensitivity tiers must match rel_sensitivity_t");

static int memory_pii_sensitivity(const char *const *rel_types, int count, rel_sensitivity_t *out)
{
   if (!rel_types || !out || count <= 0)
      return -1;
   size_t request_len = aimee_memory_sens_request_size(rel_types, count);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   size_t response_cap = AIMEE_MEMORY_SENS_RESPONSE_MAX(count);
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   aimee_memory_sensitivity_t *tiers = calloc((size_t)count, sizeof(*tiers));
   uint32_t response_len = 0;
   int rc = -1;
   if (request && response && tiers && response_cap <= UINT32_MAX &&
       aimee_memory_sens_request_encode(rel_types, count, request, request_len) == 0 &&
       call_module(AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &response_len) == 0 &&
       aimee_memory_sens_response_decode(response, response_len, tiers, count) == 0)
   {
      for (int i = 0; i < count; ++i)
         out[i] = (rel_sensitivity_t)tiers[i];
      rc = 0;
   }
   free(request);
   free(response);
   free(tiers);
   return rc;
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

/* A prompt is carried whole, so the request is sized to it rather than to a
 * fixed frame. Failure leaves the caller's outputs untouched and reports -1:
 * guessing a capability set here would route work to a model that cannot see
 * half its input, which is worse than refusing to route it. */
static int delegate_infer_caps(const char *prompt, int tools_enabled, unsigned *required_caps,
                               int *min_context)
{
   size_t prompt_len = prompt ? strlen(prompt) : 0;
   if (prompt_len > AIMEE_DELEGATES_CAP_PROMPT_MAX)
      return -1;
   size_t request_cap = AIMEE_DELEGATES_CAP_HEADER_LEN + prompt_len;
   uint8_t *request = malloc(request_cap);
   if (!request)
      return -1;
   size_t request_len =
       aimee_delegates_cap_request_encode(prompt, prompt_len, tools_enabled, request, request_cap);
   uint8_t response[AIMEE_DELEGATES_CAP_RESPONSE_LEN];
   uint32_t response_len = 0;
   int rc =
       request_len > 0 &&
               call_module(AIMEE_DELEGATES_EVENT_CAPABILITIES, AIMEE_DELEGATES_STAGE_CAPABILITIES,
                           request, (uint32_t)request_len, response, sizeof(response),
                           &response_len) == 0
           ? aimee_delegates_cap_response_decode(response, response_len, required_caps, min_context)
           : -1;
   free(request);
   return rc;
}

static int delegate_chain(unsigned op, int has_depth, int has_parent, int parent_known,
                          int parent_active, int parent_depth, int max_depth, int *flag,
                          int32_t *current_depth)
{
   uint8_t request[AIMEE_DELEGATES_CHAIN_REQUEST_LEN];
   uint8_t response[AIMEE_DELEGATES_CHAIN_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_delegates_chain_request_encode(op, has_depth, has_parent, parent_known,
                                               parent_active, (int32_t)parent_depth,
                                               (int32_t)max_depth, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_DELEGATES_EVENT_CHAIN, AIMEE_DELEGATES_STAGE_CHAIN, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_delegates_chain_response_decode(response, response_len, flag, current_depth)
              : -1;
}

static int delegate_paths(const char *prompt, unsigned max_paths, char *paths, size_t path_stride)
{
   size_t prompt_len = prompt ? strlen(prompt) : 0;
   if (prompt_len > AIMEE_DELEGATES_PATHS_PROMPT_MAX)
      return -1;
   size_t request_cap = AIMEE_DELEGATES_PATHS_HEADER_LEN + prompt_len;
   uint8_t *request = malloc(request_cap);
   if (!request)
      return -1;
   size_t request_len =
       aimee_delegates_paths_request_encode(prompt, prompt_len, max_paths, request, request_cap);

   /* Bounded by what the module can return: a count it caps, each path bounded
    * by the caller's stride, plus a two-byte length prefix each. */
   uint32_t response_cap =
       (uint32_t)(AIMEE_DELEGATES_PATHS_RESP_HEADER_LEN + max_paths * (path_stride + 2));
   uint8_t *response = malloc(response_cap);
   if (!response)
   {
      free(request);
      return -1;
   }
   uint32_t response_len = 0;
   int rc = request_len > 0 &&
                    call_module(AIMEE_DELEGATES_EVENT_PATHS, AIMEE_DELEGATES_STAGE_PATHS, request,
                                (uint32_t)request_len, response, response_cap, &response_len) == 0
                ? aimee_delegates_paths_response_decode(response, response_len, paths, path_stride,
                                                        max_paths)
                : -1;
   free(request);
   free(response);
   return rc;
}

static int delegate_handoff(const char *text, const char *owned_files_json,
                            int require_verification, delegate_handoff_validation_t *out)
{
   size_t text_len = text ? strlen(text) : 0;
   size_t owned_len = owned_files_json ? strlen(owned_files_json) : 0;
   if (text_len > AIMEE_DELEGATES_HANDOFF_TEXT_MAX || owned_len > AIMEE_DELEGATES_HANDOFF_TEXT_MAX)
      return -1;
   size_t request_cap = AIMEE_DELEGATES_HANDOFF_HEADER_LEN + text_len + owned_len;
   uint8_t *request = malloc(request_cap);
   if (!request)
      return -1;
   size_t request_len = aimee_delegates_handoff_request_encode(
       text, text_len, owned_files_json, owned_len, require_verification, request, request_cap);

   uint8_t response[AIMEE_DELEGATES_HANDOFF_RESPONSE_LEN];
   uint32_t response_len = 0;
   int rc =
       request_len > 0 &&
               call_module(AIMEE_DELEGATES_EVENT_HANDOFF, AIMEE_DELEGATES_STAGE_HANDOFF, request,
                           (uint32_t)request_len, response, sizeof(response), &response_len) == 0
           ? 0
           : -1;
   free(request);
   if (rc != 0 || response_len != AIMEE_DELEGATES_HANDOFF_RESPONSE_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_HANDOFF_RESPONSE_MAGIC)
      return -1;

   out->valid = (int)aimee_delegates_get_u32(response + 4);
   out->repair_attempted = (int)aimee_delegates_get_u32(response + 8);
   out->done_without_verification = (int)aimee_delegates_get_u32(response + 12);
   out->needs_supervisor_review = (int)aimee_delegates_get_u32(response + 16);
   out->changed_files_count = (int)aimee_delegates_get_u32(response + 20);
   out->outside_ownership_count = (int)aimee_delegates_get_u32(response + 24);
   out->passed_tests = (int)aimee_delegates_get_u32(response + 28);
   out->commands_run = (int)aimee_delegates_get_u32(response + 32);
   aimee_delegates_handoff_field(response, 36, AIMEE_DELEGATES_HANDOFF_STATUS_LEN, out->status,
                                 sizeof(out->status));
   aimee_delegates_handoff_field(response, 36 + AIMEE_DELEGATES_HANDOFF_STATUS_LEN,
                                 AIMEE_DELEGATES_HANDOFF_STATUS_LEN, out->raw_status,
                                 sizeof(out->raw_status));
   aimee_delegates_handoff_field(response, 36 + 2 * AIMEE_DELEGATES_HANDOFF_STATUS_LEN,
                                 AIMEE_DELEGATES_HANDOFF_ERROR_LEN, out->error, sizeof(out->error));
   /* A malformed handoff is a verdict, not a transport failure: the module
    * answered, and the answer is "not valid". The caller distinguishes the two
    * by the return code, so an invalid handoff must report non-zero here while
    * still carrying its reason in *out. */
   return out->valid ? 0 : -1;
}

/* Recovering tool calls from prose: the caller's tool inventory is gathered
 * here, at the boundary, because the module cannot look tools up itself and the
 * call sites should not have to carry the list around. */
static int delegate_rescue(const char *text, int allow_json, int detect_only,
                           parsed_response_t *out)
{
   const char *names[AIMEE_DELEGATES_RESCUE_KNOWN_MAX];
   size_t text_len = text ? strlen(text) : 0;
   if (text_len == 0 || text_len > AIMEE_DELEGATES_RESCUE_TEXT_MAX)
      return 0;

   int name_count = agent_tool_known_names(names, (int)AIMEE_DELEGATES_RESCUE_KNOWN_MAX);
   size_t request_cap = AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN + text_len;
   for (int i = 0; i < name_count; i++)
      request_cap += 2 + strlen(names[i]);

   uint8_t *request = malloc(request_cap);
   if (!request)
      return 0;
   size_t request_len = aimee_delegates_rescue_request_encode(
       text, text_len, names, (size_t)name_count, allow_json,
       detect_only ? AIMEE_DELEGATES_RESCUE_MODE_DETECT : AIMEE_DELEGATES_RESCUE_MODE_PARSE,
       request, request_cap);
   if (request_len == 0)
   {
      free(request);
      return 0;
   }

   /* A rescued call cannot be larger than the text it was read out of, plus
    * per-call framing. */
   size_t response_cap = AIMEE_DELEGATES_RESCUE_RESP_HEADER_LEN + text_len +
                         (size_t)AGENT_MAX_TOOL_CALLS * (8u + 64u + 32u) + 64u;
   uint8_t *response = malloc(response_cap);
   if (!response)
   {
      free(request);
      return 0;
   }

   uint32_t response_len = 0;
   int rc = call_module(AIMEE_DELEGATES_EVENT_RESCUE, AIMEE_DELEGATES_STAGE_RESCUE, request,
                        (uint32_t)request_len, response, (uint32_t)response_cap, &response_len);
   free(request);
   if (rc != 0 || response_len < AIMEE_DELEGATES_RESCUE_RESP_HEADER_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_RESCUE_RESPONSE_MAGIC)
   {
      free(response);
      return 0;
   }

   int is_tool_call = (int)aimee_delegates_get_u32(response + 4);
   if (detect_only)
   {
      free(response);
      return is_tool_call;
   }

   uint32_t count = aimee_delegates_get_u32(response + 8);
   uint32_t content_len = aimee_delegates_get_u32(response + 12);
   size_t at = AIMEE_DELEGATES_RESCUE_RESP_HEADER_LEN;
   if (count > AGENT_MAX_TOOL_CALLS || at + content_len > response_len)
   {
      free(response);
      return 0;
   }

   if (content_len > 0 && !out->content)
   {
      out->content = malloc(content_len + 1);
      if (out->content)
      {
         memcpy(out->content, response + at, content_len);
         out->content[content_len] = '\0';
      }
   }
   at += content_len;

   for (uint32_t i = 0; i < count && out->call_count < AGENT_MAX_TOOL_CALLS; i++)
   {
      if (at + 8 > response_len)
         break;
      size_t id_len = (size_t)response[at] | ((size_t)response[at + 1] << 8);
      size_t name_len = (size_t)response[at + 2] | ((size_t)response[at + 3] << 8);
      size_t args_len = aimee_delegates_get_u32(response + at + 4);
      at += 8;
      if (at + id_len + name_len + args_len > response_len)
         break;

      parsed_tool_call_t *tc = &out->calls[out->call_count++];
      memset(tc, 0, sizeof(*tc));
      size_t n = id_len < sizeof(tc->id) ? id_len : sizeof(tc->id) - 1;
      memcpy(tc->id, response + at, n);
      at += id_len;
      n = name_len < sizeof(tc->name) ? name_len : sizeof(tc->name) - 1;
      memcpy(tc->name, response + at, n);
      at += name_len;
      tc->arguments = malloc(args_len + 1);
      if (tc->arguments)
      {
         memcpy(tc->arguments, response + at, args_len);
         tc->arguments[args_len] = '\0';
      }
      at += args_len;
   }

   if (is_tool_call && out->call_count > 0)
      out->is_tool_call = 1;
   free(response);
   return (int)count;
}

static int delegate_verify(int op, int a, int b, int max_signal_status, int *outcome_out,
                           int *escalate_out)
{
   uint8_t request[AIMEE_DELEGATES_VERIFY_REQUEST_LEN];
   uint8_t response[AIMEE_DELEGATES_VERIFY_RESPONSE_LEN];
   uint32_t response_len = 0;

   if (!outcome_out || !escalate_out ||
       aimee_delegates_verify_request_encode((unsigned)op, a, b, max_signal_status, request,
                                             sizeof(request)) != 0 ||
       call_module(AIMEE_DELEGATES_EVENT_VERIFY, AIMEE_DELEGATES_STAGE_VERIFY, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0 ||
       response_len != AIMEE_DELEGATES_VERIFY_RESPONSE_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_VERIFY_RESPONSE_MAGIC)
      return -1;

   *outcome_out = (int)aimee_delegates_get_u32(response + 4);
   *escalate_out = (int)aimee_delegates_get_u32(response + 8);
   return 0;
}

/* A coordinated run's cost to the supervisor. The tasks and the agent tiers are
 * gathered here because they are the caller's rows and the caller's config; the
 * module reads the four fields the rule needs and forgets them. */
static void delegate_economics(const db1_coord_task_t *tasks, int task_count,
                               const agent_config_t *cfg, delegate_economics_report_t *out)
{
   if (!out || task_count < 0 || (uint32_t)task_count > AIMEE_DELEGATES_ECON_MAX_TASKS)
      return;
   int agent_count = cfg ? cfg->agent_count : 0;
   if (agent_count < 0 || (uint32_t)agent_count > AIMEE_DELEGATES_ECON_MAX_AGENTS)
      return;

   size_t cap = AIMEE_DELEGATES_ECON_REQ_HEADER_LEN;
   for (int i = 0; i < task_count; i++)
   {
      cap += 12 + strlen(tasks[i].status) + strlen(tasks[i].claimed_by) + strlen(tasks[i].files) +
             strlen(tasks[i].result);
   }
   for (int i = 0; i < agent_count; i++)
      cap += 2 + strlen(cfg->agents[i].name) + 4;

   uint8_t *request = malloc(cap);
   if (!request)
      return;
   size_t at = aimee_delegates_econ_request_begin((uint32_t)task_count, (uint32_t)agent_count,
                                                  request, cap);
   for (int i = 0; i < task_count && at; i++)
   {
      at = aimee_delegates_econ_put_task(tasks[i].status, tasks[i].claimed_by, tasks[i].files,
                                         tasks[i].result, request, at, cap);
   }
   for (int i = 0; i < agent_count && at; i++)
      at = aimee_delegates_econ_put_agent(cfg->agents[i].name, cfg->agents[i].cost_tier, request,
                                          at, cap);
   if (at == 0 || at > UINT32_MAX)
   {
      free(request);
      return;
   }

   uint8_t response[AIMEE_DELEGATES_ECON_RESPONSE_LEN];
   uint32_t response_len = 0;
   int rc = call_module(AIMEE_DELEGATES_EVENT_ECONOMICS, AIMEE_DELEGATES_STAGE_ECONOMICS, request,
                        (uint32_t)at, response, sizeof(response), &response_len);
   free(request);
   if (rc != 0 || response_len != AIMEE_DELEGATES_ECON_RESPONSE_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_ECON_RESPONSE_MAGIC)
      return;

   int *fields[] = {
       &out->delegate_count,
       &out->tier_counts[0],
       &out->tier_counts[1],
       &out->tier_counts[2],
       &out->tier_counts[3],
       &out->unknown_tier_count,
       &out->prompt_tokens_total,
       &out->completion_tokens_total,
       &out->delegate_tokens_estimated,
       &out->tokenized_delegate_results,
       &out->supervisor_prompt_tokens_estimated,
       &out->handoff_count,
       &out->valid_handoffs,
       &out->invalid_handoffs,
       &out->focused_tests_run_by_delegates,
       &out->delegates_with_focused_tests,
       &out->manual_integration_events,
       &out->supervisor_actions_required,
       &out->reviewer_findings_blocking,
   };
   for (unsigned i = 0; i < AIMEE_DELEGATES_ECON_FIELD_COUNT; i++)
      *fields[i] = (int)aimee_delegates_get_u32(response + 4 + i * 4);

   size_t off = 4 + AIMEE_DELEGATES_ECON_FIELD_COUNT * 4;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_ECON_VERDICT_LEN, out->verdict,
                                 sizeof(out->verdict));
   off += AIMEE_DELEGATES_ECON_VERDICT_LEN;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_ECON_ADVICE_LEN,
                                 out->recommendation, sizeof(out->recommendation));
   off += AIMEE_DELEGATES_ECON_ADVICE_LEN;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_ECON_LABEL_LEN, out->verdict_label,
                                 sizeof(out->verdict_label));
   off += AIMEE_DELEGATES_ECON_LABEL_LEN;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_ECON_LABEL_LEN,
                                 out->cost_model_label, sizeof(out->cost_model_label));
}

/* Where a run's patches stand. The task rows are gathered here; the module
 * reads the six fields the rule needs and forgets them. */
static void delegate_patch_coord(const db1_coord_task_t *tasks, int task_count,
                                 delegate_patch_report_t *out)
{
   if (!out || task_count < 0 || (uint32_t)task_count > AIMEE_DELEGATES_PATCH_MAX_TASKS)
      return;

   size_t cap = AIMEE_DELEGATES_PATCH_REQ_HEADER_LEN;
   for (int i = 0; i < task_count; i++)
   {
      cap += 20 + strlen(tasks[i].status) + strlen(tasks[i].error) + strlen(tasks[i].files) +
             strlen(tasks[i].result);
   }

   uint8_t *request = malloc(cap);
   if (!request)
      return;
   size_t at = aimee_delegates_patch_request_begin((uint32_t)task_count, request, cap);
   for (int i = 0; i < task_count && at; i++)
   {
      at = aimee_delegates_patch_put_task(tasks[i].id, tasks[i].step_id, tasks[i].status,
                                          tasks[i].error, tasks[i].files, tasks[i].result, request,
                                          at, cap);
   }
   if (at == 0 || at > UINT32_MAX)
   {
      free(request);
      return;
   }

   size_t response_cap = AIMEE_DELEGATES_PATCH_RESP_HEADER_LEN +
                         AIMEE_DELEGATES_PATCH_MAX_TASKS * AIMEE_DELEGATES_PATCH_TASK_REC_LEN;
   uint8_t *response = malloc(response_cap);
   if (!response)
   {
      free(request);
      return;
   }

   uint32_t response_len = 0;
   int rc = call_module(AIMEE_DELEGATES_EVENT_PATCH, AIMEE_DELEGATES_STAGE_PATCH, request,
                        (uint32_t)at, response, (uint32_t)response_cap, &response_len);
   free(request);
   if (rc != 0 || response_len < AIMEE_DELEGATES_PATCH_RESP_HEADER_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_PATCH_RESPONSE_MAGIC)
   {
      free(response);
      return;
   }

   int *run_fields[] = {
       &out->task_count,
       &out->implementation_packets,
       &out->planned,
       &out->running,
       &out->returned,
       &out->verified,
       &out->reviewable,
       &out->accepted,
       &out->failed,
       &out->needs_supervisor,
       &out->invalid_handoffs,
       &out->outside_ownership_touches,
       &out->patch_overlaps,
       &out->stale_worktrees,
       &out->focused_tests_passed,
       &out->reviewer_packets,
       &out->reviewer_blocking_findings,
       &out->reviewer_owner_packet_routes,
   };
   for (unsigned i = 0; i < AIMEE_DELEGATES_PATCH_RUN_FIELDS; i++)
      *run_fields[i] = (int)aimee_delegates_get_u32(response + 4 + i * 4);

   size_t off = 4 + AIMEE_DELEGATES_PATCH_RUN_FIELDS * 4;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_PATCH_STATE_LEN,
                                 out->reviewer_status, sizeof(out->reviewer_status));
   off += AIMEE_DELEGATES_PATCH_STATE_LEN;
   aimee_delegates_handoff_field(response, off, AIMEE_DELEGATES_PATCH_NEXTCMD_LEN,
                                 out->recommended_next_command,
                                 sizeof(out->recommended_next_command));

   int count = out->task_count;
   if (count < 0 || (uint32_t)count > AIMEE_DELEGATES_PATCH_MAX_TASKS ||
       response_len != AIMEE_DELEGATES_PATCH_RESP_HEADER_LEN +
                           (uint32_t)count * AIMEE_DELEGATES_PATCH_TASK_REC_LEN)
   {
      /* The header and the body disagree: report nothing rather than a
       * partially decoded run. */
      memset(out, 0, sizeof(*out));
      free(response);
      return;
   }

   for (int i = 0; i < count; i++)
   {
      const uint8_t *rec = response + AIMEE_DELEGATES_PATCH_RESP_HEADER_LEN +
                           (size_t)i * AIMEE_DELEGATES_PATCH_TASK_REC_LEN;
      delegate_patch_task_report_t *tr = &out->tasks[i];
      int *task_fields[] = {
          &tr->task_id,
          &tr->step_id,
          &tr->handoff_valid,
          &tr->changed_files_count,
          &tr->passed_tests,
          &tr->outside_ownership_count,
          &tr->overlap_task_id,
          &tr->stale_base,
          &tr->supervisor_actions,
      };
      for (unsigned f = 0; f < AIMEE_DELEGATES_PATCH_TASK_FIELDS; f++)
         *task_fields[f] = (int)aimee_delegates_get_u32(rec + f * 4);

      size_t s_off = AIMEE_DELEGATES_PATCH_TASK_FIELDS * 4;
      aimee_delegates_handoff_field(rec, s_off, AIMEE_DELEGATES_PATCH_STATE_LEN, tr->task_status,
                                    sizeof(tr->task_status));
      s_off += AIMEE_DELEGATES_PATCH_STATE_LEN;
      aimee_delegates_handoff_field(rec, s_off, AIMEE_DELEGATES_PATCH_STATE_LEN, tr->patch_state,
                                    sizeof(tr->patch_state));
      s_off += AIMEE_DELEGATES_PATCH_STATE_LEN;
      aimee_delegates_handoff_field(rec, s_off, AIMEE_DELEGATES_PATCH_STATE_LEN, tr->handoff_status,
                                    sizeof(tr->handoff_status));
      s_off += AIMEE_DELEGATES_PATCH_STATE_LEN;
      aimee_delegates_handoff_field(rec, s_off, AIMEE_DELEGATES_PATCH_NOTE_LEN, tr->note,
                                    sizeof(tr->note));
   }
   free(response);
}

/* What a role implies about how it is run. One call answers whichever question
 * the caller asked; the module resolves the alias itself, so every answer is
 * computed from the same canonical spelling. */
static int delegate_role_policy(int op, const char *role, int a, int b, int c, int *out)
{
   uint8_t request[AIMEE_DELEGATES_ROLEPOL_REQUEST_LEN];
   uint8_t response[AIMEE_DELEGATES_ROLEPOL_RESPONSE_LEN];
   uint32_t response_len = 0;

   if (!out ||
       aimee_delegates_rolepol_request_encode(role, a, b, c, request, sizeof(request)) != 0 ||
       call_module(AIMEE_DELEGATES_EVENT_ROLEPOL, AIMEE_DELEGATES_STAGE_ROLEPOL, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0 ||
       response_len != AIMEE_DELEGATES_ROLEPOL_RESPONSE_LEN ||
       aimee_delegates_get_u32(response) != AIMEE_DELEGATES_ROLEPOL_RESPONSE_MAGIC)
      return -1;

   switch (op)
   {
   case DELEGATE_ROLE_OP_IS_WRITE:
      *out = (int)aimee_delegates_get_u32(response + 4);
      return 0;
   case DELEGATE_ROLE_OP_BUILTIN:
      *out = (int)aimee_delegates_get_u32(response + 8);
      return 0;
   case DELEGATE_ROLE_OP_CACHE:
      *out = (int)aimee_delegates_get_u32(response + 12);
      return 0;
   case DELEGATE_ROLE_OP_AUTO_TOOLS:
      *out = (int)aimee_delegates_get_u32(response + 16);
      return 0;
   case DELEGATE_ROLE_OP_FINAL_TURNS:
      *out = (int)aimee_delegates_get_u32(response + 20);
      return 0;
   case DELEGATE_ROLE_OP_PARENT_DIFF:
      *out = (int)aimee_delegates_get_u32(response + 24);
      return 0;
   case DELEGATE_ROLE_OP_TASK_SHAPE:
      *out = (int)aimee_delegates_get_u32(response + 28);
      return 0;
   default:
      return -1;
   }
}

/* The create argv for a delegate container, decided by the module.
 *
 * The request is built here and the answer is used verbatim: this adapter makes
 * no decision about mounts, environment, the container's name or its flags. It
 * is a wire, and deliberately nothing more -- a "small fix" applied here would
 * be a rule with none of the module's checks behind it.
 *
 * The mount table is the largest field (the runtime's report of this process's
 * own mounts), so the request buffer is sized for it rather than hand-counted. */
static int delegate_launch_args(const aimee_delegates_launch_spec_t *spec, char *name_out,
                                size_t name_cap, const char **argv_out, size_t argv_cap,
                                size_t *arg_len_out, uint8_t *buf, size_t buf_cap)
{
   if (!spec || !buf || buf_cap < 2)
      return -1;

   size_t request_cap = 1u << 20;
   uint8_t *request = malloc(request_cap);
   if (!request)
      return -1;
   size_t request_len = aimee_delegates_launch_request_encode(spec, request, request_cap);
   if (request_len == 0)
   {
      free(request);
      return -1;
   }

   /* One byte of slack: the caller NUL-terminates the last argv entry in place,
    * which writes one past the response. */
   uint32_t response_len = 0;
   int rc = call_module(AIMEE_DELEGATES_EVENT_LAUNCH, AIMEE_DELEGATES_STAGE_LAUNCH, request,
                        (uint32_t)request_len, buf, (uint32_t)(buf_cap - 1), &response_len);
   free(request);
   if (rc != 0)
      return -1;

   return aimee_delegates_launch_response_decode(buf, response_len, name_out, name_cap, argv_out,
                                                 argv_cap, arg_len_out);
}

/* The Dockerfile a sandbox image is built from, and the tag naming its content.
 * A wire, like the launch adapter: it validates nothing and renders nothing. */
static int delegate_image_spec(const char *base, const char *const *pkgs, int npkgs,
                               const char *verbatim, char *tag, size_t tag_cap, char *dockerfile,
                               size_t df_cap)
{
   /* An operator-committed Dockerfile can be large, so both buffers are heap. */
   size_t request_cap = 1u << 20;
   size_t response_cap = (1u << 20) + 4096;
   uint8_t *request = malloc(request_cap);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   size_t request_len =
       aimee_delegates_imgspec_request_encode(base, pkgs, npkgs, verbatim, request, request_cap);
   if (request_len == 0)
   {
      free(request);
      free(response);
      return -1;
   }

   uint32_t response_len = 0;
   int rc = call_module(AIMEE_DELEGATES_EVENT_IMGSPEC, AIMEE_DELEGATES_STAGE_IMGSPEC, request,
                        (uint32_t)request_len, response, (uint32_t)response_cap, &response_len);
   free(request);
   if (rc == 0)
      rc = aimee_delegates_imgspec_response_decode(response, response_len, tag, tag_cap, dockerfile,
                                                   df_cap);
   else
      rc = -1;
   free(response);
   return rc;
}

/* The isolation verdict for a container the runtime just started. A wire. */
static int delegate_isolation(const char *report, int probe_failed, int require_isolation,
                              int *refuse, int *warn, int *is_error, char *reason,
                              size_t reason_cap)
{
   uint8_t request[AIMEE_DELEGATES_ISOLATION_REPORT_MAX + 64];
   size_t request_len = aimee_delegates_isolation_request_encode(
       report, probe_failed, require_isolation, request, sizeof(request));
   if (request_len == 0)
      return -1;

   uint8_t response[2048];
   uint32_t response_len = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_ISOLATION, AIMEE_DELEGATES_STAGE_ISOLATION, request,
                   (uint32_t)request_len, response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_delegates_isolation_response_decode(response, response_len, refuse, warn, is_error,
                                                    reason, reason_cap);
}

/* Which built sandbox images may be deleted. A wire: the inventory goes out and
 * the verdicts come back, and nothing here decides either. */
static int delegate_image_gc(const uint8_t *request, size_t request_len, uint8_t *response,
                             size_t response_cap, size_t *response_len)
{
   uint32_t got = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_IMGGC, AIMEE_DELEGATES_STAGE_IMGGC, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &got) != 0)
      return -1;
   *response_len = got;
   return 0;
}

/* Which agents may serve this packet. A wire. */
static int delegate_route_filter(const uint8_t *request, size_t request_len, uint8_t *response,
                                 size_t response_cap, size_t *response_len)
{
   uint32_t got = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_ROUTEFILTER, AIMEE_DELEGATES_STAGE_ROUTEFILTER, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &got) != 0)
      return -1;
   *response_len = got;
   return 0;
}

/* Did a successful write delegate change anything? A wire. */
static int delegate_noop_write(unsigned flags, int named_count, int *noop, int *benign,
                               char *message, size_t message_cap)
{
   uint8_t request[AIMEE_DELEGATES_NOOPWRITE_REQUEST_LEN];
   if (aimee_delegates_noopwrite_request_encode(flags, named_count, request, sizeof(request)) != 0)
      return -1;

   uint8_t response[1024];
   uint32_t response_len = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_NOOPWRITE, AIMEE_DELEGATES_STAGE_NOOPWRITE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_delegates_noopwrite_response_decode(response, response_len, noop, benign, message,
                                                    message_cap);
}

/* What a delegate plan becomes. A passthrough: the caller encodes, because the
 * caller is what holds the plan and the filesystem facts that go with it. */
static int delegate_launch_plan(const uint8_t *request, size_t request_len, uint8_t *response,
                                size_t response_cap, size_t *response_len)
{
   uint32_t got = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_LAUNCHPLAN, AIMEE_DELEGATES_STAGE_LAUNCHPLAN, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &got) != 0)
      return -1;
   *response_len = got;
   return 0;
}

/* Did a review look at what it reviewed? A wire. */
static int delegate_review_evidence(const char *role, const char *response, unsigned flags,
                                    unsigned *verdict, char *message, size_t message_cap)
{
   /* A delegate's report, plus a short role. Larger than any real review; a
    * report that does not fit is not judged rather than judged in part. */
   static _Thread_local uint8_t request[256 * 1024];
   int request_len =
       aimee_delegates_reviewev_request_encode(role, response, flags, request, sizeof(request));
   if (request_len < 0)
      return -1;

   uint8_t buf[1024];
   uint32_t response_len = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_REVIEWEV, AIMEE_DELEGATES_STAGE_REVIEWEV, request,
                   (uint32_t)request_len, buf, sizeof(buf), &response_len) != 0)
      return -1;
   return aimee_delegates_reviewev_response_decode(buf, response_len, verdict, message,
                                                   message_cap);
}

/* Did the delegate touch the files its brief named? A passthrough: the caller
 * encodes, because the caller holds the paths and the facts about them. */
static int delegate_drift(const uint8_t *request, size_t request_len, unsigned *severity,
                          char *message, size_t message_cap)
{
   uint8_t response[4096];
   uint32_t response_len = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_DRIFT, AIMEE_DELEGATES_STAGE_DRIFT, request,
                   (uint32_t)request_len, response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_delegates_drift_response_decode(response, response_len, severity, message,
                                                message_cap);
}

/* What a delegate may do. A passthrough: the caller encodes, because the caller
 * holds the role and the definition that came with it. */
static int delegate_permissions(const uint8_t *request, size_t request_len, uint8_t *response,
                                size_t response_cap, size_t *response_len)
{
   uint32_t got = 0;
   if (call_module(AIMEE_DELEGATES_EVENT_PERMS, AIMEE_DELEGATES_STAGE_PERMS, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &got) != 0)
      return -1;
   *response_len = got;
   return 0;
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
   if (!allowed || aimee_git_ref_request_encode(ref, request, sizeof(request)) != 0)
      return -1;
   int result = call_module(AIMEE_GIT_EVENT_REF_VALIDATE, AIMEE_GIT_STAGE_REF_VALIDATE, request,
                            sizeof(request), response, sizeof(response), &response_len);
   return result == AIMEE_MODULE_CALL_OK
              ? aimee_git_ref_response_decode(response, response_len, allowed)
              : result;
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
   memory_fact_gate_register_checker(memory_fact_gate);
   memory_extract_register_extractor(memory_extract);
   memory_extract_register_turn_scanner(memory_scan_turn);
   memory_pii_register_turn_classifier(memory_pii_turn);
   memory_pii_register_sensitivity_batch(memory_pii_sensitivity);
   learning_router_register_signal_classifier(learning_classify);
   delegate_role_register_canonicalizer(delegate_canonicalize);
   delegate_routing_register_capability_provider(delegate_infer_caps);
   delegate_register_chain_provider(delegate_chain);
   delegate_register_paths_provider(delegate_paths);
   delegate_register_handoff_provider(delegate_handoff);
   delegate_register_rescue_provider(delegate_rescue);
   delegate_register_verify_provider(delegate_verify);
   delegate_register_economics_provider(delegate_economics);
   delegate_register_patch_provider(delegate_patch_coord);
   delegate_register_role_policy_provider(delegate_role_policy);
   delegate_register_launch_args_provider(delegate_launch_args);
   delegate_register_image_spec_provider(delegate_image_spec);
   delegate_register_isolation_provider(delegate_isolation);
   delegate_register_image_gc_provider(delegate_image_gc);
   delegate_register_route_filter_provider(delegate_route_filter);
   delegate_register_noop_write_provider(delegate_noop_write);
   delegate_register_launch_plan_provider(delegate_launch_plan);
   delegate_register_review_evidence_provider(delegate_review_evidence);
   delegate_register_drift_provider(delegate_drift);
   delegate_register_permissions_provider(delegate_permissions);
   agent_tools_register_classifier(tool_classify);
   ws_scope_register_ref_validator(workspace_validate);
   /* Same decision, same owner: webuser's runtime dir names a single path
    * component, and workspace owns what a reference may be. One registration
    * serves both seams so the rule cannot drift between them. */
   webuser_runtime_register_name_validator(workspace_validate);
   git_ops_register_classifier(git_classify);
   git_ops_register_ref_validator(git_validate_ref);
   gw_response_governance_register_provider(governance_evaluate);
   wfe_advance_register_decision_provider(workflows_advance_decide);
   skill_trigger_register_match_provider(skill_trigger_match);
   response_dedup_register_key_provider(response_key);
   server_error_kind_register_http_status_provider(runtime_web_http_status);
}
