/* test_agent_repair.c: message_history_repair tests, split out of test_agent.c
 * to keep each test source under the 2000-line hard limit. Independent binary
 * (unit-test-agent-repair) so it links the same agent objects without sharing
 * test_agent.c's translation unit. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "aimee.h"
#include "cJSON.h"
#include "agent_protocol.h"

static cJSON *make_msg(const char *role, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", role);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

/* Helper: make an OpenAI assistant message with tool_calls */
static cJSON *make_assistant_with_tools_openai(const char **ids, const char **names, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddNullToObject(msg, "content");
   cJSON *tcs = cJSON_AddArrayToObject(msg, "tool_calls");
   for (int i = 0; i < count; i++)
   {
      cJSON *tc = cJSON_CreateObject();
      cJSON_AddStringToObject(tc, "id", ids[i]);
      cJSON_AddStringToObject(tc, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(tc, "function");
      cJSON_AddStringToObject(fn, "name", names[i]);
      cJSON_AddStringToObject(fn, "arguments", "{}");
      cJSON_AddItemToArray(tcs, tc);
   }
   return msg;
}

/* Helper: make an OpenAI tool result message */
static cJSON *make_tool_result_openai(const char *tool_call_id, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "tool");
   cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

/* Helper: make an Anthropic assistant message with tool_use blocks */
static cJSON *make_assistant_with_tools_anthropic(const char **ids, const char **names, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON *content = cJSON_AddArrayToObject(msg, "content");
   for (int i = 0; i < count; i++)
   {
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "tool_use");
      cJSON_AddStringToObject(block, "id", ids[i]);
      cJSON_AddStringToObject(block, "name", names[i]);
      cJSON_AddItemToObject(block, "input", cJSON_CreateObject());
      cJSON_AddItemToArray(content, block);
   }
   return msg;
}

/* Helper: make an Anthropic user message with tool_result blocks */
static cJSON *make_tool_results_anthropic(const char **ids, const char **contents, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "user");
   cJSON *content = cJSON_AddArrayToObject(msg, "content");
   for (int i = 0; i < count; i++)
   {
      cJSON *tr = cJSON_CreateObject();
      cJSON_AddStringToObject(tr, "type", "tool_result");
      cJSON_AddStringToObject(tr, "tool_use_id", ids[i]);
      cJSON_AddStringToObject(tr, "content", contents[i]);
      cJSON_AddItemToArray(content, tr);
   }
   return msg;
}

static void test_repair_empty(void)
{
   assert(message_history_repair(NULL) == 0);
   cJSON *arr = cJSON_CreateArray();
   assert(message_history_repair(arr) == 0);
   cJSON_Delete(arr);
}

static void test_repair_no_tools(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("system", "You are helpful."));
   cJSON_AddItemToArray(arr, make_msg("user", "Hello"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "Hi there!"));
   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_repair_consistent_openai(void)
{
   /* Complete tool call cycle — should not be modified */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"call_1"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_1", "file1.txt\nfile2.txt"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "Found 2 files."));

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 4);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_call_openai(void)
{
   /* Assistant made a tool call but result is missing (simulating crash mid-execution) */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"call_orphan"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));
   /* No tool result! */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 3);

   /* Verify the synthetic result was inserted */
   cJSON *last = cJSON_GetArrayItem(arr, 2);
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(last, "role"));
   assert(strcmp(role, "tool") == 0);
   const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(last, "tool_call_id"));
   assert(strcmp(tcid, "call_orphan") == 0);
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(last, "content"));
   assert(strstr(content, "outcome is unknown") != NULL);

   cJSON_Delete(arr);
}

static void test_repair_orphaned_result_openai(void)
{
   /* Tool result exists but no matching call (state corruption) */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_ghost", "some result"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "done"));

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 2); /* orphaned result removed */

   cJSON_Delete(arr);
}

static void test_repair_multiple_orphans_openai(void)
{
   /* Two tool calls, only one has a result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "do stuff"));
   const char *ids[] = {"call_a", "call_b"};
   const char *names[] = {"bash", "read_file"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 2));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_a", "result_a"));
   /* call_b has no result */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);

   /* Should now have: user, assistant, tool(call_a), tool(call_b synthetic) */
   int found_b = 0;
   cJSON *m;
   cJSON_ArrayForEach(m, arr)
   {
      const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "tool_call_id"));
      if (tcid && strcmp(tcid, "call_b") == 0)
      {
         found_b = 1;
         const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
         assert(strstr(c, "outcome is unknown") != NULL);
      }
   }
   assert(found_b == 1);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_call_anthropic(void)
{
   /* Anthropic format: tool_use block with no matching tool_result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"toolu_orphan"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   /* No user message with tool_result */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);

   /* Should have added a user message with tool_result */
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON *last = cJSON_GetArrayItem(arr, 2);
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(last, "role"));
   assert(strcmp(role, "user") == 0);

   cJSON *content = cJSON_GetObjectItem(last, "content");
   assert(cJSON_IsArray(content));
   cJSON *tr = cJSON_GetArrayItem(content, 0);
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "type"));
   assert(strcmp(type, "tool_result") == 0);
   const char *tuid = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "tool_use_id"));
   assert(strcmp(tuid, "toolu_orphan") == 0);

   cJSON_Delete(arr);
}

static void test_repair_consistent_anthropic(void)
{
   /* Complete Anthropic tool cycle — should not be modified */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"toolu_1"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   const char *rids[] = {"toolu_1"};
   const char *rcontents[] = {"file1.txt"};
   cJSON_AddItemToArray(arr, make_tool_results_anthropic(rids, rcontents, 1));

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_result_anthropic(void)
{
   /* Anthropic: tool_result with no matching tool_use */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   /* Add an assistant with a tool_use to establish Anthropic format detection */
   const char *ids[] = {"toolu_real"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   /* User message with both a valid and an orphaned tool_result */
   const char *rids[] = {"toolu_real", "toolu_ghost"};
   const char *rcontents[] = {"ok", "orphan"};
   cJSON_AddItemToArray(arr, make_tool_results_anthropic(rids, rcontents, 2));

   int repairs = message_history_repair(arr);
   assert(repairs == 1); /* orphaned toolu_ghost removed */

   /* The user message should still exist with one tool_result */
   cJSON *user_msg = cJSON_GetArrayItem(arr, 2);
   cJSON *content = cJSON_GetObjectItem(user_msg, "content");
   assert(cJSON_GetArraySize(content) == 1);
   const char *tuid =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "tool_use_id"));
   assert(strcmp(tuid, "toolu_real") == 0);

   cJSON_Delete(arr);
}

static void test_repair_responses_api(void)
{
   /* Responses API: function_call with no function_call_output */
   cJSON *arr = cJSON_CreateArray();

   cJSON *fc = cJSON_CreateObject();
   cJSON_AddStringToObject(fc, "type", "function_call");
   cJSON_AddStringToObject(fc, "call_id", "fc_orphan");
   cJSON_AddStringToObject(fc, "name", "bash");
   cJSON_AddStringToObject(fc, "arguments", "{}");
   cJSON_AddItemToArray(arr, fc);

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 2);

   cJSON *output = cJSON_GetArrayItem(arr, 1);
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(output, "type"));
   assert(strcmp(type, "function_call_output") == 0);
   const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(output, "call_id"));
   assert(strcmp(cid, "fc_orphan") == 0);

   cJSON_Delete(arr);
}

static void test_repair_idempotent(void)
{
   /* Repair should be idempotent — running twice gives same result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "do stuff"));
   const char *ids[] = {"call_x"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));

   assert(message_history_repair(arr) == 1);
   int size_after_first = cJSON_GetArraySize(arr);

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == size_after_first);

   cJSON_Delete(arr);
}

int main(void)
{
   test_repair_empty();
   test_repair_no_tools();
   test_repair_consistent_openai();
   test_repair_orphaned_call_openai();
   test_repair_orphaned_result_openai();
   test_repair_multiple_orphans_openai();
   test_repair_orphaned_call_anthropic();
   test_repair_consistent_anthropic();
   test_repair_orphaned_result_anthropic();
   test_repair_responses_api();
   test_repair_idempotent();
   printf("agent_repair: all tests passed\n");
   return 0;
}
