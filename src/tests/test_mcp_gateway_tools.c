/* test_mcp_gateway_tools.c: unit tests for the ask_user MCP tool. */
#include "headers/server_mcp_gateway.h"
#include <assert.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
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

static const char *first_text(cJSON *result)
{
   assert(cJSON_IsObject(result));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "status")), "ok"));
   assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   if (!content || !cJSON_IsArray(content))
      return NULL;
   cJSON *item = cJSON_GetArrayItem(content, 0);
   if (!item)
      return NULL;
   cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "text");
   return cJSON_IsString(t) ? t->valuestring : NULL;
}

/* ---- send_message is retired with the delivery gateway ---- */

static void test_send_message_retired(void)
{
   cJSON *args = make_args("target", "ntfy:homelab", "text", "hello");
   assert(mcp_gateway_tool_dispatch("send_message", args) == NULL);
   cJSON_Delete(args);
   PASS("send_message_retired");
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
   assert(!strcmp(
       text, "1. staging\n2. production\n3. Other (type your answer)\n\nWhich environment?\n"));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "isError")));
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
   assert(!strcmp(text, "What is your name?\n(Type your answer)"));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "isError")));
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
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   cJSON_Delete(result);
   cJSON_Delete(args);
   PASS("ask_user_missing_question");
}

static char *long_utf8(const char *prefix, size_t repeats)
{
   size_t n = strlen(prefix), bytes = n + repeats * 3;
   char *text = malloc(bytes + 1);
   assert(text);
   memcpy(text, prefix, n);
   for (size_t i = 0; i < repeats; i++)
      memcpy(text + n + i * 3, "雪", 3);
   text[bytes] = 0;
   return text;
}
static void test_long_question_and_choices(void)
{
   char *question = long_utf8("question:", 6000);
   char *labels[4];
   cJSON *args = make_args("question", question, NULL, NULL), *choices = cJSON_CreateArray();
   for (int i = 0; i < 4; i++)
   {
      char prefix[] = "choice0:";
      prefix[6] = (char)('1' + i);
      labels[i] = long_utf8(prefix, 3000 + (size_t)i);
      assert(cJSON_AddItemToArray(choices, cJSON_CreateString(labels[i])));
   }
   assert(cJSON_AddItemToArray(choices, cJSON_CreateString("fifth choice is not rendered")));
   assert(cJSON_AddItemToObject(args, "choices", choices));
   cJSON *result = mcp_gateway_tool_dispatch("ask_user", args);
   const char *text = first_text(result), *cursor = text;
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   for (int i = 0; i < 4; i++)
   {
      assert(cursor[0] == '1' + i && cursor[1] == '.' && cursor[2] == ' ');
      cursor += 3;
      size_t n = strlen(labels[i]);
      assert(!memcmp(cursor, labels[i], n) && cursor[n] == '\n');
      cursor += n + 1;
      free(labels[i]);
   }
   const char *other = "5. Other (type your answer)\n\n";
   assert(!strncmp(cursor, other, strlen(other)));
   cursor += strlen(other);
   assert(!memcmp(cursor, question, strlen(question)) && !strcmp(cursor + strlen(question), "\n"));
   assert(!strstr(text, "fifth choice is not rendered") && strlen(text) > 50000);
   cJSON_Delete(result);
   cJSON_Delete(args);
   args = make_args("question", question, NULL, NULL);
   result = mcp_gateway_tool_dispatch("ask_user", args);
   text = first_text(result);
   assert(!memcmp(text, question, strlen(question)) &&
          !strcmp(text + strlen(question), "\n(Type your answer)"));
   cJSON_Delete(result);
   cJSON_Delete(args);
   free(question);
   PASS("long UTF8 question and four choices retained exactly");
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

int main(void)
{
   printf("test_mcp_gateway_tools\n");
   test_send_message_retired();
   test_ask_user_with_choices();
   test_ask_user_open_ended();
   test_ask_user_missing_question();
   test_long_question_and_choices();
   test_unknown_tool_returns_null();
   printf("All tests passed.\n");
   return 0;
}
