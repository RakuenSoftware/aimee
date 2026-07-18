/* mcp_tools_gateway.c: MCP tool registrations for the ambient-presence gateway. */
#include "mcp_tools_gateway.h"
#include <cJSON.h>
#include <string.h>

static cJSON *build_tool(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}

void mcp_add_gateway_tools(cJSON *tools)
{
   /* send_message: send text to a delivery target without waiting for a reply */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *tgt = cJSON_AddObjectToObject(p, "target");
      cJSON_AddStringToObject(tgt, "type", "string");
      cJSON_AddStringToObject(tgt, "description", "Delivery target spec or 'origin'");
      cJSON *txt = cJSON_AddObjectToObject(p, "text");
      cJSON_AddStringToObject(txt, "type", "string");
      cJSON_AddStringToObject(txt, "description", "Message text to send");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("target"));
      cJSON_AddItemToArray(req, cJSON_CreateString("text"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool(
              "send_message",
              "Send a text message to a delivery target (telegram:<chat_id>, ntfy:<topic>, "
              "webhook:<url>, or origin) without waiting for a reply. Use to deliver findings "
              "to an unattended operator.",
              s));
   }

   /* ask_user: present a question to the operator on the originating channel */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "question");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "The question to ask");
      cJSON *c = cJSON_AddObjectToObject(p, "choices");
      cJSON_AddStringToObject(c, "type", "array");
      cJSON *items = cJSON_CreateObject();
      cJSON_AddStringToObject(items, "type", "string");
      cJSON_AddItemToObject(c, "items", items);
      cJSON_AddNumberToObject(c, "maxItems", 4);
      cJSON_AddStringToObject(c, "description",
                              "Up to 4 choices. If omitted, the question is open-ended.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("question"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("ask_user",
                     "Present a question to the operator on the originating messaging channel. "
                     "Formats up to 4 numbered choices; an implicit 'Other (type your answer)' is "
                     "always appended. The agent's turn ends after delivery; the operator's next "
                     "message on that channel is the answer.",
                     s));
   }
}