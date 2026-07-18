/* test_mcp_gateway_tools.c: unit tests for send_message and ask_user MCP tools. */
#include "mcp_tools_gateway.h"
#include "headers/server_mcp_gateway.h"
#include <assert.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("  PASS: %s\n", name)

/* ---- helpers ---- */

static cJSON *make_args(const char *key1, const char *val1, const char *key2, const char *val2)
{
   cJSON *j = cJSON_CreateObject();
   if (key1 && val1)
      cJSON_AddStringToObject(j, key1, val1);
   if (key2 && val2)
      cJSON_AddStringToObject(j, key2, val2);
   return j;
}

static const char *first_text(cJSON *content)
{
   if (!content || !cJSON_IsArray(content))
      return NULL;
   cJSON *item = cJSON_GetArrayItem(content, 0);
   if (!item)
      return NULL;
   cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "text");
   return cJSON_IsString(t) ? t->valuestring : NULL;
}

/* ---- send_message tests ---- */

static void test_send_message_valid(void)
{
   cJSON *args = make_args("target", "ntfy:homelab", "text", "hello");
   cJSON *result = mcp_gateway_tool_dispatch("send_message", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strncmp(text, "sent:", 5) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("send_message_valid");
}

static void test_send_message_missing_text(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "target", "ntfy:foo");
   cJSON *result = mcp_gateway_tool_dispatch("send_message", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strncmp(text, "error:", 6) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("send_message_missing_text");
}

static void test_send_message_missing_target(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "text", "hello");
   cJSON *result = mcp_gateway_tool_dispatch("send_message", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strncmp(text, "error:", 6) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("send_message_missing_target");
}

/* ---- ask_user tests ---- */

static void test_ask_user_with_choices(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "question", "Which environment?");
   cJSON *choices = cJSON_CreateArray();
   cJSON_AddItemToArray(choices, cJSON_CreateString("staging"));
   cJSON_AddItemToArray(choices, cJSON_CreateString("production"));
   cJSON_AddItemToObject(args, "choices", choices);

   cJSON *result = mcp_gateway_tool_dispatch("ask_user", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strstr(text, "1.") != NULL);
   assert(strstr(text, "staging") != NULL);
   assert(strstr(text, "Other") != NULL);
   assert(strstr(text, "Which environment?") != NULL);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("ask_user_with_choices");
}

static void test_ask_user_open_ended(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "question", "What is your name?");
   cJSON *result = mcp_gateway_tool_dispatch("ask_user", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strstr(text, "What is your name?") != NULL);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("ask_user_open_ended");
}

static void test_ask_user_missing_question(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *result = mcp_gateway_tool_dispatch("ask_user", args);
   assert(result != NULL);
   const char *text = first_text(result);
   assert(text != NULL);
   assert(strncmp(text, "error:", 6) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("ask_user_missing_question");
}

/* ---- unknown tool ---- */

static void test_unknown_tool_returns_null(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *result = mcp_gateway_tool_dispatch("no_such_tool", args);
   assert(result == NULL);
   cJSON_Delete(args);
   PASS("unknown_tool_returns_null");
}

/* ---- mcp_add_gateway_tools registers 2 tools ---- */

static void test_gateway_tools_count(void)
{
   cJSON *tools = cJSON_CreateArray();
   int before = cJSON_GetArraySize(tools);
   mcp_add_gateway_tools(tools);
   int after = cJSON_GetArraySize(tools);
   assert(after - before == 2);
   cJSON_Delete(tools);
   PASS("gateway_tools_count");
}

int main(void)
{
   printf("test_mcp_gateway_tools\n");
   test_send_message_valid();
   test_send_message_missing_text();
   test_send_message_missing_target();
   test_ask_user_with_choices();
   test_ask_user_open_ended();
   test_ask_user_missing_question();
   test_unknown_tool_returns_null();
   test_gateway_tools_count();
   printf("All tests passed.\n");
   return 0;
}
