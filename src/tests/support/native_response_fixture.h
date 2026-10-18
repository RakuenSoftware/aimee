/* Wire fixtures for ingress tests. These produce native frames, never substitute
 * for the production decoder or policy implementation. */
#ifndef AIMEE_TEST_NATIVE_RESPONSE_FIXTURE_H
#define AIMEE_TEST_NATIVE_RESPONSE_FIXTURE_H
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *native_response_fixture(const char *wire, const char *text, const char *calls_json,
                                     const char *stop)
{
   int anthropic = !wire || !strcmp(wire, "anthropic");
   int responses = wire && !strcmp(wire, "chatgpt");
   cJSON *calls = calls_json ? cJSON_Parse(calls_json) : cJSON_CreateArray();
   assert(cJSON_IsArray(calls));
   int count = cJSON_GetArraySize(calls);
   cJSON *root = cJSON_CreateObject(), *parts = cJSON_CreateArray();
   assert(root && parts);
   if (anthropic)
   {
      cJSON_AddStringToObject(root, "role", "assistant");
      cJSON_AddItemToObject(root, "content", parts);
      cJSON_AddStringToObject(root, "stop_reason", stop ? stop : count ? "tool_use" : "end_turn");
   }
   else if (responses)
   {
      cJSON_AddStringToObject(root, "status", "completed");
      cJSON_AddItemToObject(root, "output", parts);
   }
   else
   {
      cJSON *choices = cJSON_AddArrayToObject(root, "choices");
      cJSON *choice = cJSON_CreateObject();
      cJSON_AddItemToArray(choices, choice);
      cJSON *message = cJSON_AddObjectToObject(choice, "message");
      cJSON_AddStringToObject(message, "role", "assistant");
      cJSON_AddStringToObject(message, "content", text ? text : "");
      cJSON_AddItemToObject(message, "tool_calls", parts);
      const char *reason = !stop                       ? (count ? "tool_calls" : "stop")
                           : !strcmp(stop, "tool_use") ? "tool_calls"
                           : !strcmp(stop, "end_turn") ? "stop"
                                                       : stop;
      cJSON_AddStringToObject(choice, "finish_reason", reason);
   }
   if ((anthropic || responses) && text && text[0])
   {
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", anthropic ? "text" : "output_text");
      cJSON_AddStringToObject(block, "text", text);
      if (responses)
      {
         cJSON *message = cJSON_CreateObject(),
               *content = cJSON_AddArrayToObject(message, "content");
         cJSON_AddStringToObject(message, "type", "message");
         cJSON_AddStringToObject(message, "role", "assistant");
         cJSON_AddItemToArray(content, block);
         cJSON_AddItemToArray(parts, message);
      }
      else
         cJSON_AddItemToArray(parts, block);
   }
   const cJSON *call;
   cJSON_ArrayForEach(call, calls)
   {
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "id"));
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "name"));
      const char *args = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call, "arguments"));
      cJSON *item = cJSON_CreateObject();
      assert(id && name && args && item);
      cJSON_AddStringToObject(item, "type",
                              anthropic   ? "tool_use"
                              : responses ? "function_call"
                                          : "function");
      cJSON_AddStringToObject(item, responses ? "call_id" : "id", id);
      if (anthropic)
      {
         cJSON_AddStringToObject(item, "name", name);
         cJSON *input = cJSON_Parse(args);
         assert(input);
         cJSON_AddItemToObject(item, "input", input);
      }
      else
      {
         cJSON *function = responses ? item : cJSON_AddObjectToObject(item, "function");
         cJSON_AddStringToObject(function, "name", name);
         cJSON_AddStringToObject(function, "arguments", args);
      }
      cJSON_AddItemToArray(parts, item);
   }
   char *json = cJSON_PrintUnformatted(root);
   assert(json);
   cJSON_Delete(root);
   cJSON_Delete(calls);
   if (!responses)
      return json;
   size_t capacity = strlen(json) + 100;
   char *frame = malloc(capacity);
   assert(frame);
   snprintf(
       frame, capacity,
       "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":%s}\n\n",
       json);
   free(json);
   return frame;
}
#endif
