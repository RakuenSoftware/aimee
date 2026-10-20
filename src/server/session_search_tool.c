#include "headers/session_search_tool.h"
#include "db1_client/primary_sessions.h"
#include <stdio.h>
#include <stdlib.h>

cJSON *session_search_tool_result_for_principal(const cJSON *args, const char *principal)
{
   cJSON *request = cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
   cJSON *result = NULL;
   char *wire = NULL, *reply = NULL;
   if (!request || !principal || !principal[0])
      goto unavailable;
   /* Private-looking caller arguments are still caller data. Remove every
    * duplicate before installing the authenticated transport principal. */
   while (cJSON_GetObjectItemCaseSensitive(request, "_principal"))
      cJSON_DeleteItemFromObjectCaseSensitive(request, "_principal");
   if (!cJSON_AddStringToObject(request, "_principal", principal))
      goto unavailable;
   wire = cJSON_PrintUnformatted(request);
   reply = wire ? db1_session_ir_search(principal, wire) : NULL;
   result = reply ? cJSON_Parse(reply) : NULL;
   if (cJSON_IsObject(result))
      goto done;
   cJSON_Delete(result);
   result = NULL;
unavailable:
   result = cJSON_CreateObject();
   if (result)
      cJSON_AddStringToObject(result, "error", "owned canonical session search unavailable");
done:
   free(wire);
   free(reply);
   cJSON_Delete(request);
   return result;
}

cJSON *session_search_tool_result_for_uid(cJSON *args, unsigned uid)
{
   char principal[32];
   snprintf(principal, sizeof principal, "uid:%u", uid);
   return session_search_tool_result_for_principal(args, principal);
}

cJSON *session_search_mcp_tool(void)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", "session_search");
   cJSON_AddStringToObject(
       t, "description",
       "Search owned canonical session conversations. Default current view includes imported "
       "history; use view=observations for original messages removed by compaction. Preserve view "
       "and source_revision when scrolling.");
   cJSON *schema = cJSON_Parse(
       "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"session_"
       "id\":{\"type\":\"string\"},\"around_message_id\":{\"type\":\"integer\"},"
       "\"window\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"offset\":{\"type\":"
       "\"integer\",\"minimum\":0},\"include_"
       "sources\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"view\":{\"type\":"
       "\"string\",\"enum\":[\"current\",\"observations\"]},\"source_revision\":{\"type\":"
       "\"string\",\"description\":\"Exact source_revision returned for this session and view; "
       "include with scroll to reject source changes.\"}}}");
   cJSON_AddItemToObject(t, "inputSchema", schema ? schema : cJSON_CreateObject());
   return t;
}
