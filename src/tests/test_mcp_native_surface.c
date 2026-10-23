#include "support/typed_catalog_fixture.h"
#include "aimee_sha256.h"
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
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Only external catalogue owners are substituted; canonical construction/filtering
 * and all admission are the actual production implementation below. */
static const char *local_adverts, *federated_adverts;
static int local_lookups, federated_lookups;
static cJSON *catalog_local_adverts(int timeout)
{
   assert(timeout == 1000);
   ++local_lookups;
   return local_adverts ? cJSON_Parse(local_adverts) : NULL;
}
static char *catalog_federated_adverts(void)
{
   ++federated_lookups;
   return federated_adverts ? strdup(federated_adverts) : NULL;
}
#define mcp_client_registry_build_namespaced_tools catalog_local_adverts
#define kb_client_tool_registry_snapshot_json      catalog_federated_adverts
#include "../server/agent_tools.c"
#undef mcp_client_registry_build_namespaced_tools
#undef kb_client_tool_registry_snapshot_json

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
   cJSON *tools = fixture_chat_tools();
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

   cJSON *tools = fixture_chat_tools();
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

   cJSON *tools = fixture_responses_tools();
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

   cJSON *tools = fixture_chat_tools();
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
   tools = fixture_chat_tools();
   assert(find_chat_tool(tools, "tool_the_provider_does_not_know") == NULL);
   cJSON_Delete(tools);
   printf("  PASS: registration_is_idempotent_and_bad_advert_is_skipped\n");
}

/* Golden bytes captured from the admitted pre-cutover owner at 6e59415b9:
 * builtins only, git unavailable, no MCP/plugin/KB advertisements. This pins
 * each exposure mask, ordered definitions and every schema/description byte. */
static void test_pre_cutover_catalog_vectors(void)
{
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES,
                                 AIMEE_WIRE_ANTHROPIC};
   const int counts[] = {31, 30, 31};
   const char *hashes[] = {"170635e60a128d3a4bb843c0b71195cb37649d0058611f2b226e87c1216f666e",
                           "b5d46bf887ed46897678367c0327529f4c9c86f4e6cf49338a0611c7085dd9e1",
                           "6ef67c6a1c123c5493a32d87b661933ccab4eeeec32119db1ccdd7102f133500"};
   for (int i = 0; i < 3; ++i)
   {
      cJSON *array = fixture_catalog_wire(wires[i]);
      assert(cJSON_GetArraySize(array) == counts[i]);
      char *json = cJSON_PrintUnformatted(array), hash[65];
      assert(json);
      assert(!aimee_sha256_hex(json, strlen(json), hash));
      assert(!strcmp(hash, hashes[i]));
      free(json);
      cJSON_Delete(array);
   }
   puts("pre-cutover native catalogue bytes unchanged across three exposure profiles OK");
}

static void *catalog_oom(size_t size)
{
   (void)size;
   return NULL;
}
static void test_catalog_oom_and_typed_schema_rewrite(void)
{
   aimee_request_t catalog = {.model = strdup("last-admitted")};
   cJSON_Hooks hooks = {.malloc_fn = catalog_oom, .free_fn = free};
   cJSON_InitHooks(&hooks);
   assert(agent_tools_catalog(AIMEE_WIRE_OPENAI_CHAT, &catalog) == -1);
   cJSON_InitHooks(NULL);
   assert(!strcmp(catalog.model, "last-admitted") && !catalog.n_tools);
   aimee_request_free(&catalog);
   catalog = (aimee_request_t){.tools = calloc(1, sizeof *catalog.tools), .n_tools = 1};
   assert(catalog.tools);
   catalog.tools[0] = (aimee_tool_t){
       .name = strdup("fixture"),
       .schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\","
                             "\"format\":\"date\"}},\"additionalProperties\":false}")};
   assert(!agent_tools_catalog_sanitize(&catalog, "openai"));
   assert(cJSON_HasObjectItem(catalog.tools[0].schema, "additionalProperties"));
   assert(!agent_tools_catalog_sanitize(&catalog, "ollama"));
   assert(!cJSON_HasObjectItem(catalog.tools[0].schema, "additionalProperties"));
   assert(!cJSON_HasObjectItem(
       cJSON_GetObjectItemCaseSensitive(
           cJSON_GetObjectItemCaseSensitive(catalog.tools[0].schema, "properties"), "date"),
       "format"));
   assert(!strcmp(catalog.tools[0].name, "fixture") && !aimee_ir_request_admit(&catalog));
   aimee_request_free(&catalog);
   puts("canonical catalogue allocation failure is atomic; typed provider schema rewrites preserve "
        "identity OK");
}

static void test_canonical_owner_precedence_and_direct_mcp(void)
{
   local_adverts = "[{\"name\":\"plugin:echo\",\"description\":\"local-one\",\"inputSchema\":{"
                   "\"type\":\"object\"}},"
                   "{\"name\":\"plugin:echo\",\"description\":\"local-two\",\"inputSchema\":{"
                   "\"type\":\"object\"}}]";
   federated_adverts = "{\"mcp_tools\":[{\"name\":\"plugin:echo\",\"description\":\"foreign\","
                       "\"inputSchema\":{\"type\":\"object\"}},"
                       "{\"name\":\"federation:read\",\"description\":\"retained\",\"inputSchema\":"
                       "{\"type\":\"object\"}}]}";
   for (int w = 0; w < 3; ++w)
   {
      aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES, AIMEE_WIRE_ANTHROPIC};
      aimee_request_t catalog = {0};
      assert(!agent_tools_catalog(wires[w], &catalog));
      int count = 0;
      for (int i = 0; i < catalog.n_tools; ++i)
         if (!strcmp(catalog.tools[i].name, "plugin:echo"))
         {
            assert(!strncmp(catalog.tools[i].description, "local-", 6));
            ++count;
         }
      assert(count == 2 && !aimee_ir_find_function(&catalog, NULL, "plugin:echo"));
      assert(aimee_ir_find_function(&catalog, NULL, "federation:read"));
      aimee_tool_t duplicate = {.name = strdup("respond"),
                                .schema = cJSON_Parse("{\"type\":\"object\"}")};
      catalog.tools = realloc(catalog.tools, (size_t)(catalog.n_tools + 2) * sizeof *catalog.tools);
      assert(catalog.tools);
      catalog.tools[catalog.n_tools++] = duplicate;
      duplicate =
          (aimee_tool_t){.name = strdup("respond"), .schema = cJSON_Parse("{\"type\":\"object\"}")};
      catalog.tools[catalog.n_tools++] = duplicate;
      assert(!agent_tools_catalog_respond(&catalog)); /* any existing name, even ambiguous */
      aimee_request_free(&catalog);
   }
   local_adverts = "[{\"name\":\"bad:tool\",\"inputSchema\":[]}]";
   aimee_request_t retained = {.model = strdup("sentinel")};
   assert(agent_tools_catalog(AIMEE_WIRE_OPENAI_CHAT, &retained) == -1);
   assert(!strcmp(retained.model, "sentinel") && !retained.n_tools);
   int local_before = local_lookups, federated_before = federated_lookups;
   aimee_tool_t mcp = {0};
   assert(!agent_tools_execution_definition(FAKE_TOOL, &mcp));
   assert(!strcmp(mcp.name, FAKE_TOOL) && local_lookups == local_before &&
          federated_lookups == federated_before);
   aimee_tool_free(&mcp);
   aimee_request_free(&retained);
   local_adverts = federated_adverts = NULL;
   puts("canonical owner: local precedence, ambiguous identity, atomic failure, independent MCP "
        "lookup OK");
}

int main(void)
{
   printf("test_mcp_native_surface:\n");
   /* Order matters: the unregistered case must run before anything registers,
    * since the registry is process-global (the server registers once at startup). */
   test_catalog_oom_and_typed_schema_rewrite();
   test_pre_cutover_catalog_vectors();
   test_unregistered_advertises_nothing();
   test_registered_tool_is_advertised_with_its_mcp_schema();
   test_advertised_on_the_responses_surface_too();
   test_registration_is_idempotent_and_bad_advert_is_skipped();
   test_canonical_owner_precedence_and_direct_mcp();
   assert(g_advert_calls > 0); /* the schema really came from the provider */
   printf("All mcp_native_surface tests passed.\n");
   return 0;
}
