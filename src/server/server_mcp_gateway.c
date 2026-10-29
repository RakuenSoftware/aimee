/* ask_user MCP results: format a question for the requesting client to present. */
#include "server_mcp_gateway.h"
#include <cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Complete MCP tool result; transport success is independent of tool failure. */
static cJSON *tool_result(const char *text, int is_error)
{
   cJSON *result = cJSON_CreateObject(), *content = cJSON_CreateArray(),
         *item = cJSON_CreateObject();
   if (!result || !content || !item || !cJSON_AddStringToObject(result, "status", "ok") ||
       !cJSON_AddBoolToObject(result, "isError", is_error) ||
       !cJSON_AddStringToObject(item, "type", "text") ||
       !cJSON_AddStringToObject(item, "text", text ? text : ""))
      goto failed;
   if (!cJSON_AddItemToArray(content, item))
      goto failed;
   item = NULL;
   if (!cJSON_AddItemToObject(result, "content", content))
      goto failed;
   return result;
failed:
   cJSON_Delete(item);
   cJSON_Delete(content);
   cJSON_Delete(result);
   return NULL;
}
static int reserve_text(size_t *size, const char *text)
{
   size_t n = strlen(text);
   if (n > SIZE_MAX - *size)
      return -1;
   *size += n;
   return 0;
}
static void copy_text(char **cursor, const char *text)
{
   size_t n = strlen(text);
   memcpy(*cursor, text, n);
   *cursor += n;
}
/* Size before copying. No fixed buffer, UTF8 truncation or snprintf cursor. */
static char *format_question(const char *question, const cJSON *choices)
{
   int count = cJSON_IsArray(choices) ? cJSON_GetArraySize(choices) : 0;
   if (count > 4)
      count = 4;
   const char *labels[4] = {0};
   const char *tail = count ? ". Other (type your answer)\n\n" : "\n(Type your answer)";
   size_t size = 1;
   if (reserve_text(&size, question) || reserve_text(&size, tail))
      return NULL;
   if (count)
   {
      if (reserve_text(&size, "1\n"))
         return NULL;
      for (int i = 0; i < count; i++)
      {
         labels[i] = cJSON_GetStringValue(cJSON_GetArrayItem(choices, i));
         if (labels[i] && (reserve_text(&size, "1. \n") || reserve_text(&size, labels[i])))
            return NULL;
      }
   }
   char *text = malloc(size), *cursor = text;
   if (!text)
      return NULL;
   if (count)
   {
      for (int i = 0; i < count; i++)
         if (labels[i])
         {
            *cursor++ = (char)('1' + i);
            copy_text(&cursor, ". ");
            copy_text(&cursor, labels[i]);
            *cursor++ = '\n';
         }
      *cursor++ = (char)('1' + count);
      copy_text(&cursor, tail);
      copy_text(&cursor, question);
      *cursor++ = '\n';
   }
   else
   {
      copy_text(&cursor, question);
      copy_text(&cursor, tail);
   }
   *cursor = 0;
   return text;
}
cJSON *mcp_gateway_tool_dispatch(const char *tool, cJSON *jargs)
{
   if (!tool || !jargs)
      return NULL;
   if (!strcmp(tool, "ask_user"))
   {
      const char *question =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(jargs, "question"));
      if (!question || !question[0])
         return tool_result("error: question required", 1);
      char *text = format_question(question, cJSON_GetObjectItemCaseSensitive(jargs, "choices"));
      if (!text)
         return tool_result("error: question formatting unavailable", 1);
      cJSON *result = tool_result(text, 0);
      free(text);
      return result;
   }
   return NULL;
}
