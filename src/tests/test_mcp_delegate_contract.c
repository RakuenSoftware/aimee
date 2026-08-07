/* The delegate tool must let its caller ask for tools.
 *
 * `mcp__aimee__delegate` advertised exactly {role, prompt, branch, cwd, persona}
 * -- no way to say "this one needs a filesystem" -- while the CLI had `--tools`
 * all along. Tools were then granted by role, and `code` was not on the list.
 *
 * Measured: a `code` packet delegated over MCP returned a per-file diff summary
 * for files that were never written. With no file tools and no way to request
 * them, narrating the work is the only thing the agent can still do, and the
 * result reads exactly like success. The caller found out by searching the disk
 * for a filename the report claimed to have created.
 *
 * Two halves fix it and both must hold: the role default (see
 * test_delegate_role.c) and this -- the parameter reaching the model at all. A
 * server that honors `tools` while the schema hides it is the same bug wearing
 * a fix.
 *
 * The other tool families drag in most of the server; stub them so this links
 * against the schema alone, following test_mcp_roundtable_contract.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "aimee/protocols/mcp/mcp_tools.h"

void mcp_add_discovery_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_extended_tools(cJSON *tools)
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
cJSON *mcp_client_registry_build_namespaced_tools(int timeout_ms)
{
   (void)timeout_ms;
   return NULL;
}

static cJSON *schema_for(const char *tool_name)
{
   cJSON *tools = mcp_build_tools_list_flat();
   assert(tools != NULL);
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, tool_name) == 0)
         return cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   }
   return NULL;
}

int main(void)
{
   printf("mcp_delegate_contract: ");
   cJSON *schema = schema_for("delegate");
   assert(schema != NULL);

   const cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
   assert(props != NULL);

   /* The parameter the CLI has always had. */
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive(props, "tools");
   assert(tools != NULL);

   /* Typed, so a caller passing `false` is understood as a deliberate
    * text-only run rather than dropped as an unparseable value. */
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(tools, "type");
   assert(cJSON_IsString(type) && strcmp(type->valuestring, "boolean") == 0);

   /* Optional: omitting it must leave the role's own default in charge, which
    * is what makes a `code` delegate work without the caller knowing to ask. */
   const cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, req)
   {
      assert(!(cJSON_IsString(item) && strcmp(item->valuestring, "tools") == 0));
   }

   printf("ok\n");
   return 0;
}
