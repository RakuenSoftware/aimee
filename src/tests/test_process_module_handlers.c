#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/governance/module_api.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/roundtable/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workflows/module_api.h>
#include <aimee/workspace/module_api.h>

#define DECLARE_HANDLER(name)                                                                      \
   extern aimee_module_status_t name(const aimee_module_invocation_t *, const uint8_t *, uint32_t, \
                                     uint8_t *, uint32_t, uint32_t *, void *)

DECLARE_HANDLER(aimee_memory_module_handler);
DECLARE_HANDLER(aimee_learning_module_handler);
DECLARE_HANDLER(aimee_delegates_module_handler);
DECLARE_HANDLER(aimee_tools_module_handler);
DECLARE_HANDLER(aimee_workspace_module_handler);
DECLARE_HANDLER(aimee_git_module_handler);
DECLARE_HANDLER(aimee_skills_module_handler);
DECLARE_HANDLER(aimee_governance_module_handler);
DECLARE_HANDLER(aimee_workflows_module_handler);
DECLARE_HANDLER(aimee_roundtable_module_handler);
DECLARE_HANDLER(aimee_kb_synthesis_module_handler);
DECLARE_HANDLER(aimee_runtime_web_module_handler);
DECLARE_HANDLER(aimee_benchmarks_module_handler);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static void test_memory(void)
{
   const int64_t scores[] = {0, 329999, 330000, 659999, 660000};
   const aimee_memory_confidence_t expected[] = {
       AIMEE_MEMORY_CONFIDENCE_LOW, AIMEE_MEMORY_CONFIDENCE_LOW, AIMEE_MEMORY_CONFIDENCE_MEDIUM,
       AIMEE_MEMORY_CONFIDENCE_MEDIUM, AIMEE_MEMORY_CONFIDENCE_HIGH};
   for (size_t i = 0; i < sizeof(scores) / sizeof(scores[0]); ++i)
   {
      uint8_t request[AIMEE_MEMORY_REQUEST_LEN], response[AIMEE_MEMORY_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_MEMORY_STAGE_RERANK};
      aimee_memory_confidence_t result;
      assert(aimee_memory_request_encode(scores[i], request, sizeof(request)) == 0);
      assert(aimee_memory_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_memory_response_decode(response, response_len, &result) == 0);
      assert(result == expected[i]);
   }
}

static uint32_t learning_mask(const char *signal)
{
   uint8_t request[AIMEE_LEARNING_REQUEST_LEN], response[AIMEE_LEARNING_RESPONSE_LEN];
   uint32_t response_len = 0, mask = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_LEARNING_STAGE_OBSERVE};
   assert(aimee_learning_request_encode(signal, request, sizeof(request)) == 0);
   assert(aimee_learning_module_handler(&invocation, request, sizeof(request), response,
                                        sizeof(response), &response_len,
                                        NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_learning_response_decode(response, response_len, &mask) == 0);
   return mask;
}

static void test_learning(void)
{
   assert(learning_mask("thumb_up") == AIMEE_LEARNING_SINK_RERANKER);
   assert(
       learning_mask("correction") ==
       (AIMEE_LEARNING_SINK_RERANKER | AIMEE_LEARNING_SINK_SUPERSEDE | AIMEE_LEARNING_SINK_RULE));
   assert(learning_mask("workflow_repetition") == AIMEE_LEARNING_SINK_WORKFLOW);
   assert(learning_mask("unknown") == 0);
}

static void test_delegates(void)
{
   uint8_t request[AIMEE_DELEGATES_MESSAGE_LEN], response[AIMEE_DELEGATES_MESSAGE_LEN];
   uint32_t response_len = 0;
   char role[AIMEE_DELEGATES_ROLE_MAX + 1];
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_INVOKE};
   assert(aimee_delegates_message_encode(AIMEE_DELEGATES_REQUEST_MAGIC, "implement", request,
                                         sizeof(request)) == 0);
   assert(aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_delegates_message_decode(response, response_len, AIMEE_DELEGATES_RESPONSE_MAGIC,
                                         role, sizeof(role)) == 0);
   assert(strcmp(role, "code") == 0);
}

static aimee_tool_class_t tool_class(const char *name)
{
   uint8_t request[AIMEE_TOOLS_REQUEST_LEN], response[AIMEE_TOOLS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_tool_class_t result;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_TOOLS_STAGE_DISPATCH};
   assert(aimee_tools_request_encode(name, request, sizeof(request)) == 0);
   assert(aimee_tools_module_handler(&invocation, request, sizeof(request), response,
                                     sizeof(response), &response_len,
                                     NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_tools_response_decode(response, response_len, &result) == 0);
   return result;
}

static void test_tools(void)
{
   assert(tool_class("bash") == AIMEE_TOOL_CLASS_EXEC);
   assert(tool_class("read_file") == AIMEE_TOOL_CLASS_READ);
   assert(tool_class("mcp:remote") == AIMEE_TOOL_CLASS_REMOTE);
   assert(tool_class("not_registered") == AIMEE_TOOL_CLASS_UNKNOWN);
}

static void test_workspace(void)
{
   char max_ref[AIMEE_WORKSPACE_REF_MAX + 1];
   memset(max_ref, 'a', 64);
   max_ref[64] = '/';
   memset(max_ref + 65, 'b', 64);
   max_ref[AIMEE_WORKSPACE_REF_MAX] = '\0';
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   int allowed = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKSPACE_STAGE_ACCESS};
   assert(aimee_workspace_request_encode(max_ref, strlen(max_ref), request, sizeof(request)) == 0);
   assert(aimee_workspace_get_u16(request + 6) == AIMEE_WORKSPACE_REF_MAX);
   assert(aimee_workspace_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_workspace_response_decode(response, response_len, &allowed) == 0 && allowed);
}

static void test_git(void)
{
   uint8_t request[AIMEE_GIT_REQUEST_LEN], response[AIMEE_GIT_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_git_classification_t result;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_GIT_STAGE_OPERATION};
   assert(aimee_git_request_encode("push", request, sizeof(request)) == 0);
   assert(aimee_git_module_handler(&invocation, request, sizeof(request), response,
                                   sizeof(response), &response_len,
                                   NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_git_response_decode(response, response_len, &result) == 0);
   assert(result.operation == AIMEE_GIT_OP_PUSH && result.needs_credentials);
}

static void test_skills(void)
{
   uint8_t request[AIMEE_SKILLS_REQUEST_LEN], response[AIMEE_SKILLS_RESPONSE_LEN];
   uint32_t response_len = 0;
   int fire = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_SKILLS_STAGE_CONTEXT};
   assert(aimee_skills_request_encode(12, 6, request, sizeof(request)) == 0);
   assert(aimee_skills_module_handler(&invocation, request, sizeof(request), response,
                                      sizeof(response), &response_len,
                                      NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_skills_response_decode(response, response_len, &fire) == 0 && fire);
}

static void test_governance(void)
{
   static const char *inactive_tools[] = {"Agent", "read_file"};
   static const char *denied_tools[] = {"spawn_agent", "Task"};
   static const char *partial_tools[] = {"read_file", "RemoteTrigger", "write_file"};
   static const char *derived_tools[] = {"Agent", "bash"};
   static const char *allowed_tools[] = {"agent", "delegate"};
   static const struct
   {
      int active;
      const char *const *tools;
      uint32_t tool_count;
      const char *stop_reason;
      uint32_t keep_mask;
      uint32_t drop_count;
      const char *final_reason;
   } cases[] = {
       {0, inactive_tools, 2, "", 3, 0, ""},
       {1, NULL, 0, "", 0, 0, ""},
       {1, denied_tools, 2, "tool_use", 0, 2, "end_turn"},
       {1, partial_tools, 3, "max_tokens", 5, 1, "max_tokens"},
       {1, derived_tools, 2, "", 2, 1, "tool_use"},
       {1, allowed_tools, 2, "refusal", 3, 0, "refusal"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_GOVERNANCE_REQUEST_LEN], response[AIMEE_GOVERNANCE_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_governance_decision_t decision;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_GOVERNANCE_STAGE_EVALUATE};
      assert(aimee_governance_request_encode(cases[i].active, cases[i].tools,
                                             cases[i].tool_count, cases[i].stop_reason, request,
                                             sizeof(request)) == 0);
      assert(aimee_governance_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_governance_response_decode(response, response_len, cases[i].tool_count,
                                              &decision) == 0);
      assert(decision.keep_mask == cases[i].keep_mask);
      assert(decision.drop_count == cases[i].drop_count);
      assert(strcmp(decision.stop_reason, cases[i].final_reason) == 0);
   }
}

static void test_workflows(void)
{
   static const struct
   {
      const char *bound;
      const char *work_item;
      const char *observed;
      const char *actual_stage;
      const char *actual_state;
      int have_nonce;
      const char *nonce;
      const char *last_nonce;
      aimee_workflows_advance_outcome_t outcome;
   } cases[] = {
       {"wi_1", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_OK},
       {"", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_UNBOUND},
       {"wi_2", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_UNBOUND},
       {"wi_1", "wi_1", "understand", "split", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_STALE},
       {"wi_1", "wi_1", "understand", "understand", "accepted", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_TERMINAL},
       {"wi_1", "", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_BADARGS},
       {"wi_1", "wi_1", "understand", "split", "accepted", 1, "nonce_1", "nonce_1",
        AIMEE_WORKFLOWS_ADVANCE_REPLAY},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_WORKFLOWS_REQUEST_LEN], response[AIMEE_WORKFLOWS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_workflows_advance_outcome_t outcome;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKFLOWS_STAGE_ADVANCE};
      assert(aimee_workflows_request_encode(
                 cases[i].bound, cases[i].work_item, cases[i].observed, cases[i].actual_stage,
                 cases[i].actual_state, cases[i].have_nonce, cases[i].nonce, cases[i].last_nonce,
                 request, sizeof(request)) == 0);
      assert(aimee_workflows_module_handler(&invocation, request, sizeof(request), response,
                                            sizeof(response), &response_len,
                                            NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_workflows_response_decode(response, response_len, &outcome) == 0);
      assert(outcome == cases[i].outcome);
   }
}

static void test_roundtable(void)
{
   static const struct
   {
      aimee_roundtable_replay_status_t status;
      int factual;
      const char *claimed;
      aimee_roundtable_verify_action_t action;
      const char *severity;
   } cases[] = {
       {AIMEE_ROUNDTABLE_REPLAY_CONTRADICTED, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_REJECT, ""},
       {AIMEE_ROUNDTABLE_REPLAY_VACUOUS, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_REJECT, ""},
       {AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE, 1, "blocking",
        AIMEE_ROUNDTABLE_VERIFY_DEGRADE, "blocking"},
       {AIMEE_ROUNDTABLE_REPLAY_NO_EVIDENCE, 0, "blocking", AIMEE_ROUNDTABLE_VERIFY_CAP,
        "suggestion"},
       {AIMEE_ROUNDTABLE_REPLAY_MATCH, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_KEEP, "blocking"},
       {AIMEE_ROUNDTABLE_REPLAY_MATCH, 0, "blocking", AIMEE_ROUNDTABLE_VERIFY_CAP, "suggestion"},
       {AIMEE_ROUNDTABLE_REPLAY_CORRECTED, 1, "nit", AIMEE_ROUNDTABLE_VERIFY_KEEP, "nit"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_ROUNDTABLE_REQUEST_LEN], response[AIMEE_ROUNDTABLE_RESPONSE_LEN];
      uint32_t response_len = 0;
      char severity[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u];
      aimee_roundtable_verify_action_t action;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_ROUNDTABLE_STAGE_DELIBERATE};
      assert(aimee_roundtable_request_encode(cases[i].status, cases[i].factual, cases[i].claimed,
                                             request, sizeof(request)) == 0);
      assert(aimee_roundtable_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_roundtable_response_decode(response, response_len, &action, severity,
                                              sizeof(severity)) == 0);
      assert(action == cases[i].action && strcmp(severity, cases[i].severity) == 0);
   }
}

static void test_kb_synthesis(void)
{
   static const char *none_string[] = {"No Side Effects"};
   static const char *none_array[] = {"none", "n/a"};
   static const char *honest_string[] = {"writes to disk"};
   static const char *mixed_array[] = {"none", "network"};
   static const char *write_callees[] = {"strlen", "write"};
   static const char *socket_callees[] = {"socket"};
   static const char *ordered_callees[] = {"strlen", "PQexec", "write"};
   static const char *clean_callees[] = {"strlen", "memcpy"};
   static const char *case_callees[] = {"Write", "pqexec"};
   static const struct
   {
      aimee_kb_synthesis_claim_kind_t kind;
      const char *const *claims;
      uint32_t claim_count;
      const char *const *callees;
      uint32_t callee_count;
      int contradicts;
      const char *reason;
   } cases[] = {
       {AIMEE_KB_SYNTHESIS_CLAIM_NONE, NULL, 0, write_callees, 2, 1, "write"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING, none_string, 1, socket_callees, 1, 1, "socket"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, none_array, 2, ordered_callees, 3, 1,
        "PQexec"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, NULL, 0, clean_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING, honest_string, 1, write_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, mixed_array, 2, socket_callees, 1, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING, NULL, 0, write_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_NONE, NULL, 0, case_callees, 2, 0, ""},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
      uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_kb_synthesis_grounding_decision_t decision;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_KB_SYNTHESIS_STAGE_GROUNDING};
      assert(aimee_kb_synthesis_request_encode(
                 cases[i].kind, cases[i].claims, cases[i].claim_count, cases[i].callees,
                 cases[i].callee_count, request, sizeof(request)) == 0);
      assert(aimee_kb_synthesis_module_handler(&invocation, request, sizeof(request), response,
                                               sizeof(response), &response_len,
                                               NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_kb_synthesis_response_decode(response, response_len, &decision) == 0);
      assert(decision.contradicts == cases[i].contradicts);
      assert(strcmp(decision.reason, cases[i].reason) == 0);
   }
}

static void test_runtime_web(void)
{
   static const struct
   {
      const char *kind;
      uint32_t status;
   } cases[] = {
       {"invalid_argument", 400u}, {"not_found", 404u}, {"permission_denied", 403u},
       {"unavailable", 503u},      {"", 502u},          {"unknown", 502u},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_RUNTIME_WEB_REQUEST_LEN];
      uint8_t response[AIMEE_RUNTIME_WEB_RESPONSE_LEN];
      uint32_t response_len = 0, status = 0;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_RUNTIME_WEB_STAGE_CLASSIFY};
      assert(aimee_runtime_web_request_encode(cases[i].kind, request, sizeof(request)) == 0);
      assert(aimee_runtime_web_module_handler(&invocation, request, sizeof(request), response,
                                              sizeof(response), &response_len,
                                              NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_runtime_web_response_decode(response, response_len, &status) == 0);
      assert(status == cases[i].status);
   }
}

static void test_benchmarks(void)
{
   static const int64_t perfect_retrieved[] = {11, 22, 33};
   static const int64_t perfect_relevant[] = {11, 22, 33};
   static const int64_t rank_two_retrieved[] = {5, 9, 7};
   static const int64_t rank_two_relevant[] = {9};
   static const int64_t duplicate_retrieved[] = {7, 7};
   static const int64_t duplicate_relevant[] = {7};
   static const struct
   {
      const int64_t *retrieved;
      uint32_t retrieved_count;
      const int64_t *relevant;
      uint32_t relevant_count;
      uint32_t k;
      double mrr;
      double ndcg;
      double recall;
   } cases[] = {
       {perfect_retrieved, 3, perfect_relevant, 3, 3, 1.0, 1.0, 1.0},
       {rank_two_retrieved, 3, rank_two_relevant, 1, 3, 0.5, 0.6309297535714574, 1.0},
       {duplicate_retrieved, 2, duplicate_relevant, 1, 2, 1.0, 1.6309297535714575, 2.0},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_BENCHMARKS_REQUEST_LEN], response[AIMEE_BENCHMARKS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_benchmarks_ir_scores_t scores;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_BENCHMARKS_STAGE_RUN};
      assert(aimee_benchmarks_request_encode(
                 cases[i].retrieved, cases[i].retrieved_count, cases[i].relevant,
                 cases[i].relevant_count, cases[i].k, request, sizeof(request)) == 0);
      assert(aimee_benchmarks_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_benchmarks_response_decode(response, response_len, &scores) == 0);
      assert(fabs(scores.mrr - cases[i].mrr) < 1e-12);
      assert(fabs(scores.ndcg - cases[i].ndcg) < 1e-12);
      assert(fabs(scores.recall - cases[i].recall) < 1e-12);
   }
}

int main(void)
{
   test_memory();
   test_learning();
   test_delegates();
   test_tools();
   test_workspace();
   test_git();
   test_skills();
   test_governance();
   test_workflows();
   test_roundtable();
   test_kb_synthesis();
   test_runtime_web();
   test_benchmarks();
   puts("process module handlers: PASS");
   return 0;
}
