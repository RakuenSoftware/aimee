#include "server_mcp_learning.h"
#include "aimee.h"
#include "db1_client/db1.h"
#include "kb_client.h"
#include <stdlib.h>
#include <string.h>

static cJSON *text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

cJSON *server_mcp_tool_rules_propose(cJSON *args)
{
   cJSON *jtext = cJSON_GetObjectItemCaseSensitive(args, "text");
   cJSON *jreason = cJSON_GetObjectItemCaseSensitive(args, "reason");
   if (!cJSON_IsString(jtext) || !jtext->valuestring[0])
      return text_content("error: missing 'text' parameter");

   int id = kb_client_collab_rules_propose(
       jtext->valuestring, cJSON_IsString(jreason) ? jreason->valuestring : "", "agent");
   if (id < 0)
      return text_content("error: could not propose rule");

   char buf[128];
   snprintf(buf, sizeof(buf), "Rule proposed as #%d", id);
   return text_content(buf);
}

cJSON *server_mcp_tool_rules_list(void)
{
   char *json = kb_client_collab_rules_list_json();
   if (!json)
      return text_content("[]");
   cJSON *content = text_content(json);
   free(json);
   return content;
}

cJSON *server_mcp_tool_learning_propose(cJSON *args)
{
   cJSON *jsig = cJSON_GetObjectItemCaseSensitive(args, "signal_type");
   if (!cJSON_IsString(jsig) || !jsig->valuestring[0])
      return text_content("error: missing 'signal_type' parameter");

   char *envelope = kb_client_learning_propose_signal_json(args);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return text_content("error: failed to record learning signal");
   }
   cJSON *dispatch = cJSON_GetObjectItemCaseSensitive(resp, "dispatch");
   char *json = NULL;
   if (cJSON_IsObject(dispatch))
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, dispatch);
      json = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content = text_content(json ? json : "{\"signal_id\":0}");
   free(json);
   return content;
}

cJSON *server_mcp_tool_learning_review(cJSON *args)
{
   cJSON *jstate = cJSON_GetObjectItemCaseSensitive(args, "state");
   cJSON *jsink = cJSON_GetObjectItemCaseSensitive(args, "sink");
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(args, "limit");
   const char *state =
       cJSON_IsString(jstate) && jstate->valuestring[0] ? jstate->valuestring : "pending";
   const char *sink = cJSON_IsString(jsink) && jsink->valuestring[0] ? jsink->valuestring : NULL;
   int limit = cJSON_IsNumber(jlimit) ? jlimit->valueint : 20;

   char *json = kb_client_learning_list_proposals_json(state, sink, limit);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return text_content("error: failed to list learning proposals");

   cJSON *proposals = cJSON_GetObjectItemCaseSensitive(resp, "proposals");
   if (!cJSON_IsArray(proposals))
   {
      cJSON_Delete(resp);
      return text_content("[]");
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, proposals);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   cJSON *content = text_content(out ? out : "[]");
   free(out);
   return content;
}
