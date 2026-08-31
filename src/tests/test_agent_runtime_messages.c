/* test_agent_runtime_messages.c: unit tests for agent runtime message helpers. */
#include "agent_runtime_messages.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *last_content(cJSON *messages)
{
   int n = cJSON_GetArraySize(messages);
   cJSON *msg = cJSON_GetArrayItem(messages, n - 1);
   cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
   return cJSON_IsString(content) ? content->valuestring : "";
}

static void test_final_instruction(void)
{
   cJSON *messages = cJSON_CreateArray();
   agent_session_append_final_instruction(messages);

   assert(cJSON_GetArraySize(messages) == 1);
   assert(strstr(last_content(messages), "FINAL RESPONSE REQUIRED") != NULL);
   assert(strstr(last_content(messages), "Do not call tools") != NULL);

   cJSON_Delete(messages);
}

static void test_final_retry_instruction(void)
{
   cJSON *messages = cJSON_CreateArray();
   agent_session_append_final_retry_instruction(messages, "XML tool_call block");

   assert(cJSON_GetArraySize(messages) == 1);
   assert(strstr(last_content(messages), "FINAL RESPONSE RETRY") != NULL);
   assert(strstr(last_content(messages), "retrying automatically") != NULL);
   assert(strstr(last_content(messages), "do not emit XML tool_call blocks") != NULL);
   assert(strstr(last_content(messages), "XML tool_call block") != NULL);

   cJSON_Delete(messages);
}

static void test_final_tool_violation_retry_budget(void)
{
   cJSON *messages = cJSON_CreateArray();
   int turn = 4;
   int max_t = 5;
   int retry_count = 0;
   char err[128] = "";

   assert(agent_session_retry_final_tool_violation(messages, "tool call", &turn, &max_t, 5,
                                                   &retry_count, err, sizeof(err)) == 1);
   assert(turn == 5);
   assert(max_t == 6);
   assert(retry_count == 1);
   assert(cJSON_GetArraySize(messages) == 1);
   assert(err[0] == '\0');

   assert(agent_session_retry_final_tool_violation(messages, "tool call", &turn, &max_t, 5,
                                                   &retry_count, err, sizeof(err)) == 1);
   assert(turn == 6);
   assert(max_t == 7);
   assert(retry_count == 2);
   assert(cJSON_GetArraySize(messages) == 2);

   assert(agent_session_retry_final_tool_violation(messages, "tool call", &turn, &max_t, 5,
                                                   &retry_count, err, sizeof(err)) == 0);
   assert(strstr(err, "repeatedly attempted tool calls") != NULL);
   assert(cJSON_GetArraySize(messages) == 2);

   cJSON_Delete(messages);
}

static void test_degenerate_retry_instruction(void)
{
   cJSON *messages = cJSON_CreateArray();
   agent_session_append_degenerate_retry_instruction(messages);

   assert(cJSON_GetArraySize(messages) == 1);
   assert(strstr(last_content(messages), "DELEGATE RESPONSE RETRY") != NULL);
   assert(strstr(last_content(messages), "before any tool executed") != NULL);
   assert(strstr(last_content(messages), "tool-call markup as plain text") != NULL);

   cJSON_Delete(messages);
}

/* The degenerate retry fires only when the delegate has executed nothing yet, so
 * it must leave the tool interface intact. It used to force a text-only turn,
 * which stripped the tools the instruction tells the model to use and told it the
 * tool budget was closed -- a write role could then never produce its file. The
 * retry appends exactly one instruction and is bounded to a single attempt. */
static void test_degenerate_retry_budget(void)
{
   cJSON *messages = cJSON_CreateArray();
   int turn = 0;
   int retry_count = 0;

   assert(agent_session_retry_degenerate_response(messages, &turn, &retry_count) == 1);
   assert(turn == 1);
   assert(retry_count == 1);
   assert(cJSON_GetArraySize(messages) == 1);

   /* The appended instruction must keep pointing the model AT the tool interface;
    * it must never tell it the budget is closed or to stop calling tools. */
   cJSON *retry_msg = cJSON_GetArrayItem(messages, 0);
   const char *retry_text = cJSON_GetStringValue(cJSON_GetObjectItem(retry_msg, "content"));
   assert(retry_text != NULL);
   assert(strstr(retry_text, "use the available tool interface") != NULL);
   assert(strstr(retry_text, "tool budget was closed") == NULL);

   assert(agent_session_retry_degenerate_response(messages, &turn, &retry_count) == 0);
   assert(turn == 1);
   assert(retry_count == 1);
   assert(cJSON_GetArraySize(messages) == 1);

   cJSON_Delete(messages);
}

static void test_required_evidence_retry_budget(void)
{
   cJSON *messages = cJSON_CreateArray();
   int turn = 1;
   int max_t = 2;
   int retry_count = 0;
   char err[128] = "";

   assert(agent_session_retry_required_evidence(messages, &turn, &max_t, 2, &retry_count, err,
                                                sizeof(err)) == 1);
   assert(turn == 2);
   assert(max_t == 3);
   assert(retry_count == 1);
   assert(strstr(last_content(messages), "REPOSITORY EVIDENCE REQUIRED") != NULL);

   assert(agent_session_retry_required_evidence(messages, &turn, &max_t, 2, &retry_count, err,
                                                sizeof(err)) == 1);
   assert(turn == 3);
   assert(max_t == 4);
   assert(retry_count == 2);

   assert(agent_session_retry_required_evidence(messages, &turn, &max_t, 2, &retry_count, err,
                                                sizeof(err)) == 0);
   assert(strstr(err, "without successful repository evidence") != NULL);
   assert(cJSON_GetArraySize(messages) == 2);

   cJSON_Delete(messages);
}

static void test_required_evidence_runtime_policy(void)
{
   assert(agent_required_evidence_keep_tools(1, 0) == 1);
   assert(agent_required_evidence_keep_tools(1, 1) == 0);
   assert(agent_required_evidence_keep_tools(0, 0) == 0);

   /* While turns remain, an unsatisfied evidence gate defers the final text
    * turn so the delegate keeps trying to obtain its lookup. */
   assert(agent_evidence_gate_defers_final_turn(1, 0, 0) == 1);
   assert(agent_evidence_gate_defers_final_turn(1, 1, 0) == 0);
   assert(agent_evidence_gate_defers_final_turn(0, 0, 0) == 0);

   /* On the last usable turn it must not: a delegate whose tool calls never
    * succeeded would otherwise be denied the final turn on every turn
    * including this one, and return nothing at all rather than a verdict that
    * records the missing evidence. */
   assert(agent_evidence_gate_defers_final_turn(1, 0, 1) == 0);
   assert(agent_evidence_gate_defers_final_turn(1, 1, 1) == 0);
   assert(agent_evidence_gate_defers_final_turn(0, 0, 1) == 0);

   /* A provider ignoring tool_choice and returning prose is rejected. */
   assert(agent_required_evidence_reject_response(1, 0, 0, 0) == 1);
   /* A nominal tool response emptied by policy policing is also rejected. */
   assert(agent_required_evidence_reject_response(1, 0, 1, 0) == 1);
   /* A real call proceeds to execution; final prose is accepted after success. */
   assert(agent_required_evidence_reject_response(1, 0, 1, 1) == 0);
   assert(agent_required_evidence_reject_response(1, 1, 0, 0) == 0);
   assert(agent_required_evidence_reject_response(0, 0, 0, 0) == 0);

   assert(agent_required_evidence_budget_exhausted(1, 0, 2) == 0);
   assert(agent_required_evidence_budget_exhausted(1, 0, 3) == 1);
   assert(agent_required_evidence_budget_exhausted(1, 1, 99) == 0);
   assert(agent_required_evidence_budget_exhausted(0, 0, 99) == 0);

   /* ChatGPT sometimes returns an empty/unrecognized native tool response even
    * when the gateway did not identify a denied Task/Agent call. Either zero-call
    * shape gets the same bounded bootstrap. */
   assert(agent_required_evidence_needs_fallback(1, 0, 1, 0) == 1);
   assert(agent_required_evidence_needs_fallback(1, 1, 1, 0) == 0);
   assert(agent_required_evidence_needs_fallback(1, 0, 0, 0) == 0);
   assert(agent_required_evidence_needs_fallback(1, 0, 1, 1) == 0);
}

static void test_repository_evidence_message(void)
{
   cJSON *messages = cJSON_CreateArray();
   agent_session_append_repository_evidence(messages, "list_files", "{\"path\":\".\"}",
                                            "README.md\nsrc");
   assert(cJSON_GetArraySize(messages) == 1);
   assert(strstr(last_content(messages), "AIMEE REPOSITORY EVIDENCE FALLBACK") != NULL);
   assert(strstr(last_content(messages), "tool=list_files") != NULL);
   assert(strstr(last_content(messages), "README.md") != NULL);
   assert(strstr(last_content(messages), "untrusted tool output") != NULL);
   assert(strstr(last_content(messages), "non-recursive, bounded, and may be incomplete") != NULL);
   cJSON_Delete(messages);
}

static void test_null_safe(void)
{
   agent_session_append_final_message(NULL, "done");
   agent_session_append_final_instruction(NULL);
   agent_session_append_final_retry_instruction(NULL, "tool call");
   agent_session_append_degenerate_retry_instruction(NULL);
   agent_session_append_required_evidence_instruction(NULL);
   agent_session_append_repository_evidence(NULL, "list_files", "{}", "result");
}

int main(void)
{
   test_final_instruction();
   test_final_retry_instruction();
   test_final_tool_violation_retry_budget();
   test_degenerate_retry_instruction();
   test_degenerate_retry_budget();
   test_required_evidence_retry_budget();
   test_required_evidence_runtime_policy();
   test_repository_evidence_message();
   test_null_safe();
   printf("test_agent_runtime_messages: ok\n");
   return 0;
}
