/* tool_call_args.c: sanitization of assistant tool_call arguments.
 *
 * Strict OpenAI-compatible providers (MiniMax-M2.7, in particular) reject
 * requests whose assistant tool_calls[].function.arguments is missing, a
 * JSON object, or an unparseable JSON string with HTTP 400
 * "invalid params, invalid function arguments json string". These helpers
 * coerce every persisted/echoed tool_call to a valid JSON-encoded string. */
#include "tool_call_args.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

void tool_call_normalize_assistant_arguments(cJSON *assistant_message, int index,
                                             const char *arguments)
{
   if (!assistant_message || !arguments)
      return;
   cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(assistant_message, "tool_calls");
   cJSON *tc = cJSON_IsArray(tool_calls) ? cJSON_GetArrayItem(tool_calls, index) : NULL;
   cJSON *fn = tc ? cJSON_GetObjectItemCaseSensitive(tc, "function") : NULL;
   if (!fn)
      return;
   cJSON *replacement = cJSON_CreateString(arguments);
   if (!replacement)
      return;
   if (cJSON_GetObjectItemCaseSensitive(fn, "arguments"))
   {
      if (!cJSON_ReplaceItemInObjectCaseSensitive(fn, "arguments", replacement))
         cJSON_Delete(replacement);
   }
   else
   {
      cJSON_AddItemToObject(fn, "arguments", replacement);
   }
}

void tool_call_sanitize_assistant_arguments(cJSON *assistant_message)
{
   if (!assistant_message)
      return;
   cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(assistant_message, "tool_calls");
   if (!cJSON_IsArray(tool_calls))
      return;
   cJSON *tc;
   cJSON_ArrayForEach(tc, tool_calls)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
      if (!fn)
         continue;
      cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
      int ok = 0;
      if (args && cJSON_IsString(args) && args->valuestring)
      {
         cJSON *check = cJSON_Parse(args->valuestring);
         if (check)
         {
            cJSON_Delete(check);
            ok = 1;
         }
      }
      if (ok)
         continue;
      if (args)
         cJSON_DeleteItemFromObjectCaseSensitive(fn, "arguments");
      cJSON_AddStringToObject(fn, "arguments", "{}");
   }
}
