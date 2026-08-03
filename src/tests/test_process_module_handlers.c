#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/roundtable/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
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
DECLARE_HANDLER(aimee_roundtable_module_handler);
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
   test_roundtable();
   test_benchmarks();
   puts("process module handlers: PASS");
   return 0;
}
