/* Actual flat and family-collapsed catalogs under a generic host capability
 * filter; the peer below selects capabilities, never native data authority. */
#include "cJSON.h"
#include <aimee/protocols/mcp/mcp_tools.h>
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int selected, malformed_schema;
int aimee_module_tool_input_schema(const char *name, const cJSON *original, cJSON **out)
{
   *out = NULL;
   if (!selected || (strcmp(name, "memory_serve") && strcmp(name, "memory_claim_card") &&
                     strcmp(name, "memory")))
      return 0;
   if (malformed_schema)
   {
      *out = cJSON_CreateString("invalid");
      return 1;
   }
   *out = cJSON_Duplicate(original, 1);
   assert(*out);
   cJSON *properties = cJSON_GetObjectItemCaseSensitive(*out, "properties");
   cJSON_DeleteItemFromObjectCaseSensitive(properties, "store");
   cJSON_AddItemToObject(properties, "store",
                         cJSON_Parse("{\"type\":\"string\",\"enum\":[\"user\"]}"));
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(*out, "required"),
                        cJSON_CreateString("store"));
   return 1;
}
int aimee_module_tool_source_mode(const char *name)
{
   if (!selected)
      return 0;
   if (!strcmp(name, "memory_serve") || !strcmp(name, "memory_claim_card"))
      return 1;
   return !strncmp(name, "memory_", 7) || !strcmp(name, "mutate") || !strcmp(name, "list_facts")
              ? -1
              : 0;
}
void mcp_add_discovery_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_skill_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_gateway_tools(cJSON *tools)
{
   (void)tools;
}
cJSON *session_search_mcp_tool(void)
{
   return NULL;
}
cJSON *mcp_client_registry_build_namespaced_tools(int timeout)
{
   (void)timeout;
   return NULL;
}
void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}
static const cJSON *named(const cJSON *tools, const char *name)
{
   const cJSON *tool;
   cJSON_ArrayForEach(tool, tools) if (!strcmp(cJSON_GetStringValue(
                                                   cJSON_GetObjectItemCaseSensitive(tool, "name")),
                                               name)) return tool;
   return NULL;
}
static int command(const cJSON *family, const char *name)
{
   const cJSON *schema = cJSON_GetObjectItemCaseSensitive(family, "inputSchema");
   const cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   const cJSON *field = cJSON_GetObjectItemCaseSensitive(props, "command");
   const cJSON *items = cJSON_GetObjectItemCaseSensitive(field, "enum"), *item;
   cJSON_ArrayForEach(item,
                      items) if (cJSON_IsString(item) && !strcmp(item->valuestring, name)) return 1;
   return 0;
}
static void schema_contract(const cJSON *tool)
{
   const cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema"),
               *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties"),
               *store = cJSON_GetObjectItemCaseSensitive(properties, "store");
   if (!selected)
      return;
   const cJSON *values = cJSON_GetObjectItemCaseSensitive(store, "enum");
   assert(cJSON_GetArraySize(values) == 1 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(values, 0)), "user"));
   const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required"), *item;
   int found = 0;
   cJSON_ArrayForEach(item, required) if (cJSON_IsString(item) &&
                                          !strcmp(item->valuestring, "store")) found++;
   assert(found == 1 && !cJSON_GetObjectItemCaseSensitive(store, "default"));
}
int main(void)
{
   for (selected = 0; selected <= 1; selected++)
   {
      cJSON *flat = mcp_build_tools_list_flat();
      assert(flat && named(flat, "memory_serve") && named(flat, "memory_claim_card"));
      schema_contract(named(flat, "memory_serve"));
      schema_contract(named(flat, "memory_claim_card"));
      assert(named(flat, "delegate"));
      assert((named(flat, "memory_get") != NULL) == !selected);
      assert((named(flat, "list_facts") != NULL) == !selected);
      assert((named(flat, "mutate") != NULL) == !selected);
      cJSON_Delete(flat);
      cJSON *collapsed = mcp_build_tools_list();
      const cJSON *memory = named(collapsed, "memory");
      assert(memory && command(memory, "serve") && command(memory, "claim_card"));
      schema_contract(memory);
      assert(command(memory, "get") == !selected && command(memory, "list") == !selected &&
             command(memory, "mutate") == !selected);
      cJSON_Delete(collapsed);
   }
   selected = 1;
   malformed_schema = 1;
   cJSON *invalid = mcp_build_tools_list_flat();
   assert(!named(invalid, "memory_serve") && !named(invalid, "memory_claim_card") &&
          named(invalid, "delegate"));
   cJSON_Delete(invalid);
   puts("MCP source discovery: legacy preserved, selected denied tools absent from flat and "
        "collapsed enums");
   return 0;
}
