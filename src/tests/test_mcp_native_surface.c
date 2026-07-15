/* test_mcp_native_surface.c: the MCP-derived native tool surface.
 *
 * aimee's MCP table is the single source of truth for which tools exist; entries
 * marked native are registered at startup so aimee's OWN agents get them too. This
 * pins the half that only the server could exercise before: that a registered tool
 * is ADVERTISED to the model, carrying the tool's own MCP schema.
 *
 * That half is exactly where this class of bug has always hidden. git_commit was
 * MCP-only, so a delegate's only route to land work was shelling out to `git` --
 * the thing require_aimee_git forbids. index_find_callers was MCP-only, so a review
 * panel asked "is this still called?" hedged on a symbol with twelve callers one
 * query away. Both read correctly and both shipped green; both were caught by
 * running a delegate on hardware. A registry that resolves in a toolset but never
 * reaches the model's tools array fails the same silent way.
 *
 * The provider is faked so this needs no server: the point under test is aimee's
 * plumbing (advertise / schema / filter), not any one MCP handler. */
#include "aimee.h" /* MAX_PATH_LEN, via agent_types.h */
#include "agent_tools.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_TOOL "index_find_callers"

static int g_advert_calls;

/* Stands in for the real MCP tools/list entry: {"name","description","inputSchema"}. */
static cJSON *fake_advert(const char *tool)
{
   g_advert_calls++;
   if (!tool || strcmp(tool, FAKE_TOOL) != 0)
      return NULL;
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", FAKE_TOOL);
   cJSON_AddStringToObject(t, "description", "Find the call sites of a symbol.");
   cJSON *schema = cJSON_AddObjectToObject(t, "inputSchema");
   cJSON_AddStringToObject(schema, "type", "object");
   cJSON *props = cJSON_AddObjectToObject(schema, "properties");
   cJSON *sym = cJSON_AddObjectToObject(props, "symbol");
   cJSON_AddStringToObject(sym, "type", "string");
   return t;
}

static cJSON *fake_call(const char *tool, cJSON *args, const char *sid)
{
   (void)args;
   (void)sid;
   cJSON *content = cJSON_CreateArray();
   cJSON *block = cJSON_CreateObject();
   cJSON_AddStringToObject(block, "type", "text");
   cJSON_AddStringToObject(block, "text", tool);
   cJSON_AddItemToArray(content, block);
   return content;
}

/* Find a tool in an OpenAI Chat-format tools array: {type,function:{name,...}}. */
static cJSON *find_chat_tool(cJSON *tools, const char *name)
{
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
      cJSON *n = cJSON_GetObjectItemCaseSensitive(fn, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         return fn;
   }
   return NULL;
}

/* Unregistered — a thin client or any binary without the server tier — must not
 * advertise a tool that has no implementation behind it. An agent offered a tool
 * that always errors is worse than one that was never offered it. */
static void test_unregistered_advertises_nothing(void)
{
   agent_tools_set_mcp_provider(NULL, NULL);
   assert(!agent_tools_is_mcp_derived(FAKE_TOOL));
   cJSON *tools = build_tools_array();
   assert(find_chat_tool(tools, FAKE_TOOL) == NULL);
   cJSON_Delete(tools);
   printf("  PASS: unregistered_advertises_nothing\n");
}

/* The registration reaches the model's tools array, carrying the tool's OWN schema.
 *
 * Reusing the MCP schema rather than hand-writing a native one is the point: a
 * second hand-written schema is how git_commit came to advertise add_all and
 * set_upstream, parameters its handler had never accepted. */
static void test_registered_tool_is_advertised_with_its_mcp_schema(void)
{
   agent_tools_set_mcp_provider(fake_call, fake_advert);
   agent_tools_register_mcp_tool(FAKE_TOOL);
   assert(agent_tools_is_mcp_derived(FAKE_TOOL));

   cJSON *tools = build_tools_array();
   cJSON *fn = find_chat_tool(tools, FAKE_TOOL);
   assert(fn != NULL);

   cJSON *desc = cJSON_GetObjectItemCaseSensitive(fn, "description");
   assert(cJSON_IsString(desc) && strstr(desc->valuestring, "call sites") != NULL);

   /* The schema must arrive intact — not an empty object. A parameterless
    * git_commit would be called wrong on the first turn. */
   cJSON *params = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
   assert(params != NULL);
   cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
   assert(props && cJSON_GetObjectItemCaseSensitive(props, "symbol") != NULL);
   cJSON_Delete(tools);
   printf("  PASS: registered_tool_is_advertised_with_its_mcp_schema\n");
}

/* Both native surfaces, not just the one that happened to be wired. The Responses
 * API shape is flat (name/description/parameters at the top level) rather than
 * nested under "function" — a tool present in one and missing from the other is
 * invisible to whichever providers use that shape. */
static void test_advertised_on_the_responses_surface_too(void)
{
   agent_tools_set_mcp_provider(fake_call, fake_advert);
   agent_tools_register_mcp_tool(FAKE_TOOL);

   cJSON *tools = build_tools_array_responses();
   int found = 0;
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, FAKE_TOOL) == 0)
      {
         assert(cJSON_GetObjectItemCaseSensitive(t, "parameters") != NULL);
         found = 1;
      }
   }
   assert(found);
   cJSON_Delete(tools);
   printf("  PASS: advertised_on_the_responses_surface_too\n");
}

/* Registration is idempotent, and a tool with no usable advert is skipped rather
 * than offered with an empty schema. */
static void test_registration_is_idempotent_and_bad_advert_is_skipped(void)
{
   agent_tools_set_mcp_provider(fake_call, fake_advert);
   agent_tools_register_mcp_tool(FAKE_TOOL);
   agent_tools_register_mcp_tool(FAKE_TOOL);
   agent_tools_register_mcp_tool(FAKE_TOOL);

   cJSON *tools = build_tools_array();
   int count = 0;
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
      cJSON *n = cJSON_GetObjectItemCaseSensitive(fn, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, FAKE_TOOL) == 0)
         count++;
   }
   assert(count == 1); /* three registrations, advertised once */
   cJSON_Delete(tools);

   /* fake_advert returns NULL for anything else: registered, but unadvertisable. */
   agent_tools_register_mcp_tool("tool_the_provider_does_not_know");
   tools = build_tools_array();
   assert(find_chat_tool(tools, "tool_the_provider_does_not_know") == NULL);
   cJSON_Delete(tools);
   printf("  PASS: registration_is_idempotent_and_bad_advert_is_skipped\n");
}

int main(void)
{
   printf("test_mcp_native_surface:\n");
   /* Order matters: the unregistered case must run before anything registers,
    * since the registry is process-global (the server registers once at startup). */
   test_unregistered_advertises_nothing();
   test_registered_tool_is_advertised_with_its_mcp_schema();
   test_advertised_on_the_responses_surface_too();
   test_registration_is_idempotent_and_bad_advert_is_skipped();
   assert(g_advert_calls > 0); /* the schema really came from the provider */
   printf("All mcp_native_surface tests passed.\n");
   return 0;
}
