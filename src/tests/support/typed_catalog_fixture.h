/* Explicit external-format fixture adapters. Production owns canonical tools. */
#ifndef AIMEE_TYPED_CATALOG_FIXTURE_H
#define AIMEE_TYPED_CATALOG_FIXTURE_H
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <aimee/tools/agent_tools.h>
#include <aimee/translation/aimee_frontend.h>
static inline int fixture_catalog_import(const cJSON *array, aimee_wire_t wire,
                                         aimee_request_t *out)
{
   if (!array)
      return 0;
   if (!cJSON_IsArray(array))
      return -1;
   out->n_tools = cJSON_GetArraySize(array);
   if (out->n_tools > AIMEE_IR_MAX_BLOCKS)
   {
      out->n_tools = 0;
      return -1;
   }
   out->tools = out->n_tools ? calloc((size_t)out->n_tools, sizeof *out->tools) : NULL;
   if (out->n_tools && !out->tools)
   {
      out->n_tools = 0;
      return -1;
   }
   for (int i = 0; i < out->n_tools; ++i)
      if (wire == AIMEE_WIRE_ANTHROPIC
              ? anthropic_ir_parse_tool(cJSON_GetArrayItem(array, i), out->tools + i)
              : openai_ir_parse_tool(cJSON_GetArrayItem(array, i), out->tools + i,
                                     wire == AIMEE_WIRE_OPENAI_CHAT))
      {
         aimee_request_free(out);
         return -1;
      }
   return 0;
}
static inline cJSON *fixture_catalog_render(const aimee_request_t *catalog, aimee_wire_t wire)
{
   cJSON *array = cJSON_CreateArray();
   assert(array);
   for (int i = 0; i < catalog->n_tools; ++i)
   {
      const aimee_tool_t *t = catalog->tools + i;
      cJSON *item;
      if (wire == AIMEE_WIRE_ANTHROPIC)
      {
         item = cJSON_CreateObject();
         assert(item);
         assert(cJSON_AddStringToObject(item, "name", t->name));
         assert(cJSON_AddStringToObject(item, "description", t->description ? t->description : ""));
         assert(cJSON_AddItemToObject(item, "input_schema", cJSON_Duplicate(t->schema, 1)));
      }
      else
         item = openai_ir_render_tool(t, wire == AIMEE_WIRE_RESPONSES);
      assert(item && cJSON_AddItemToArray(array, item));
   }
   return array;
}
static inline cJSON *fixture_catalog_wire(aimee_wire_t wire)
{
   aimee_request_t catalog = {0};
   assert(!agent_tools_catalog(wire, &catalog));
   cJSON *array = fixture_catalog_render(&catalog, wire);
   aimee_request_free(&catalog);
   return array;
}
static inline cJSON *fixture_chat_tools(void)
{
   return fixture_catalog_wire(AIMEE_WIRE_OPENAI_CHAT);
}
static inline cJSON *fixture_responses_tools(void)
{
   return fixture_catalog_wire(AIMEE_WIRE_RESPONSES);
}
static inline cJSON *fixture_anthropic_tools(void)
{
   return fixture_catalog_wire(AIMEE_WIRE_ANTHROPIC);
}
static inline void fixture_filter_tools(cJSON *array, const char *role)
{
   cJSON *first = cJSON_GetArrayItem(array, 0);
   aimee_wire_t wire = cJSON_HasObjectItem(first, "function")       ? AIMEE_WIRE_OPENAI_CHAT
                       : cJSON_HasObjectItem(first, "input_schema") ? AIMEE_WIRE_ANTHROPIC
                                                                    : AIMEE_WIRE_RESPONSES;
   aimee_request_t catalog = {0};
   assert(!fixture_catalog_import(array, wire, &catalog));
   assert(!agent_tools_catalog_filter(&catalog, role));
   cJSON *rendered = fixture_catalog_render(&catalog, wire);
   aimee_request_free(&catalog);
   while (cJSON_GetArraySize(array))
      cJSON_DeleteItemFromArray(array, 0);
   while (cJSON_GetArraySize(rendered))
      assert(cJSON_AddItemToArray(array, cJSON_DetachItemFromArray(rendered, 0)));
   cJSON_Delete(rendered);
}
#endif
